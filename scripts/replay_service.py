#!/usr/bin/env python3
"""Start and stop one detached Replay service for a Maze Client checkout."""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import tempfile
import time
from typing import Iterator


START_TIMEOUT_SECONDS = 15.0
STOP_TIMEOUT_SECONDS = 5.0


class ReplayServiceError(RuntimeError):
    """Replay lifecycle or configuration is invalid."""


def _strip_inline_comment(line: str) -> str:
    quote = ""
    for index, character in enumerate(line):
        if quote:
            if character == quote:
                quote = ""
        elif character in ("'", '"'):
            quote = character
        elif character == "#":
            return line[:index]
    if quote:
        raise ReplayServiceError("Client config contains an unterminated quote")
    return line


def _strip_quotes(value: str) -> str:
    if not value:
        return value
    if value[0] in ("'", '"') or value[-1] in ("'", '"'):
        if len(value) < 2 or value[0] != value[-1]:
            raise ReplayServiceError("Client config contains mismatched quotes")
        return value[1:-1]
    return value


def _read_viz_config(config_argument: str) -> tuple[Path, str, str]:
    configured_path = Path(config_argument).expanduser()
    if not configured_path.is_absolute():
        configured_path = Path.cwd() / configured_path
    if configured_path.is_symlink() or not configured_path.is_file():
        raise ReplayServiceError(
            f"config must be a regular, non-symlink file: {configured_path}"
        )
    config_path = configured_path.resolve(strict=True)

    values: dict[str, str] = {}
    current_section = ""
    try:
        lines = config_path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ReplayServiceError(f"cannot read Client config: {error}") from error

    for raw_line in lines:
        line = _strip_inline_comment(raw_line)
        trimmed = line.strip()
        if not trimmed or ":" not in trimmed:
            continue
        key_part, value_part = (part.strip() for part in trimmed.split(":", 1))
        has_indent = bool(line) and line[0] in (" ", "\t")
        if not has_indent and not value_part:
            current_section = key_part
            continue
        if not has_indent or current_section != "viz":
            continue
        if key_part not in ("output_dir", "server_port"):
            continue
        if key_part in values:
            raise ReplayServiceError(f"duplicate Client config field: viz.{key_part}")
        values[key_part] = _strip_quotes(value_part)

    for key in ("output_dir", "server_port"):
        if not values.get(key):
            raise ReplayServiceError(f"missing Client config field: viz.{key}")
    return config_path, values["output_dir"], values["server_port"]


def _parse_port(value: str, source: str) -> int:
    try:
        port = int(value)
    except (TypeError, ValueError) as error:
        raise ReplayServiceError(f"{source} must be an integer port") from error
    if port <= 0 or port > 65535:
        raise ReplayServiceError(f"{source} port must be in [1, 65535]")
    return port


def _resolve_replay_config(
    config_argument: str,
    replay_dir_override: str | None,
    replay_port_override: str | None,
) -> tuple[Path, Path, int]:
    config_path, configured_dir, configured_port = _read_viz_config(
        config_argument
    )
    replay_dir_value = replay_dir_override or configured_dir
    if (not replay_dir_value or replay_dir_value != replay_dir_value.strip() or
            "\n" in replay_dir_value or "\r" in replay_dir_value):
        raise ReplayServiceError("Replay directory override is invalid")
    replay_dir = Path(replay_dir_value).expanduser()
    if not replay_dir.is_absolute():
        replay_dir = config_path.parent / replay_dir
    replay_dir = Path(os.path.abspath(os.path.normpath(replay_dir)))
    if replay_dir.exists() and (replay_dir.is_symlink() or not replay_dir.is_dir()):
        raise ReplayServiceError(
            f"Replay output must be a directory or an absent path: {replay_dir}"
        )
    replay_port = _parse_port(
        replay_port_override or configured_port,
        "Replay",
    )
    return config_path, replay_dir, replay_port


def _state_directory(repo_dir: Path) -> Path:
    configured = os.environ.get("RL_REPLAY_STATE_DIR")
    if configured:
        state_dir = Path(configured).expanduser()
        if not state_dir.is_absolute():
            raise ReplayServiceError("RL_REPLAY_STATE_DIR must be absolute")
    else:
        repo_key = hashlib.sha256(str(repo_dir).encode("utf-8")).hexdigest()[:12]
        state_dir = Path(tempfile.gettempdir()) / f"maze-client-replay-{repo_key}"
    state_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    return state_dir


