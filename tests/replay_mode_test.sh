#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_root="$(mktemp -d)"
trap 'rm -rf "${test_root}"' EXIT

fake_client="${test_root}/maze_client"
cat >"${fake_client}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
cat >"${MAZE_SESSION_POLICY_PATH}" <<EOF
workload=${FAKE_WORKLOAD}
replay_policy=${FAKE_REPLAY_POLICY}
EOF
sleep "${FAKE_CLIENT_SLEEP:-0}"
SH
chmod +x "${fake_client}"

replay_marker="${test_root}/replay-started"
fake_replay="${test_root}/replay.sh"
cat >"${fake_replay}" <<SH
#!/usr/bin/env bash
touch "${replay_marker}"
exit 0
SH
chmod +x "${fake_replay}"

MAZE_CLIENT_BIN="${fake_client}" \
MAZE_REPLAY_BIN="${fake_replay}" \
FAKE_WORKLOAD=training \
FAKE_REPLAY_POLICY=disabled \
bash "${repo_dir}/run.sh"

if [ -e "${replay_marker}" ]; then
    echo "training started Replay" >&2
    exit 1
fi

if bash "${repo_dir}/replay.sh" training \
    >"${test_root}/replay.out" 2>&1; then
    echo "Replay accepted training" >&2
    exit 1
fi

grep -q "Replay is disabled for training" "${test_root}/replay.out"

cat >"${fake_replay}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
case "${1:?workload is required}" in
    local-test|model-evaluation)
        ;;
    *)
        exit 2
        ;;
esac
: "${MAZE_REPLAY_PORT:?MAZE_REPLAY_PORT is required}"
exec python3 -m http.server "${MAZE_REPLAY_PORT}" --bind 127.0.0.1
SH
chmod +x "${fake_replay}"

for workload in local-test model-evaluation; do
    replay_dir="${test_root}/${workload}"
    result_path="${replay_dir}/client-result.json"
    replay_port="$(
        python3 - <<'PY'
import socket

with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    print(listener.getsockname()[1])
PY
    )"

    MAZE_CLIENT_BIN="${fake_client}" \
    MAZE_REPLAY_BIN="${fake_replay}" \
    MAZE_VIZ_OUTPUT_DIR="${replay_dir}" \
    MAZE_REPLAY_PORT="${replay_port}" \
    MAZE_VALIDATION_ID="atomic-result-test" \
    MAZE_VALIDATION_RESULT_PATH="${result_path}" \
    FAKE_WORKLOAD="${workload}" \
    FAKE_REPLAY_POLICY=record-and-serve \
    bash "${repo_dir}/run.sh" \
        >"${test_root}/${workload}.out" 2>&1 &
    run_pid=$!

    result_ready=0
    for _ in $(seq 1 100); do
        if [ -f "${result_path}" ]; then
            result_ready=1
            break
        fi
        if ! kill -0 "${run_pid}" 2>/dev/null; then
            wait "${run_pid}"
            exit $?
        fi
        sleep 0.02
    done
    if [ "${result_ready}" -ne 1 ]; then
        echo "${workload} result was not published" >&2
        exit 1
    fi

    python3 - "${result_path}" "${workload}" <<'PY'
import json
import sys
from pathlib import Path

document = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert document["validation_id"] == "atomic-result-test"
assert document["workload"] == sys.argv[2]
assert document["exit_code"] == 0
PY

    if find "${replay_dir}" -name 'client-result.json.tmp.*' -print -quit |
       grep -q .; then
        echo "temporary validation result was not cleaned" >&2
        exit 1
    fi

    kill -TERM "${run_pid}"
    wait "${run_pid}"
done
