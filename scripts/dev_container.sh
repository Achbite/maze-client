#!/usr/bin/env bash

set -euo pipefail

action="${1:-shell}"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
workspace_root="${RL_TRAINING_WORKSPACE:-$(cd "${repo_dir}/.." && pwd -P)}"
container_name="client-dev"
network_name="rl-training-dev"
tag="${RL_CLIENT_DEV_IMAGE_TAG:-test-001}"
dev_image="rl-training/maze-client-dev:${tag}"
cpp_dev_base_image="${CLIENT_DEV_BASE_IMAGE:-python@sha256:b27df5841f3355e9473f9a516d38a6783b6c8dfeacaf2d14a240f443b368ddb6}"
ccache_volume="rl-training-maze-client-ccache"
ccache_dir="/var/cache/ccache"
replay_host_port=9004
replay_tunnel_socket="${TMPDIR:-/tmp}/rl-training-client-dev-9004.sock"
colima_ssh_config="${RL_COLIMA_SSH_CONFIG:-${HOME}/.colima/_lima/colima/ssh.config}"

if [ -f "/.dockerenv" ]; then
    echo "make shell is a host-side Docker entrypoint; leave the component container first" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1; then
    echo "make shell is a host-side Docker entrypoint; Docker CLI is unavailable" >&2
    exit 1
fi
if ! platform="$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}' 2>/dev/null)" ||
   [ -z "${platform}" ]; then
    echo "make shell cannot reach the host Docker daemon" >&2
    exit 1
fi

dev_image_input_digest() {
    python3 - \
        "${repo_dir}/Dockerfile.dev" \
        "${repo_dir}/artifact_versions.env" \
        "${repo_dir}/scripts/dev_container.sh" \
        "${platform}" \
        "${cpp_dev_base_image}" <<'PY'
import hashlib
import sys
from pathlib import Path

digest = hashlib.sha256()
for raw in sys.argv[1:4]:
    path = Path(raw)
    digest.update(path.name.encode("utf-8"))
    digest.update(b"\0")
    digest.update(path.read_bytes())
    digest.update(b"\0")
for value in sys.argv[4:]:
    digest.update(value.encode("utf-8"))
    digest.update(b"\0")
print(digest.hexdigest())
PY
}

dev_input_digest="$(dev_image_input_digest)"

tcp_ready() {
    nc -z 127.0.0.1 "${replay_host_port}" >/dev/null 2>&1
}

colima_ssh_target() {
    awk '$1 == "Host" { print $2; exit }' "${colima_ssh_config}"
}

replay_tunnel_running() {
    local target
    [ -S "${replay_tunnel_socket}" ] || return 1
    [ -f "${colima_ssh_config}" ] || return 1
    target="$(colima_ssh_target)"
    [ -n "${target}" ] || return 1
    ssh \
        -F "${colima_ssh_config}" \
        -o "ControlPath=${replay_tunnel_socket}" \
        -O check \
        "${target}" >/dev/null 2>&1
}

ensure_replay_transport() {
    local target
    if tcp_ready || replay_tunnel_running; then
        return
    fi
    if [ ! -f "${colima_ssh_config}" ] || ! command -v ssh >/dev/null 2>&1; then
        return
    fi
    target="$(colima_ssh_target)"
    if [ -z "${target}" ]; then
        echo "Colima SSH config has no Host entry: ${colima_ssh_config}" >&2
        exit 1
    fi
    rm -f "${replay_tunnel_socket}"
    ssh \
        -F "${colima_ssh_config}" \
        -o ControlMaster=yes \
        -o "ControlPath=${replay_tunnel_socket}" \
        -o ControlPersist=no \
        -o ExitOnForwardFailure=yes \
        -o ServerAliveInterval=15 \
        -o ServerAliveCountMax=3 \
        -L "127.0.0.1:${replay_host_port}:127.0.0.1:${replay_host_port}" \
        -N \
        -f \
        "${target}"
}

stop_replay_transport() {
    local target
    if replay_tunnel_running; then
        target="$(colima_ssh_target)"
        ssh \
            -F "${colima_ssh_config}" \
            -o "ControlPath=${replay_tunnel_socket}" \
            -O exit \
            "${target}" >/dev/null 2>&1 || true
    fi
    rm -f "${replay_tunnel_socket}"
}

build_image() {
    docker build \
        --file "${repo_dir}/Dockerfile.dev" \
        --build-arg "CPP_DEV_BASE_IMAGE=${cpp_dev_base_image}" \
        --label "org.rl-training.component=maze-client-dev" \
        --label "org.rl-training.dev-input-digest=${dev_input_digest}" \
        --label "org.rl-training.dev-platform=${platform}" \
        --tag "${dev_image}" \
        "${repo_dir}"
}

ensure_dev_image() {
    local actual_digest=""
    if docker image inspect "${dev_image}" >/dev/null 2>&1; then
        actual_digest="$(
            docker image inspect \
                --format '{{index .Config.Labels "org.rl-training.dev-input-digest"}}' \
                "${dev_image}"
        )"
    fi
    if [ "${actual_digest}" != "${dev_input_digest}" ]; then
        echo "Building Maze Client development image for input ${dev_input_digest:0:12}" >&2
        build_image
    fi
}

