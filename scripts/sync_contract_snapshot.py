#!/usr/bin/env python3

import argparse
import os
import shutil
import tempfile
from pathlib import Path


SNAPSHOT_FILES = {'proto/common/identity.proto': 'common/identity.proto',
 'cpp/proto/common/identity.pb.cc': 'common/identity.pb.cc',
 'cpp/proto/common/identity.pb.h': 'common/identity.pb.h',
 'proto/communication/session.proto': 'communication/session.proto',
 'cpp/proto/communication/session.pb.cc': 'communication/session.pb.cc',
 'cpp/proto/communication/session.pb.h': 'communication/session.pb.h',
 'proto/tasks/maze/task.proto': 'tasks/maze/task.proto',
 'cpp/proto/tasks/maze/task.pb.cc': 'tasks/maze/task.pb.cc',
 'cpp/proto/tasks/maze/task.pb.h': 'tasks/maze/task.pb.h',
 'cpp/proto/tasks/maze/task.grpc.pb.cc': 'tasks/maze/task.grpc.pb.cc',
 'cpp/proto/tasks/maze/task.grpc.pb.h': 'tasks/maze/task.grpc.pb.h',
 'cpp/rl_sdk/session.h': 'rl_sdk/session.h',
 'cpp/rl_sdk/transport.h': 'rl_sdk/transport.h',
 'cpp/rl_sdk/replay_window.h': 'rl_sdk/replay_window.h',
 'cpp/rl_sdk/server_command.h': 'rl_sdk/server_command.h'}


def require_regular_file(path: Path) -> None:
    if not path.is_file() or path.is_symlink():
        raise SystemExit(f"required protocol file is missing: {path}")


def sync_snapshot(artifact_root: Path, target_root: Path) -> None:
    target_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".contract-snapshot-", dir=target_root.parent
    ) as temporary:
        stage = Path(temporary)
        for artifact_name, local_name in SNAPSHOT_FILES.items():
            source = artifact_root / artifact_name
            require_regular_file(source)
            target = stage / local_name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)

        for local_name in SNAPSHOT_FILES.values():
            target = target_root / local_name
            target.parent.mkdir(parents=True, exist_ok=True)
            os.replace(stage / local_name, target)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Explicitly synchronize the Maze task protocol files"
    )
    parser.add_argument("--artifact-dir", required=True, type=Path)
    parser.add_argument("--target-dir", required=True, type=Path)
    args = parser.parse_args()
    sync_snapshot(args.artifact_dir.resolve(), args.target_dir.resolve())
    print("Maze task protocol files synchronized")


if __name__ == "__main__":
    main()
