#!/bin/env python3
"""QEMU template/fork helpers used by vm_swap.py.

Trimmed copy of EmuFork/virtcontainer.py: only the QEMU fork path needed by
vm_swap.py is kept.
"""

from abc import ABC, abstractmethod
from concurrent.futures import ThreadPoolExecutor, as_completed
import os
import time

from vm_utils import run_command, CmdRecord, QMPClient

MINIMUM_CID = 3
SMP = 4
QEMU_MEM_GB = 4

QEMU_DEFAULT_CONFIG = {
    "network_dev": "tap0",
    "disk_image": "base.qcow2",
    "mgmt_ip": "172.16.0.2",
    "host_side_ip": "172.16.0.1"
}

# client_qemu lives next to this file so the script works from any cwd.
AGENT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "agent")


class LoggingMixin:
    def init_logging(self, log_path, record_log_name):
        self.logging_path = log_path
        self.logging_records = []
        self.logging_dump = False
        self.reocrd_log_name = record_log_name
        self.enable_log = True

    def dump_log(self):
        flag = "a"
        if not self.logging_dump:
            flag = "w"
            self.logging_dump = True
        with open(f"{self.logging_path}/{self.reocrd_log_name}", flag) as f:
            f.writelines([str(rec) for rec in self.logging_records])
            self.logging_records = []

    def append_record(self, record: CmdRecord):
        if self.enable_log:
            self.logging_records.append(record)

    def set_logging(self, enable=True):
        self.enable_log = enable


class TapManager(LoggingMixin):
    def __init__(self, num_vm, output_dir):
        self.num_vm = num_vm
        self.idx2tap = {}
        self.output_dir = output_dir
        for idx in range(1, num_vm+1):
            mgmt_host_ip, mgmt_guest_ip = self.idx2mgmt_ip(idx)
            mgmt_name = f"mgmt_{idx}"

            self.idx2tap[idx] = (
                mgmt_name, mgmt_host_ip, mgmt_guest_ip)

        self.init_logging(self.output_dir, "tap_manager.log")

    def init(self) -> bool:
        def create_mgmt_and_comm(host_ips, tap_names) -> CmdRecord:

            cmds = []
            for host_ip, tap_name in zip(host_ips, tap_names):
                cmd = [f"ip tuntap add dev {tap_name} mode tap", f"sysctl -w net.ipv6.conf.{tap_name}.disable_ipv6=1",
                       f"ip addr add {host_ip}/30 dev {tap_name}", f"ip link set {tap_name} up"]

                cmds.extend(cmd)

            cmds = ";".join(cmds)

            return run_command(cmds)

        with ThreadPoolExecutor() as executor:
            futures = []

            for idx in range(1, 1+self.num_vm):
                info = self.idx2tap[idx]
                mgmt_host_ip = info[1]

                mgmt_name = info[0]
                future = executor.submit(
                    create_mgmt_and_comm, [mgmt_host_ip], [
                        mgmt_name]
                )
                futures.append(future)

            ok = True

        for future in as_completed(futures):

            record: CmdRecord = future.result()
            if record.rc != 0:
                ok = False

            self.append_record(record)

        return ok

    def cleanup(self):
        for idx in range(1, self.num_vm+1):
            rec = run_command(f"ip link del mgmt_{idx} ")
            if rec.rc != 0:
                print(str(rec))

    def get_tap_info(self, idx):
        return self.idx2tap[idx]

    def idx2mgmt_ip(self, idx):
        offset0 = idx // 64
        offset1 = idx % 64
        host_side = f"172.18.{offset0}.{4*offset1+1}"
        guest_side = f"172.18.{offset0}.{4*offset1+2}"
        return host_side, guest_side


class Hypervisor(ABC, LoggingMixin):
    def __init__(self, idx, output_dir):
        self.output_dir = output_dir
        self.idx = idx
        assert os.path.exists(self.output_dir)

        workdir_path = os.path.join(
            self.output_dir, "hypervisor", self.vm_name())

        if not os.path.exists(workdir_path):
            os.makedirs(workdir_path, exist_ok=True)

        self.work_dir_path = os.path.abspath(workdir_path)

        self.init_logging(self.work_dir_path, "cmd.log")

    def work_dir(self):
        return self.work_dir_path

    @abstractmethod
    def create(self) -> list[CmdRecord]:
        ...

    @abstractmethod
    def start(self) -> list[CmdRecord]:
        ...

    def create_and_start(self) -> list[CmdRecord]:
        records = []
        records.extend(self.create())
        records.extend(self.start())
        return records

    def _run_command(self, cmd, timeout=80):
        rec = run_command(cmd, timeout=timeout)
        self.append_record(rec)

        return rec

    @abstractmethod
    def snapshot(self):
        ...

    @abstractmethod
    def vm_name(self):
        ...

    @abstractmethod
    def pause(self):
        pass

    @abstractmethod
    def resume(self):
        ...

    @abstractmethod
    def shutdown(self):
        pass

    @abstractmethod
    def run_command_on_guest(self, cmds):
        ...

    @abstractmethod
    def copy_to_guest(self, host_path, guest_path):
        ...

    def id(self):
        return self.idx


