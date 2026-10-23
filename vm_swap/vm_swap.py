#!/usr/bin/env python3
"""vm-swap test harness for this QEMU tree.

Logic mirrors EmuFork/vm_swap.py: boot a template VM, snapshot it, fork N
children from the shared memory file, then exercise pause/resume rounds.
In swap mode each forked VM gets -dirty-save, and --dirty-save-quota can cap
how many dirty pages are written out on pause.

Run it from a directory containing base.qcow2 and id_rsa (the same working
directory layout EmuFork/vm_swap.py expects).
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import shlex
import time

from virtcontainer import (QEMUTemplate, QEMUChild, QEMU_DEFAULT_CONFIG,
                           TapManager, QEMU_MEM_GB, SMP)
from vm_utils import run_command

QEMU_PATH = "/opt/qemu-exp/bin"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_DIR = os.path.join(SCRIPT_DIR, "agent")
DIRTIER_DIR = os.path.join(SCRIPT_DIR, "dirtier")
DIRTIER_SOURCE = os.path.join(DIRTIER_DIR, "dirtier.c")
DIRTIER_PATH = os.path.join(DIRTIER_DIR, "dirtier")

# Binaries the fork flow needs at runtime, mapped to their sources.
AGENT_BINARIES = {
    os.path.join(AGENT_DIR, "client_qemu"):
        os.path.join(AGENT_DIR, "client_qemu.cc"),
    os.path.join(AGENT_DIR, "server_qemu"):
        os.path.join(AGENT_DIR, "server_qemu.cc"),
}


def _ensure_binaries(binaries, make_dir, label):
    """Build binaries in make_dir when missing, non-executable or stale."""
    stale = []
    for binary, source in binaries.items():
        if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
            stale.append(binary)
        elif os.path.getmtime(source) > os.path.getmtime(binary):
            stale.append(binary)

    if not stale:
        return

    print(f"building {label} binaries in {make_dir} "
          f"(missing or stale: {', '.join(os.path.basename(b) for b in stale)})")
    rec = run_command(f"make -C {shlex.quote(make_dir)}")
    if rec.rc != 0:
        raise SystemExit(f"failed to build {label} binaries:\n{rec}")

    for binary in binaries:
        if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
            raise SystemExit(f"{label} build did not produce {binary}")


def ensure_agent_binaries():
    _ensure_binaries(AGENT_BINARIES, AGENT_DIR, "agent")


def ensure_dirtier_binary():
    _ensure_binaries({DIRTIER_PATH: DIRTIER_SOURCE}, DIRTIER_DIR, "dirtier")



# Number of pause/resume cycles to run on the forked VMs before waiting for
# user input.  Adjust here to change how many rounds run.
PAUSE_RESUME_ROUNDS = 4
DEBUG = False


def size_to_bytes(value):
    """Convert a size string like '512M' to bytes for QMP.

    QMP's QAPI 'size' type takes a JSON number, while the QEMU command line
    accepts suffixes; the harness uses one user-facing value for both.
    """
    if value is None or isinstance(value, int):
        return value
    text = value.strip().lower()
    units = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30, "t": 1 << 40}
    mult = 1
    if text and text[-1] in units:
        mult = units[text[-1]]
        text = text[:-1]
    return int(text) * mult


class QEMUSwapTemplate(QEMUTemplate):
    def __init__(self, idx, output_dir, config_dict, file_back_mem=True):
        super().__init__(idx, output_dir, config_dict, file_back_mem)

    def start(self):
        qemu_cmd = f"nohup {QEMU_PATH}/qemu-system-x86_64 \
                    -enable-kvm \
                    -m {QEMU_MEM_GB}G \
                    -smp {SMP} \
                    -nographic \
                    -serial mon:stdio \
                    -drive file={self.disk_image_path},format=qcow2,if=virtio \
                    {self.mem_option} \
                    -netdev tap,id=net0,ifname={self.network_dev},script=no,downscript=no \
                    -device virtio-net-pci,netdev=net0 \
                        -device vhost-vsock-pci,id=vhost-vsock0,guest-cid={self.vsock_cid} \
                    -qmp unix:{self.api_sock()},server,nowait < /dev/null > {self.work_dir()}/{self.vm_name()}.log 2>&1 &"
        records = []

        record = self._run_command(qemu_cmd)
        records.append(record)
        if record.rc != 0:
            return records

        return records


class QEMUSwapChild(QEMUChild):
    def __init__(self, idx, output_dir, mgmt_name, guest_side_ip, host_side_ip, template,
                 dirty_save_enabled=False, dirty_save_dir=None, dirty_save_quota=None):
        super().__init__(idx, output_dir, mgmt_name, guest_side_ip, host_side_ip, template)
        # dirty-save is on only when dirty_save_enabled is set.  All VMs share
        # dirty_save_dir and use dirty_<idx>.bin so the files never collide.
        self.dirty_save_enabled = dirty_save_enabled
        self.dirty_save_dir = dirty_save_dir
        self.dirty_save_quota = dirty_save_quota

    def _dirty_save_arg(self):
        """Return the QEMU -dirty-save option fragment, or '' when disabled."""
        if not self.dirty_save_enabled:
            return ""
        d = self.dirty_save_dir or self.work_dir()
        arg = f"-dirty-save dir={d}"
        if self.dirty_save_quota:
            arg += f",quota={self.dirty_save_quota}"
        arg += f",file={os.path.join(d, f'dirty_{self.idx}.bin')}"
        return arg

    def pause(self, qmp_client=None):
        return super().pause(
            qmp_client,
            enable_save_dirty=self.dirty_save_enabled,
            quota=size_to_bytes(self.dirty_save_quota))

    def create(self):
        # prepare rootfs
        records = []
        cmd = f"{QEMU_PATH}/qemu-img create -f qcow2 -b {self.template_disk_path} -F qcow2 {self.disk_image_path}"
        records.append(self._run_command(cmd))

        cmd = f"nohup {QEMU_PATH}/qemu-system-x86_64 \
                -enable-kvm \
                -m {QEMU_MEM_GB}G \
                -smp {SMP} \
                -nographic \
                -serial mon:stdio \
                -drive file={self.disk_image_path},format=qcow2,if=virtio \
                {self._dirty_save_arg()} \
                -object memory-backend-file,id=mem0,size={QEMU_MEM_GB}G,mem-path={self.vm_mem_path},share=off \
                -machine memory-backend=mem0 \
                 -netdev tap,id=net0,ifname={self.net_dev_name},script=no,downscript=no \
                -device virtio-net-pci,netdev=net0 \
                -S -incoming defer \
                -device vhost-vsock-pci,id=vhost-vsock0,guest-cid={self.vsock_cid} \
                -qmp unix:{self.api_sock()},server,nowait < /dev/null > {self.work_dir()}/{self.vm_name()}.log 2>&1 & "

        records.append(self._run_command(cmd))
        # Wait the UNIX domain socket
        time.sleep(0.1)
        return records


def read_host_memory():
    """Return (total_bytes, used_bytes) parsed from `free -b`, or None."""
    rec = run_command("LC_ALL=C free -b")
    if rec.rc != 0:
        return None
    for line in rec.stdout.splitlines():
        if line.startswith("Mem:"):
            fields = line.split()
            if len(fields) >= 3:
                try:
                    return int(fields[1]), int(fields[2])
                except ValueError:
                    break
    return None


def format_mem(nbytes):
    """Format bytes as GiB (>= 1 GiB) or MiB, preserving a negative sign."""
    if nbytes is None:
        return "n/a"
    sign = "-" if nbytes < 0 else ""
    value = abs(nbytes)
    if value >= 1 << 30:
        return f"{sign}{value / (1 << 30):.2f} GiB"
    return f"{sign}{value / (1 << 20):.1f} MiB"


def test_qemu_fork_ops(test_config: dict):
    mode = test_config["mode"]
    dirtier_bytes = test_config.get("dirtier_size", 512) * 1024 * 1024
    output_dir = test_config["output_dir"]
    dirty_save_dir = os.path.join(os.path.abspath(output_dir), "dirty_save")
    if mode == "swap" and test_config.get("dirty_save_enabled"):
        try:
            os.makedirs(dirty_save_dir, exist_ok=True)
        except OSError as e:
            raise SystemExit(
                f"cannot create dirty-save directory {dirty_save_dir}: {e}")
    if mode == "normal":
        template = QEMUTemplate(
            0, test_config["output_dir"], QEMU_DEFAULT_CONFIG)
    else:
        template = QEMUSwapTemplate(
            0, test_config["output_dir"], QEMU_DEFAULT_CONFIG)

    template.create()
    template.start()
    print("template started")
    time.sleep(15)
    result = template.run_command_on_guest("echo hello world")
    template.copy_to_guest(os.path.join(AGENT_DIR, "server_qemu"), "./")
    if test_config.get("dirtier", False):
        template.copy_to_guest(DIRTIER_PATH, "./")
    template.run_command_on_guest(
        "nohup ./server_qemu >/tmp/server.log 2>&1 &")
    template.dump_log()

    print("server started")
    time.sleep(30)
    assert result.rc == 0
    print("Start ended")
    template.pause()
    template.snapshot()
    template.shutdown(graceful=False)
    template.dump_log()
    print("template saved")

    num_vm = 4
    tap_manager = TapManager(
        num_vm=num_vm, output_dir=test_config["output_dir"])
    # We must do it
    tap_manager.init()

    childs = []

    parallel = test_config.get("parallel", False)

    # dirty-save options for forked VMs (only in swap mode).  All VMs share
    # this directory and use dirty_<idx>.bin as the snapshot file.
    swap_kwargs = {}
    if mode == "swap" and test_config.get("dirty_save_enabled"):
        swap_kwargs = {"dirty_save_enabled": True,
                       "dirty_save_dir": dirty_save_dir,
                       "dirty_save_quota": test_config.get("dirty_save_quota")}

    fork_start = time.monotonic()

    if not parallel:
        for idx in range(1, num_vm+1):
            mgmt_name, mgmt_host_ip, mgmt_guest_ip = tap_manager.get_tap_info(
                idx)

            if mode == "normal":
                child = QEMUChild(idx, output_dir=output_dir, mgmt_name=mgmt_name,
                                  guest_side_ip=mgmt_guest_ip, host_side_ip=mgmt_host_ip, template=template)
            else:
                child = QEMUSwapChild(idx, output_dir=output_dir, mgmt_name=mgmt_name,
                                      guest_side_ip=mgmt_guest_ip, host_side_ip=mgmt_host_ip, template=template,
                                      **swap_kwargs)

            childs.append(child)
            child.create()
            child.dump_log()
            child.start()

            child.run_command_on_guest("echo hello on forked vm")
            if test_config.get("dirtier", False):
                child.run_command_on_guest(f"./dirtier {dirtier_bytes}")
            child.dump_log()

    else:
        futures = []
        records = []
        with ThreadPoolExecutor() as executor:
            for idx in range(1, num_vm+1):
                mgmt_name, mgmt_host_ip, mgmt_guest_ip = tap_manager.get_tap_info(
                    idx)

                if mode == "normal":
                    child = QEMUChild(idx, output_dir=output_dir, mgmt_name=mgmt_name,
                                      guest_side_ip=mgmt_guest_ip, host_side_ip=mgmt_host_ip, template=template)
                else:
                    child = QEMUSwapChild(idx, output_dir=output_dir, mgmt_name=mgmt_name,
                                          guest_side_ip=mgmt_guest_ip, host_side_ip=mgmt_host_ip, template=template,
                                          **swap_kwargs)
                childs.append(child)
                child.set_logging(False)
                futures.append(executor.submit(child.create_and_start))

        for future in as_completed(futures):
            result = future.result()
            records += result

        with open(f"{test_config['output_dir']}/fork.log", "w") as f:
            f.writelines(str(rec) for rec in records)

        for child in childs:
            child.set_logging(True)
            rec = child.run_command_on_guest("echo hello on forked vm")
            if rec.rc != 0:
                print(child.id(), "not ok")
            if test_config.get("dirtier", False):
                child.run_command_on_guest(f"./dirtier {dirtier_bytes}")
            child.dump_log()
    fork_end = time.monotonic()
    print(f"fork time {fork_end-fork_start} s")

    # Exercise dirty-save across multiple stop/cont cycles: pause all VMs,
    # resume them, then run a command on each guest.
    input("start pause/resume rounds? \n")
    mem_rounds = []
    for rnd in range(1, PAUSE_RESUME_ROUNDS + 1):
        print(f"=== round {rnd}/{PAUSE_RESUME_ROUNDS} ===")
        before = read_host_memory()
        if before is None:
            print("host memory before pause: free failed")
        else:
            print(f"host memory before pause: {format_mem(before[1])} used "
                  f"/ {format_mem(before[0])} total")
        # Assumption: QEMU's QMP `stop` / `cont` are synchronous.  The ACK is
        # only sent after do_vm_stop() (including save_dirty_pause) / vm_start()
        # (including save_dirty_resume) have fully completed, so when these
        # calls return the pause/save or resume/restore is already done.
        round_saved_pages = 0
        round_saved_bytes = 0
        for child in childs:
            print(f"pause {child.id()}")
            stats = child.pause() or {}
            if stats:
                saved_pages = stats.get("saved-pages", 0)
                saved_bytes = stats.get("saved-bytes", 0)
                skipped_pages = stats.get("skipped-pages", 0)
                round_saved_pages += saved_pages
                round_saved_bytes += saved_bytes
                line = (f"  save-dirty: saved {saved_pages} pages "
                        f"({format_mem(saved_bytes)}), "
                        f"skipped {skipped_pages} pages")
                if stats.get("error"):
                    line += f", ERROR: {stats['error']}"
                print(line)
        if round_saved_pages or round_saved_bytes:
            print(f"  save-dirty round total: {round_saved_pages} pages "
                  f"({format_mem(round_saved_bytes)})")
        paused = read_host_memory()
        if paused is None:
            print("host memory after pause: free failed")
        elif before is None:
            print(f"host memory after pause: {format_mem(paused[1])} used")
        else:
            print(f"host memory after pause: {format_mem(paused[1])} used "
                  f"(reclaimed {format_mem(before[1] - paused[1])})")
        if DEBUG:
            input("pause done, enter to continue")

        for child in childs:
            print(f"resume {child.id()}")
            child.resume()

        resumed = read_host_memory()
        if resumed is None:
            print("host memory after resume: free failed")
        elif before is None:
            print(f"host memory after resume: {format_mem(resumed[1])} used")
        else:
            print(f"host memory after resume: {format_mem(resumed[1])} used "
                  f"(vs before pause {format_mem(resumed[1] - before[1])})")
        mem_rounds.append((before, paused, resumed))

        if DEBUG:
            input("resume done, enter to continue")
        for child in childs:
            rec = child.run_command_on_guest("echo hello world")
            print(f"echo {child.id()} rc={rec.rc}")
            child.dump_log()

    valid_rounds = [(b, p, r) for b, p, r in mem_rounds
                    if b is not None and p is not None and r is not None]
    if valid_rounds:
        n = len(valid_rounds)
        avg_before = sum(row[0][1] for row in valid_rounds) / n
        avg_paused = sum(row[1][1] for row in valid_rounds) / n
        avg_resumed = sum(row[2][1] for row in valid_rounds) / n
        print(f"=== memory summary ({n} rounds) ===")
        print(f"  avg used before pause:  {format_mem(avg_before)}")
        print(f"  avg used while paused:  {format_mem(avg_paused)}")
        print(f"  avg used after resume:  {format_mem(avg_resumed)}")
        print(f"  avg reclaimed on pause: {format_mem(avg_before - avg_paused)}")
        print(f"  avg delta after resume: {format_mem(avg_resumed - avg_before)}")

    input("child done\n")

    shutdown_start = time.monotonic()
    if not parallel:
        for child in childs:
            child: QEMUChild = child

            child.shutdown()

    else:
        futures = []
        with ThreadPoolExecutor() as executor:

            for child in childs:
                child: QEMUChild = child
                futures.append(executor.submit(
                    child.shutdown
                ))
        for future in as_completed(futures):
            future.result()

    tap_manager.cleanup()
    shutdown_end = time.monotonic()
    print(f"Shutdown time: {shutdown_end-shutdown_start} s")


def setup_tap():
    run_command(
        "ip link del tap0; ip tuntap add dev tap0 mode tap; ip link set tap0 up; ip addr add 172.16.0.1/30 dev tap0")


def teardown_tap():
    run_command("ip link del tap0")


if __name__ == "__main__":
    parser = argparse.ArgumentParser("vm_swap")

    parser.add_argument("--mode", type=str, default="normal")
    parser.add_argument("--dirtier", action="store_true",
                        help="run dirtier in forked guests (default: disabled)")
    parser.add_argument("--dirtier-size", type=int, default=512,
                        help="dirtier allocation size in MiB (default: 512)")
    parser.add_argument("--dirty-save", action="store_true",
                        help="enable dirty-save page reclaim on forked VMs (swap mode; default: disabled)")
    parser.add_argument("--dirty-save-quota", type=str, default=None,
                        help="dirty-save quota, e.g. 512M (swap mode; default: unlimited)")

    args = parser.parse_args()

    assert args.mode in ["normal", "swap"]

    test_config = {"output_dir": "./vm_swap_log",
                   "parallel": True, "mode": args.mode,
                   "dirtier": args.dirtier,
                   "dirtier_size": args.dirtier_size}
    # dirty-save is only meaningful in swap mode; read its config then.
    if args.mode == "swap":
        test_config["dirty_save_enabled"] = args.dirty_save
        test_config["dirty_save_quota"] = args.dirty_save_quota

    os.makedirs(test_config["output_dir"], exist_ok=True)
    ensure_agent_binaries()
    if args.dirtier:
        ensure_dirtier_binary()
    setup_tap()
    test_qemu_fork_ops(test_config)
    teardown_tap()
