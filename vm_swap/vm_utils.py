import json
import socket
import subprocess
import time


class CmdRecord:
    def __init__(self, cmd, node, t0, t1, result):
        self.ts_start = t0
        self.duration = t1 - t0
        self.cmd = cmd
        self.node = node
        self.r = result
        self.rc = result.returncode
        self.stdout = str(result.stdout) if result.stdout is not None else ""
        self.stderr = str(result.stderr) if result.stderr is not None else ""

    def __str__(self) -> str:
        s = f"{self.ts_start}: [{self.node}] {self.cmd}"
        if self.stdout:
            s += f"\nstdout: {self.stdout}"
        if self.stderr:
            s += f"\nstderr: {self.stderr}"
        return s + "\n"


def run_command(cmd, timeout=80, node=-1):
    t0 = time.monotonic()
    result = subprocess.run(
        cmd, shell=True, stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=timeout
    )
    t1 = time.monotonic()
    return CmdRecord(cmd, node, t0, t1, result)


class QMPClient:
    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.sock = None
        self.file = None

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.socket_path)
        self.file = self.sock.makefile(
            mode='rw', buffering=1, encoding='utf-8')

        # QMP greeting
        self._read_line()

        self.execute("qmp_capabilities")

    def _read_line(self):
        return json.loads(self.file.readline())

    def execute(self, command: str, arguments: dict = None):
        cmd = {"execute": command}
        if arguments:
            cmd["arguments"] = arguments

        self.file.write(json.dumps(cmd) + "\n")
        self.file.flush()

        # stop/cont/migrate-incoming emit an event before the ACK.
        if command in ("cont", "stop", "migrate-incoming"):
            responses = [self._read_line(), self._read_line()]
            if "return" in responses[0]:
                response = responses[0]
            elif "return" in responses[1]:
                response = responses[1]
            else:
                print(responses)
                raise ValueError()
        else:
            response = self._read_line()

        if "error" in response:
            raise Exception(f"QMP Error: {response['error']['desc']}")

        return response.get("return")

    def close(self):
        if self.file:
            self.file.close()
        if self.sock:
            self.sock.close()
