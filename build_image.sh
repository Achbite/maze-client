#!/usr/bin/env bash

set -euo pipefail

CLIENT_IMAGE_TAG="${RL_CLIENT_IMAGE_TAG:-training-001}"
CLIENT_IMAGE_NAME="rl-training/maze-client"

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace_root="$(cd "${repo_dir}/.." && pwd)"
context_root="${workspace_root}/.workspace/build-contexts/maze-client"
source "${repo_dir}/artifact_versions.env"
contract_dir="${repo_dir}/proto"

bash "${repo_dir}/scripts/verify_source_inventory.sh"

if [ ! -f "${contract_dir}/manifest.json" ] ||
   [ ! -f "${contract_dir}/common.pb.cc" ] ||
   [ ! -f "${contract_dir}/maze_task.pb.cc" ] ||
   [ ! -f "${contract_dir}/maze_task.grpc.pb.cc" ]; then
    echo "Repository-local contract snapshot is incomplete: ${contract_dir}" >&2
    exit 1
fi

python3 - "${contract_dir}" "${RL_CONTRACTS_VERSION}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
if manifest.get("package") != "rl-contracts" or manifest.get("version") != sys.argv[2]:
    raise SystemExit("Repository-local contract identity is invalid")
files = {
    "common.proto": "common.proto",
    "maze_task.proto": "maze_task.proto",
    "cpp/common.pb.cc": "common.pb.cc",
    "cpp/common.pb.h": "common.pb.h",
    "cpp/maze_task.pb.cc": "maze_task.pb.cc",
    "cpp/maze_task.pb.h": "maze_task.pb.h",
    "cpp/maze_task.grpc.pb.cc": "maze_task.grpc.pb.cc",
    "cpp/maze_task.grpc.pb.h": "maze_task.grpc.pb.h",
}
for artifact_name, local_name in files.items():
    path = root / local_name
    expected = manifest.get("files", {}).get(artifact_name)
    if not path.is_file() or not expected:
        raise SystemExit(f"Repository-local contract file is missing: {path}")
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise SystemExit(f"Repository-local contract checksum mismatch: {path}")
PY

python3 - "${repo_dir}" "${context_root}" <<'PY'
import pathlib
import shutil
import sys

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])

def ignore_runtime_outputs(directory, names):
    ignored = {".git", "build", "_deps", ".DS_Store"} & set(names)
    if pathlib.Path(directory) == source:
        ignored.update({"log", "logs"} & set(names))
    return ignored

if target.exists():
    shutil.rmtree(target)
shutil.copytree(
    source,
    target,
    ignore=ignore_runtime_outputs,
)
if not (target / "src/log/logger.h").is_file():
    raise SystemExit("Build context is missing src/log/logger.h")
PY

docker build \
    --tag "${CLIENT_IMAGE_NAME}:${CLIENT_IMAGE_TAG}" \
    "${context_root}"

printf '%s\n' "${CLIENT_IMAGE_NAME}:${CLIENT_IMAGE_TAG}"
