#!/usr/bin/env bash

set -euo pipefail

action="${1:-shell}"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
container_name="client-dev"
network_name="rl-training-dev"
tag="${RL_CLIENT_DEV_IMAGE_TAG:-test-001}"
dev_image="rl-training/maze-client-dev:${tag}"
cpp_dev_base_image="${CLIENT_DEV_BASE_IMAGE:-python:3.11-slim}"
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
        --label "org.rl-training.dev-platform=${platform}" \
        --tag "${dev_image}" \
        "${repo_dir}"
}

ensure_dev_image() {
    echo "Building Maze Client development image" >&2
    build_image
}

container_exists() {
    docker container inspect "${container_name}" >/dev/null 2>&1
}

container_running() {
    [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" = "true" ]
}

container_uses_current_image() {
    docker image inspect "${dev_image}" >/dev/null 2>&1 || return 1
    [ "$(docker inspect --format '{{.Image}}' "${container_name}")" = \
      "$(docker image inspect --format '{{.Id}}' "${dev_image}")" ]
}

container_has_legacy_contract_mount() {
    [ "$(docker inspect \
        --format '{{range .Mounts}}{{if eq .Destination "/workspace/dev-artifacts/rl-contracts"}}yes{{end}}{{end}}' \
        "${container_name}")" = "yes" ]
}

warn_container_drift() {
    if docker image inspect "${dev_image}" >/dev/null 2>&1 &&
       ! container_uses_current_image; then
        echo "client-dev uses an older local image; run make dev-refresh when ready" >&2
    fi
    if container_has_legacy_contract_mount; then
        echo "client-dev still has the retired Contracts mount; run make dev-refresh to remove it" >&2
    fi
}

container_has_business_processes() {
    local process_status
    container_running || return 1
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

ensure_container_resources() {
    if ! docker network inspect "${network_name}" >/dev/null 2>&1; then
        docker network create "${network_name}" >/dev/null
    fi
    if ! docker volume inspect "${ccache_volume}" >/dev/null 2>&1; then
        docker volume create "${ccache_volume}" >/dev/null
    fi
}

create_container() {
    docker run --detach \
        --name "${container_name}" \
        --network "${network_name}" \
        --network-alias "${container_name}" \
        --network-alias "maze-client" \
        --env "CCACHE_DIR=${ccache_dir}" \
        --publish "127.0.0.1:${replay_host_port}:9004" \
        --volume "${repo_dir}:/workspace/maze-client" \
        --volume "${ccache_volume}:${ccache_dir}" \
        "${dev_image}" >/dev/null
}

ensure_container() {
    if container_exists; then
        if ! container_running; then
            docker start "${container_name}" >/dev/null
        fi
        warn_container_drift
        ensure_replay_transport
        return
    fi

    ensure_dev_image
    ensure_container_resources
    create_container
    ensure_replay_transport
}

refresh_container() {
    local process_state
    if container_exists && container_running; then
        process_state=0
        container_has_business_processes || process_state=$?
        if [ "${process_state}" -eq 0 ]; then
            echo "client-dev has an active Client, Replay, test, or build process" >&2
            echo "Stop it first; use make replay-stop for the resident Replay service" >&2
            exit 1
        elif [ "${process_state}" -ne 1 ]; then
            exit 1
        fi
    fi

    ensure_dev_image
    ensure_container_resources
    if container_exists; then
        stop_replay_transport
        if container_running; then
            docker stop --time 5 "${container_name}" >/dev/null
        fi
        docker rm "${container_name}" >/dev/null
    fi
    create_container
    ensure_replay_transport
    echo "Maze Client development container refreshed: ${container_name}"
}

case "${action}" in
    image)
        build_image
        ;;
    refresh)
        refresh_container
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
        if container_exists; then
            if container_running; then
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
