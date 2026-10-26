"""Small process and observation helpers for the local streaming lessons."""
import contextlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def output_directory(value, lesson):
    if value:
        path = Path(value).resolve()
    else:
        base = Path.home() / ".local" / "state" / "chat-stream-lessons"
        base.mkdir(parents=True, exist_ok=True)
        path = Path(tempfile.mkdtemp(prefix=f"{lesson}-", dir=base))
    if value:
        path.mkdir(parents=True, exist_ok=False)
    return path


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def append_json(path, value):
    with Path(path).open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(value, ensure_ascii=False) + "\n")


def check_ports(ports):
    with contextlib.ExitStack() as stack:
        for port in ports:
            sock = stack.enter_context(socket.socket())
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("127.0.0.1", port))


class Process:
    """Own one process group; never discovers or terminates unrelated processes."""
    def __init__(self, command, output, name, cwd=None):
        self.output = Path(output)
        self.name = name
        self.command = [str(part) for part in command]
        self.log = (self.output / f"{name}.log").open("wb")
        self.started = time.monotonic()
        self.finished = None
        try:
            self.process = subprocess.Popen(self.command, cwd=cwd, stdout=self.log,
                                            stderr=subprocess.STDOUT, start_new_session=True)
        except BaseException:
            self.log.close()
            raise
        append_json(self.output / "commands.jsonl", dict(name=name, command=self.command,
                    pid=self.process.pid, unix_ms=int(time.time() * 1000)))

    def wait(self, timeout):
        code = self.process.wait(timeout=timeout)
        self.finished = time.monotonic()
        if code:
            raise RuntimeError(f"{self.name} exited {code}; see {self.output / (self.name + '.log')}")
        return code

    def stop(self):
        requested_stop = self.process.poll() is None
        if requested_stop:
            os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=3)
        self.log.close()
        append_json(self.output / "commands.jsonl", dict(name=self.name,
                    exit_code=self.process.returncode, requested_stop=requested_stop,
                    elapsed_s=round((self.finished or time.monotonic()) - self.started, 3)))

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.stop()


def request(url, headers=None):
    started = time.monotonic()
    try:
        response = urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        body = response.read()
        return dict(status=response.status, headers=dict(response.headers.items()),
                    elapsed_ms=round((time.monotonic() - started) * 1000, 2)), body


def wait_http(url, process=None, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process is not None and process.process.poll() is not None:
            raise RuntimeError(f"{process.name} exited before listening")
        try:
            result, body = request(url)
            if result["status"] == 200:
                return body
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(.1)
    raise TimeoutError(f"not ready: {url}")
