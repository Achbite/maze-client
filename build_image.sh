#!/usr/bin/env bash

set -euo pipefail

requested_image_tag="${RL_CLIENT_IMAGE_TAG:-}"
CLIENT_IMAGE_NAME="rl-training/maze-client"

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace_root="${RL_TRAINING_WORKSPACE:-$(cd "${repo_dir}/.." && pwd)}"
context_root="${workspace_root}/.workspace/build-contexts/maze-client-$$"
source "${repo_dir}/artifact_versions.env"
contract_dir="${repo_dir}/proto"

if test -n "$(git -C "${repo_dir}" status --porcelain --untracked-files=all)"; then
    echo "refusing to build a Client runtime image from a dirty worktree" >&2
    exit 1
fi

bash "${repo_dir}/scripts/verify_source_inventory.sh"

if [ ! -f "${contract_dir}/manifest.json" ] ||
   [ ! -f "${contract_dir}/common.pb.cc" ] ||
   [ ! -f "${contract_dir}/maze_task.pb.cc" ] ||
   [ ! -f "${contract_dir}/maze_task.grpc.pb.cc" ]; then
    echo "Repository-local contract snapshot is incomplete: ${contract_dir}" >&2
    exit 1
fi

python3 - \
    "${contract_dir}" \
    "${RL_CONTRACTS_VERSION}" \
    "${RL_CONTRACTS_PLATFORM}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
if (
    manifest.get("schema_version") != 2
    or manifest.get("package") != "rl-contracts"
    or manifest.get("version") != sys.argv[2]
    or manifest.get("platform") != sys.argv[3]
    or manifest.get("source_tree_state") != "clean"
):
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

stack_identity_tool="${workspace_root}/rl-framework/tools/compute_stack_source_id.py"
if [ ! -f "${stack_identity_tool}" ]; then
    echo "stack source identity tool is missing: ${stack_identity_tool}" >&2
    exit 1
fi
stack_identity_json="$(
    python3 "${stack_identity_tool}" --workspace-root "${workspace_root}"
)"
identity_fields="$(
    python3 -c '
import json
import sys

document = json.loads(sys.argv[1])
print("\t".join((
    document["stack_source_id"],
    document["repositories"]["maze-client"],
    document["artifacts"]["rl-contracts"]["artifact_digest"],
    document["artifacts"]["rl-contracts"]["manifest_sha256"],
    document["configs"]["maze-client"]["sha256"],
)))
' "${stack_identity_json}"
)"
IFS=$'\t' read -r \
    stack_source_id component_commit contracts_artifact_digest \
    contracts_manifest_digest component_config_digest \
    <<< "${identity_fields}"
canonical_image_tag="a3-${RL_CONTRACTS_VERSION}-${stack_source_id:0:12}"
if [ -n "${requested_image_tag}" ] &&
   [ "${requested_image_tag}" != "${canonical_image_tag}" ]; then
    echo "Client image tag must match the canonical stack identity:" >&2
    echo "  expected=${canonical_image_tag}" >&2
    echo "  requested=${requested_image_tag}" >&2
    exit 1
fi
image_ref="${CLIENT_IMAGE_NAME}:${canonical_image_tag}"

if docker image inspect "${image_ref}" >/dev/null 2>&1; then
    existing_identity="$(
        docker image inspect --format \
            '{{index .Config.Labels "org.rl-training.stack-source-id"}}|{{index .Config.Labels "org.rl-training.component"}}|{{index .Config.Labels "org.rl-training.component-commit"}}|{{index .Config.Labels "org.rl-training.contracts-version"}}|{{index .Config.Labels "org.rl-training.contracts-artifact-digest"}}|{{index .Config.Labels "org.rl-training.contracts-manifest-digest"}}|{{index .Config.Labels "org.rl-training.component-config-digest"}}' \
            "${image_ref}"
    )"
    expected_identity="${stack_source_id}|maze-client|${component_commit}|${RL_CONTRACTS_VERSION}|${contracts_artifact_digest}|${contracts_manifest_digest}|${component_config_digest}"
    if [ "${existing_identity}" != "${expected_identity}" ]; then
        echo "refusing to overwrite an existing Client tag with another identity: ${image_ref}" >&2
        exit 1
    fi
    printf '%s\n' "${image_ref}"
    exit 0
fi

trap 'rm -rf "${context_root}"' EXIT

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

mkdir -p "${context_root}/_deps/identity"
printf '%s\n' "${stack_identity_json}" \
    > "${context_root}/_deps/identity/stack-source.json"

docker build \
    --label "org.opencontainers.image.revision=${component_commit}" \
    --label "org.rl-training.component=maze-client" \
    --label "org.rl-training.component-commit=${component_commit}" \
    --label "org.rl-training.stack-source-id=${stack_source_id}" \
    --label "org.rl-training.contracts-version=${RL_CONTRACTS_VERSION}" \
    --label "org.rl-training.contracts-artifact-digest=${contracts_artifact_digest}" \
    --label "org.rl-training.contracts-manifest-digest=${contracts_manifest_digest}" \
    --label "org.rl-training.component-config-digest=${component_config_digest}" \
    --tag "${image_ref}" \
    "${context_root}"

printf '%s\n' "${image_ref}"
