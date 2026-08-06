#!/usr/bin/env bash

set -euo pipefail

action="${1:-shell}"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
container_name="client-dev"
network_name="rl-training-dev"
tag="${RL_CLIENT_DEV_IMAGE_TAG:-test-001}"
runtime_image="rl-training/maze-client:${RL_CLIENT_IMAGE_TAG:-test-001}"
dev_image="rl-training/maze-client-dev:${tag}"
replay_host_port=9004
replay_tunnel_socket="${TMPDIR:-/tmp}/rl-training-client-dev-9004.sock"
colima_ssh_config="${RL_COLIMA_SSH_CONFIG:-${HOME}/.colima/_lima/colima/ssh.config}"
replay_mode="${RL_REPLAY_MODE:-local-test}"

case "${replay_mode}" in
    local-test|model-evaluation)
        ;;
    *)
        echo "invalid RL_REPLAY_MODE: ${replay_mode}" >&2
        exit 2
        ;;
esac

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
        --build-arg "CLIENT_RUNTIME_IMAGE=${runtime_image}" \
        --tag "${dev_image}" \
        "${repo_dir}"
}

ensure_container() {
    if ! docker image inspect "${dev_image}" >/dev/null 2>&1; then
        build_image
    fi
    if ! docker network inspect "${network_name}" >/dev/null 2>&1; then
        docker network create "${network_name}" >/dev/null
    fi
    if docker container inspect "${container_name}" >/dev/null 2>&1; then
        replay_binding="$(docker port "${container_name}" 9004/tcp 2>/dev/null || true)"
        if [ "${replay_binding}" != "127.0.0.1:${replay_host_port}" ]; then
            docker rm --force "${container_name}" >/dev/null
        fi
    fi
    if ! docker container inspect "${container_name}" >/dev/null 2>&1; then
        run_args=(
            --detach
            --name "${container_name}" \
            --network "${network_name}" \
            --network-alias "${container_name}" \
            --network-alias "maze-client" \
            --env RL_REPLAY_PORT=9004 \
            --publish "127.0.0.1:${replay_host_port}:9004" \
            --volume "${repo_dir}:/workspace/maze-client" \
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
    test)
        ensure_container
        docker exec "${container_name}" sh -lc \
            "cd /workspace/maze-client && \
             ./build.sh && \
             python3 -m py_compile tools/viz_player/maze_viz_server.py && \
             bash tests/replay_mode_test.sh"
        ;;
    replay)
        ensure_container
        printf 'Replay URL: http://127.0.0.1:%s/\n' "${replay_host_port}"
        exec docker exec -it "${container_name}" bash -lc \
            "cd /workspace/maze-client && exec bash ./replay.sh ${replay_mode}"
        ;;
    replay-start)
        ensure_container
        printf 'Replay URL: http://127.0.0.1:%s/\n' "${replay_host_port}"
        ;;
    replay-stop)
        stop_replay_transport
        ;;
    clean)
        if docker container inspect "${container_name}" >/dev/null 2>&1; then
            if [ "$(docker inspect --format '{{.State.Running}}' "${container_name}")" = "true" ]; then
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
