#!/usr/bin/env python3

import subprocess
import sys


def run(binary: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, *arguments],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_contract.py <maze_client>")

    help_result = run(sys.argv[1], "--help")
    if help_result.returncode != 0:
        raise AssertionError(help_result.stderr or help_result.stdout)
    required_help = (
        "--aiserver HOST:PORT      -> network.server_host/server_port",
        "--replay-dir PATH         -> viz.output_dir",
        "--replay-port PORT        -> viz.server_port",
    )
    for expected_line in required_help:
        if expected_line not in help_result.stdout:
            raise AssertionError(
                f"Client help lost config mapping: {expected_line!r}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
