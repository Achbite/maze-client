#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
workspace_root="${RL_TRAINING_WORKSPACE:-$(cd "${repo_dir}/.." && pwd -P)}"
context_root="${workspace_root}/.workspace/build-contexts/maze-client-$$"
image_name="rl-training/maze-client"
image_tag="${RL_PROJECT_IMAGE_TAG:-maze-tag-001}"

if [[ ! "${image_tag}" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]]; then
    echo "RL_PROJECT_IMAGE_TAG is not a valid Docker tag" >&2
    exit 2
fi

bash "${repo_dir}/scripts/verify_source_inventory.sh"
python3 "${repo_dir}/scripts/verify_contract_snapshot.py" \
    "${repo_dir}/proto"

trap 'rm -rf "${context_root}"' EXIT
python3 - "${repo_dir}" "${context_root}" <<'PY'
import pathlib
import shutil
import sys

source = pathlib.Path(sys.argv[1])
target = pathlib.Path(sys.argv[2])


def ignore_runtime_outputs(directory, names):
    ignored = {".git"} & set(names)
    if pathlib.Path(directory) == source:
        ignored.update(
            {
                ".workspace",
                "_deps",
                "build",
                "build-contract-0.3",
                "log",
                "logs",
                "replays",
            }
            & set(names)
        )
    return ignored


shutil.copytree(source, target, ignore=ignore_runtime_outputs)
if not (target / "src/log/logger.h").is_file():
    raise SystemExit("Build context is missing src/log/logger.h")
PY

image_ref="${image_name}:${image_tag}"
docker build \
    --label "org.rl-training.component=client" \
    --label "org.rl-training.project-image-tag=${image_tag}" \
    --tag "${image_ref}" \
    "${context_root}"

printf '%s\n' "${image_ref}"
