#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
image_name="rl-training/maze-client"
image_tag="${RL_CLIENT_IMAGE_TAG:-p1-d3t-0.14.0}"

if [[ ! "${image_tag}" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}$ ]]; then
    echo "RL_CLIENT_IMAGE_TAG is not a valid explicit Docker tag" >&2
    exit 2
fi

image_ref="${image_name}:${image_tag}"
if docker image inspect "${image_ref}" >/dev/null 2>&1; then
    echo "refusing to overwrite an existing Client image tag: ${image_ref}" >&2
    echo "choose a new explicit RL_CLIENT_IMAGE_TAG" >&2
    exit 1
fi

docker build \
    --label "org.rl-training.component=maze-client" \
    --label "org.rl-training.component-contract.path=/opt/rl/component-contract/manifest.json" \
    --label "org.rl-training.build-profile=p1-d3t" \
    --tag "${image_ref}" \
    "${repo_dir}"

printf '%s\n' "${image_ref}"