class QEMU(Hypervisor):
    def __init__(self, idx, output_dir, mgmt_ip, host_side_ip):
        super().__init__(idx, output_dir)
        self.mgmt_ip = mgmt_ip
        self.host_side_ip = host_side_ip
        self.vm_mem_path = None
        self.vm_state_path = None
        self.vsock_cid = self.idx + MINIMUM_CID

        self.api_sock_path = f"/tmp/qemu_{self.id()}"

    def get_qmp_client(self):
        qmp_client = QMPClient(self.api_sock())
        qmp_client.connect()
        return qmp_client

    def create(self):
        raise NotImplementedError("create")

    def start(self):
        raise NotImplementedError("start")

    def run_command_on_guest(self, cmds, quoted=True):
        if isinstance(cmds, list):
            cmds = ";".join(cmds)
        if quoted:
            guest_cmd = f"ssh -o StrictHostKeyChecking=no -i id_rsa root@{self.mgmt_ip} '{cmds}'"
        else:
            guest_cmd = f"ssh -o StrictHostKeyChecking=no -i id_rsa root@{self.mgmt_ip} {cmds}"

        return self._run_command(guest_cmd)

    def _wait_migration(self, qmp_client: QMPClient):
        for _ in range(100):
            result = qmp_client.execute("query-migrate")
            if "status" in result and result["status"] == "completed":
                return

            time.sleep(0.1)

        print("Wait migration ERROR")

    def snapshot(self):
        qmp_client = self.get_qmp_client()
        arguments = {"capabilities": [
            {"capability": "x-ignore-shared", "state": True}]}
        qmp_client.execute(
            "migrate-set-capabilities", arguments=arguments)

        arguments = {"uri": f"exec:cat>{self.vm_state_path}"}

        qmp_client.execute("migrate", arguments=arguments)

        self._wait_migration(qmp_client)
        qmp_client.close()

        return []

    def resume(self, qmp_client=None):
        need_close = True
        if qmp_client == None:
            qmp_client = self.get_qmp_client()
            need_close = False

        res = qmp_client.execute("cont")
        if need_close:
            qmp_client.close()

        return []

    def pause(self, qmp_client=None, enable_save_dirty=False, quota=None):
        need_close = False
        if qmp_client is None:
            qmp_client = self.get_qmp_client()
            need_close = True

        args = {}
        if enable_save_dirty:
            args["enable-save-dirty"] = True
            if quota:
                args["quota"] = quota

        res = qmp_client.execute("stop", arguments=args or None)
        if need_close:
            qmp_client.close()
        return res

    def copy_to_guest(self, host_path, guest_path):
        cmd = f"scp -i id_rsa {host_path} root@{self.mgmt_ip}:{guest_path}"

        return self._run_command(cmd)

    def vm_name(self):
        return f"qemu_{self.idx}"

    def api_sock(self):
        return self.api_sock_path

    def shutdown(self, graceful=True):
        if graceful:
            self.run_command_on_guest("shutdown -h now")
            return []

        qmp_client = self.get_qmp_client()
        qmp_client.execute("quit")
        qmp_client.close()
        return []


class QEMUTemplate(QEMU):
    def __init__(self, idx, output_dir, config_dict: dict, file_back_mem=True):
        assert "disk_image" in config_dict
        assert "mgmt_ip" in config_dict
        assert "host_side_ip" in config_dict
        assert "network_dev" in config_dict

        super().__init__(idx, output_dir,
                         mgmt_ip=config_dict["mgmt_ip"], host_side_ip=config_dict["host_side_ip"])

        self.disk_image_path = os.path.abspath(config_dict["disk_image"])
        self.network_dev = config_dict["network_dev"]
        self.vm_state_path = os.path.join(self.work_dir(), "vm_state")
        self.vm_mem_path = os.path.join(self.work_dir(), "vm_mem")

        if file_back_mem:
            self.mem_option = f" -object memory-backend-file,id=mem0,size={QEMU_MEM_GB}G,mem-path={self.vm_mem_path},share=on -machine memory-backend=mem0"
        else:
            self.mem_option = " "

        run_command(f"rm {self.api_sock()}")

    def create(self):
        return []

    def start(self):
        qemu_cmd = f"nohup qemu-system-x86_64 \
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


class QEMUChild(QEMU):
    def __init__(self, idx, output_dir, mgmt_name, guest_side_ip, host_side_ip, template: QEMUTemplate):
        super().__init__(idx, output_dir, guest_side_ip, host_side_ip)
        self.net_dev_name = mgmt_name
        self.vm_mem_path = template.vm_mem_path
        self.vm_state_path = template.vm_state_path
        self.template_disk_path = template.disk_image_path
        self.disk_image_path = f"{self.work_dir()}/{self.vm_name()}.qcow2"

    def create(self):
        # prepare rootfs
        records = []
        cmd = f"qemu-img create -f qcow2 -b {self.template_disk_path} -F qcow2 {self.disk_image_path}"
        records.append(self._run_command(cmd))

        cmd = f"nohup qemu-system-x86_64 \
                -enable-kvm \
                -m {QEMU_MEM_GB}G \
                -smp {SMP} \
                -nographic \
                -serial mon:stdio \
                -drive file={self.disk_image_path},format=qcow2,if=virtio \
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

    def start(self):
        qmp_client = self.get_qmp_client()

        arguments = {"capabilities": [
            {"capability": "x-ignore-shared", "state": True}]}
        qmp_client.execute(
            "migrate-set-capabilities", arguments=arguments)

        qmp_client.execute("migrate-incoming",
                           {"uri": f"exec:cat {self.vm_state_path}"})

        self._wait_migration(qmp_client)
        self.resume(qmp_client)

        qmp_client.close()
        results = []
        results.append(self._reset_mgmt())
        results.append(self._setup_mgmt_gateway())

        return results

    def _reset_mgmt(self):
        cmd = f"{AGENT_DIR}/client_qemu {self.mgmt_ip} {self.vsock_cid}"
        return self._run_command(cmd)

    def _setup_mgmt_gateway(self) -> bool:
        # TODO: modify NIC name (i.e., ens3)
        cmd = f"ip route add default via {self.host_side_ip} dev ens3"
        return self.run_command_on_guest(cmd)