prepare_contract_artifact() {
    contract_dir="$(
        RL_TRAINING_WORKSPACE="${workspace_root}" \
            bash "${workspace_root}/rl-contracts/build_dev_artifact.sh"
    )"
    if [ ! -f "${contract_dir}/manifest.json" ] ||
       [ ! -f "${contract_dir}/cpp/maze_task.pb.cc" ]; then
        echo "development Contracts artifact is incomplete: ${contract_dir}" >&2
        return 1
    fi
}

container_mount_source() {
    local destination="$1"
    docker inspect \
        --format "{{range .Mounts}}{{if eq .Destination \"${destination}\"}}{{.Source}}{{end}}{{end}}" \
        "${container_name}"
}

container_mount_name() {
    local destination="$1"
    docker inspect \
        --format "{{range .Mounts}}{{if eq .Destination \"${destination}\"}}{{.Name}}{{end}}{{end}}" \
        "${container_name}"
}

container_has_business_processes() {
    local process_status
    [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" = "true" ] ||
        return 1
    set +e
    docker exec "${container_name}" sh -lc \
        "pgrep -f '[m]aze_client|[/]run.sh|[m]aze_viz_server.py|[c]make --build|[c]test' >/dev/null"
    process_status=$?
    set -e
    if [ "${process_status}" -eq 0 ]; then
        return 0
    fi
    if [ "${process_status}" -eq 1 ]; then
        return 1
    fi
    echo "Unable to inspect client-dev business processes" >&2
    return 2
}

container_matches_inputs() {
    local replay_binding
    replay_binding="$(docker port "${container_name}" 9004/tcp 2>/dev/null || true)"
    [ "$(docker inspect --format '{{.Image}}' "${container_name}")" = \
      "$(docker image inspect --format '{{.Id}}' "${dev_image}")" ] &&
    [ "${replay_binding}" = "127.0.0.1:${replay_host_port}" ] &&
    [ "$(container_mount_name "/var/cache/ccache")" = "${ccache_volume}" ] &&
    [ "$(container_mount_source "/workspace/dev-artifacts/rl-contracts")" = \
      "${contract_dir}" ]
}

ensure_container() {
    local process_state
    ensure_dev_image
    prepare_contract_artifact
    if ! docker network inspect "${network_name}" >/dev/null 2>&1; then
        docker network create "${network_name}" >/dev/null
    fi
    if ! docker volume inspect "${ccache_volume}" >/dev/null 2>&1; then
        docker volume create "${ccache_volume}" >/dev/null
    fi
    if docker container inspect "${container_name}" >/dev/null 2>&1 &&
       ! container_matches_inputs; then
        process_state=0
        container_has_business_processes || process_state=$?
        if [ "${process_state}" -eq 0 ]; then
            echo "client-dev inputs changed while Client/build processes are active" >&2
            echo "Stop the active process before recreating client-dev" >&2
            exit 1
        elif [ "${process_state}" -ne 1 ]; then
            exit 1
        fi
        echo "Recreating idle client-dev for current development inputs" >&2
        docker rm --force "${container_name}" >/dev/null
    fi
    if ! docker container inspect "${container_name}" >/dev/null 2>&1; then
        run_args=(
            --detach
            --name "${container_name}" \
            --network "${network_name}" \
            --network-alias "${container_name}" \
            --network-alias "maze-client" \
            --env "CCACHE_DIR=${ccache_dir}" \
            --env "RL_CONTRACT_DEV_ARTIFACT_DIR=/workspace/dev-artifacts/rl-contracts" \
            --publish "127.0.0.1:${replay_host_port}:9004" \
            --volume "${repo_dir}:/workspace/maze-client" \
            --volume "${ccache_volume}:${ccache_dir}" \
            --volume "${contract_dir}:/workspace/dev-artifacts/rl-contracts:ro" \
        )
        docker run "${run_args[@]}" "${dev_image}" >/dev/null
    elif [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" != "true" ]; then
        docker start "${container_name}" >/dev/null
    fi
    ensure_replay_transport
}

case "${action}" in
    image)
        build_image
        ;;
    shell)
        ensure_container
        exec docker exec -it "${container_name}" bash
        ;;
    build)
        ensure_container
        docker exec "${container_name}" sh -lc \
            "cd /workspace/maze-client && ./build.sh"
        ;;
    replay)
        ensure_container
        exec docker exec -it "${container_name}" bash -lc \
            "cd /workspace/maze-client && exec bash ./run_replay.sh"
        ;;
    replay-stop)
        if docker container inspect "${container_name}" >/dev/null 2>&1 &&
           [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" = "true" ]; then
            docker exec "${container_name}" bash -lc \
                "cd /workspace/maze-client && exec bash ./run_replay.sh -stop"
        else
            echo "[Replay] not running"
        fi
        stop_replay_transport
        ;;
    clean)
        if docker container inspect "${container_name}" >/dev/null 2>&1; then
            if [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" = "true" ]; then
                process_state=0
                container_has_business_processes || process_state=$?
                if [ "${process_state}" -eq 0 ]; then
                    echo "client-dev has active Client/build processes" >&2
                    echo "Stop the active process before make dev-clean" >&2
                    exit 1
                elif [ "${process_state}" -ne 1 ]; then
                    exit 1
                fi
                docker stop --time 5 "${container_name}" >/dev/null
            fi
            docker rm "${container_name}" >/dev/null
        fi
        stop_replay_transport
        ;;
    *)
        echo "unknown dev action: ${action}" >&2
        exit 2
        ;;
esac