@contextlib.contextmanager
def _state_lock(state_dir: Path) -> Iterator[None]:
    lock_path = state_dir / "operation.lock"
    with lock_path.open("a+", encoding="utf-8") as lock_stream:
        fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX)
        yield


def _atomic_write_json(path: Path, payload: dict[str, object]) -> None:
    temporary_path = path.with_name(f".{path.name}.{secrets.token_hex(6)}.tmp")
    try:
        with temporary_path.open("w", encoding="utf-8") as stream:
            json.dump(payload, stream, ensure_ascii=False, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            temporary_path.unlink()


def _load_state(state_file: Path) -> dict[str, object] | None:
    try:
        payload = json.loads(state_file.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except OSError as error:
        raise ReplayServiceError(
            f"cannot read Replay state {state_file}: {error}"
        ) from error
    except json.JSONDecodeError as error:
        raise ReplayServiceError(
            f"Replay state is not valid JSON: {state_file}"
        ) from error
    if not isinstance(payload, dict):
        raise ReplayServiceError(
            f"Replay state is not a JSON object: {state_file}"
        )
    return payload


def _process_is_live(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError as error:
        raise ReplayServiceError(
            f"cannot inspect Replay process {pid}: {error}"
        ) from error

    proc_stat = Path(f"/proc/{pid}/stat")
    if proc_stat.is_file():
        try:
            fields = proc_stat.read_text(encoding="utf-8").split()
            return len(fields) > 2 and fields[2] != "Z"
        except OSError as error:
            raise ReplayServiceError(
                f"cannot inspect Replay process {pid}: {error}"
            ) from error

    try:
        result = subprocess.run(
            ["ps", "-o", "stat=", "-p", str(pid)],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise ReplayServiceError(
            f"cannot inspect Replay process {pid}: {error}"
        ) from error
    if result.returncode != 0:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError as error:
            raise ReplayServiceError(
                f"cannot inspect Replay process {pid}: {error}"
            ) from error
        raise ReplayServiceError(
            f"cannot inspect Replay process {pid}: ps exited "
            f"with {result.returncode}"
        )
    status = result.stdout.strip()
    if not status:
        raise ReplayServiceError(
            f"cannot inspect Replay process {pid}: ps returned no status"
        )
    return bool(status) and not status.startswith("Z")


def _process_command(pid: int) -> tuple[str, ...]:
    proc_command = Path(f"/proc/{pid}/cmdline")
    if proc_command.is_file():
        try:
            return tuple(
                part.decode("utf-8", errors="replace")
                for part in proc_command.read_bytes().split(b"\0")
                if part
            )
        except OSError as error:
            raise ReplayServiceError(
                f"cannot read Replay process command {pid}: {error}"
            ) from error
    try:
        result = subprocess.run(
            ["ps", "-ww", "-o", "command=", "-p", str(pid)],
            check=False,
            capture_output=True,
            text=True,
        )
    except OSError as error:
        raise ReplayServiceError(
            f"cannot read Replay process command {pid}: {error}"
        ) from error
    command = result.stdout.strip()
    if result.returncode != 0 or not command:
        if not _process_is_live(pid):
            return ()
        raise ReplayServiceError(
            f"cannot read Replay process command {pid}"
        )
    return (command,)


def _owned_replay_pid(
    state: dict[str, object], server_script: Path
) -> int | None:
    pid = state.get("pid")
    instance_id = state.get("instance_id")
    if (
        not isinstance(pid, int)
        or isinstance(pid, bool)
        or pid <= 0
        or not isinstance(instance_id, str)
        or not instance_id
    ):
        raise ReplayServiceError("Replay state has no valid process identity")
    if not _process_is_live(pid):
        return None
    command = _process_command(pid)
    if not command:
        return None
    if len(command) == 1:
        matches = str(server_script) in command[0] and instance_id in command[0]
    else:
        matches = str(server_script) in command and instance_id in command
    if not matches:
        raise ReplayServiceError(
            f"Replay state PID {pid} belongs to a different process; "
            "refusing to remove the state or signal the process"
        )
    return pid


def _cleanup_state(state_file: Path, state: dict[str, object] | None) -> None:
    if state:
        ready_file = state.get("ready_file")
        if isinstance(ready_file, str):
            with contextlib.suppress(FileNotFoundError):
                Path(ready_file).unlink()
    with contextlib.suppress(FileNotFoundError):
        state_file.unlink()


def _public_url(host: str, port: int) -> str:
    display_host = "127.0.0.1" if host in ("0.0.0.0", "::") else host
    if ":" in display_host and not display_host.startswith("["):
        display_host = f"[{display_host}]"
    return f"http://{display_host}:{port}/"


def _stop_replay(repo_dir: Path) -> int:
    state_dir = _state_directory(repo_dir)
    state_file = state_dir / "service.json"
    server_script = repo_dir / "tools" / "viz_player" / "maze_viz_server.py"
    with _state_lock(state_dir):
        state = _load_state(state_file)
        if state is None:
            print("[Replay] not running")
            return 0
        pid = _owned_replay_pid(state, server_script)
        if pid is None:
            _cleanup_state(state_file, state)
            print("[Replay] not running")
            return 0

        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        except PermissionError as error:
            raise ReplayServiceError(
                f"cannot stop owned Replay process {pid}: {error}"
            ) from error

        deadline = time.monotonic() + STOP_TIMEOUT_SECONDS
        while _process_is_live(pid) and time.monotonic() < deadline:
            time.sleep(0.1)
        if _process_is_live(pid):
            _owned_replay_pid(state, server_script)
            os.kill(pid, signal.SIGKILL)
            kill_deadline = time.monotonic() + 2.0
            while _process_is_live(pid) and time.monotonic() < kill_deadline:
                time.sleep(0.1)
            if _process_is_live(pid):
                raise ReplayServiceError(f"Replay process {pid} did not stop")

        _cleanup_state(state_file, state)
        print(f"[Replay] stopped pid={pid}")
        return 0


def _validate_ready_receipt(
    ready_file: Path,
    pid: int,
    instance_id: str,
    replay_dir: Path,
    host: str,
    port: int,
) -> bool:
    try:
        receipt = json.loads(ready_file.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, json.JSONDecodeError):
        return False
    return (
        isinstance(receipt, dict)
        and receipt.get("schema_version") == 1
        and receipt.get("source") == "client-replay-ready"
        and receipt.get("pid") == pid
        and receipt.get("instance_id") == instance_id
        and receipt.get("replay_dir") == str(replay_dir)
        and receipt.get("host") == host
        and receipt.get("port") == port
    )


def _start_replay(repo_dir: Path, arguments: argparse.Namespace) -> int:
    config_path, replay_dir, replay_port = _resolve_replay_config(
        arguments.config,
        arguments.replay_dir,
        arguments.replay_port,
    )
    host = arguments.host
    validation_id = arguments.validation_id
    for name, value in (("Replay host", host), ("validation id", validation_id)):
        if not value or value != value.strip() or "\n" in value or "\r" in value:
            raise ReplayServiceError(f"{name} is invalid")

    state_dir = _state_directory(repo_dir)
    state_file = state_dir / "service.json"
    server_script = repo_dir / "tools" / "viz_player" / "maze_viz_server.py"
    if not server_script.is_file():
        raise ReplayServiceError(f"Replay server is missing: {server_script}")

    configured_log = os.environ.get("RL_REPLAY_LOG_FILE")
    log_file = Path(configured_log).expanduser() if configured_log else (
        repo_dir / "log" / "replay-server.log"
    )
    if not log_file.is_absolute():
        log_file = repo_dir / log_file
    log_file = Path(os.path.abspath(os.path.normpath(log_file)))
    log_file.parent.mkdir(parents=True, exist_ok=True)

    with _state_lock(state_dir):
        existing = _load_state(state_file)
        existing_pid = (
            _owned_replay_pid(existing, server_script)
            if existing is not None
            else None
        )
        if existing_pid is not None:
            pid = existing_pid
            existing_host = existing.get("host")
            existing_port = existing.get("port")
            existing_config = existing.get("config_path")
            existing_dir = existing.get("replay_dir")
            existing_validation = existing.get("validation_id")
            if not isinstance(existing_host, str) or not isinstance(
                existing_port, int
            ):
                raise ReplayServiceError(
                    "running Replay state is incomplete; stop it before restart"
                )
            requested_identity = (
                str(config_path),
                str(replay_dir),
                host,
                replay_port,
                validation_id,
            )
            existing_identity = (
                existing_config,
                existing_dir,
                existing_host,
                existing_port,
                existing_validation,
            )
            if existing_identity != requested_identity:
                raise ReplayServiceError(
                    "Replay is already running with different config; "
                    "run run_replay.sh -stop first"
                )
            existing_log = str(existing.get("log_file", log_file))
            print(
                f"[Replay] already running pid={pid} "
                f"url={_public_url(existing_host, existing_port)} "
                f"log={existing_log}"
            )
            return 0
        if existing is not None:
            _cleanup_state(state_file, existing)

        instance_id = secrets.token_hex(16)
        ready_file = state_dir / f"ready-{instance_id}.json"
        command = [
            sys.executable,
            "-u",
            str(server_script),
            "--dir",
            str(replay_dir),
            "--host",
            host,
            "--port",
            str(replay_port),
            "--mode",
            "evaluation",
            "--validation-id",
            validation_id,
            "--instance-id",
            instance_id,
            "--ready-file",
            str(ready_file),
        ]
        with log_file.open("ab", buffering=0) as log_stream:
            process = subprocess.Popen(
                command,
                cwd=repo_dir,
                stdin=subprocess.DEVNULL,
                stdout=log_stream,
                stderr=subprocess.STDOUT,
                close_fds=True,
                start_new_session=True,
            )

        state: dict[str, object] = {
            "schema_version": 1,
            "status": "starting",
            "pid": process.pid,
            "instance_id": instance_id,
            "repo_dir": str(repo_dir),
            "server_script": str(server_script),
            "config_path": str(config_path),
            "replay_dir": str(replay_dir),
            "host": host,
            "port": replay_port,
            "validation_id": validation_id,
            "log_file": str(log_file),
            "ready_file": str(ready_file),
            "started_at": time.time(),
        }
        _atomic_write_json(state_file, state)

        try:
            deadline = time.monotonic() + START_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                if _validate_ready_receipt(
                    ready_file,
                    process.pid,
                    instance_id,
                    replay_dir,
                    host,
                    replay_port,
                ):
                    state["status"] = "running"
                    _atomic_write_json(state_file, state)
                    print(
                        f"[Replay] started pid={process.pid} "
                        f"url={_public_url(host, replay_port)} log={log_file}"
                    )
                    return 0
                return_code = process.poll()
                if return_code is not None:
                    raise ReplayServiceError(
                        f"server exited during startup with code {return_code}; "
                        f"log={log_file}"
                    )
                time.sleep(0.1)
            raise ReplayServiceError(
                f"server readiness timed out after {START_TIMEOUT_SECONDS:.0f}s; "
                f"log={log_file}"
            )
        except BaseException:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2.0)
            _cleanup_state(state_file, state)
            raise


def _build_parser(repo_dir: Path) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="run_replay.sh",
        description=(
            "Start the evaluation Replay HTTP service in the background. "
            "Use run_replay.sh -stop to stop it."
        ),
    )
    parser.add_argument(
        "--config",
        default=os.environ.get(
            "RL_REPLAY_CONFIG", str(repo_dir / "configs" / "client_config.yaml")
        ),
        help="Client YAML config (default: configs/client_config.yaml)",
    )
    parser.add_argument(
        "--replay-dir",
        default=os.environ.get("RL_VIZ_OUTPUT_DIR"),
        help="override viz.output_dir",
    )
    parser.add_argument(
        "--replay-port",
        default=os.environ.get("RL_REPLAY_PORT"),
        help="override viz.server_port",
    )
    parser.add_argument(
        "--host",
        default=os.environ.get("RL_REPLAY_HOST", "0.0.0.0"),
        help="HTTP listen host (default: 0.0.0.0)",
    )
    parser.add_argument(
        "--validation-id",
        default=os.environ.get("RL_VALIDATION_ID", "local-validation"),
        help="validation identity reported by /api/status",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    raw_arguments = list(sys.argv[1:] if argv is None else argv)
    repo_parser = argparse.ArgumentParser(add_help=False)
    repo_parser.add_argument("--repo-dir", required=True)
    repo_arguments, remaining = repo_parser.parse_known_args(raw_arguments)
    repo_dir = Path(repo_arguments.repo_dir).resolve(strict=True)

    if remaining == ["-stop"]:
        return _stop_replay(repo_dir)
    if "-stop" in remaining:
        raise ReplayServiceError("-stop must be the only Replay argument")

    parser = _build_parser(repo_dir)
    arguments = parser.parse_args(remaining)
    return _start_replay(repo_dir, arguments)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ReplayServiceError as error:
        print(f"Replay error: {error}", file=sys.stderr)
        raise SystemExit(1)
