import json
import os
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def _available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _process_is_running(pid: int) -> bool:
    status = subprocess.run(
        ["ps", "-o", "stat=", "-p", str(pid)],
        capture_output=True,
        text=True,
        check=False,
    )
    process_state = status.stdout.strip()
    return (
        status.returncode == 0
        and bool(process_state)
        and not process_state.startswith("Z")
    )


class ReplayServiceLifecycleTest(unittest.TestCase):
    def test_start_receipt_background_process_and_stop(self):
        with tempfile.TemporaryDirectory() as temporary:
            runtime_dir = Path(temporary)
            state_dir = runtime_dir / "state"
            replay_dir = runtime_dir / "replay"
            replay_dir.mkdir()
            log_file = runtime_dir / "replay.log"
            port = _available_port()
            config_file = runtime_dir / "client.yaml"
            config_file.write_text(
                "viz:\n"
                f"  output_dir: {replay_dir}\n"
                f"  server_port: {port}\n",
                encoding="utf-8",
            )
            environment = {
                **os.environ,
                "RL_REPLAY_STATE_DIR": str(state_dir),
                "RL_REPLAY_LOG_FILE": str(log_file),
            }
            command = [
                "bash",
                "./run_replay.sh",
                "--config",
                str(config_file),
                "--host",
                "127.0.0.1",
                "--validation-id",
                "replay-lifecycle-test",
            ]
            state_file = state_dir / "service.json"
            started_pid = None
            try:
                started = subprocess.run(
                    command,
                    cwd=REPOSITORY_ROOT,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=25,
                    check=False,
                )
                self.assertEqual(
                    started.returncode,
                    0,
                    msg=f"stdout={started.stdout!r} stderr={started.stderr!r}",
                )
                start_lines = started.stdout.strip().splitlines()
                self.assertEqual(len(start_lines), 1)
                self.assertTrue(start_lines[0].startswith("[Replay] started "))
                self.assertEqual(started.stderr, "")

                state = json.loads(state_file.read_text(encoding="utf-8"))
                started_pid = int(state["pid"])
                self.assertEqual(state["status"], "running")
                self.assertEqual(int(state["port"]), port)
                os.kill(started_pid, 0)

                stopped = subprocess.run(
                    ["bash", "./run_replay.sh", "-stop"],
                    cwd=REPOSITORY_ROOT,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=15,
                    check=False,
                )
                self.assertEqual(
                    stopped.returncode,
                    0,
                    msg=f"stdout={stopped.stdout!r} stderr={stopped.stderr!r}",
                )
                self.assertEqual(len(stopped.stdout.strip().splitlines()), 1)
                self.assertTrue(stopped.stdout.startswith("[Replay] stopped "))
                self.assertEqual(stopped.stderr, "")
                self.assertFalse(state_file.exists())
                self.assertFalse(_process_is_running(started_pid))
                started_pid = None
            finally:
                if state_file.exists():
                    subprocess.run(
                        ["bash", "./run_replay.sh", "-stop"],
                        cwd=REPOSITORY_ROOT,
                        env=environment,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=15,
                        check=False,
                    )
                if started_pid is not None:
                    try:
                        os.kill(started_pid, 15)
                    except ProcessLookupError:
                        pass
