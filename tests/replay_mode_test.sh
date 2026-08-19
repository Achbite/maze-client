#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_root="$(mktemp -d)"
trap 'rm -rf "${test_root}"' EXIT

fake_client="${test_root}/maze_client"
cat >"${fake_client}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
cat >"${RL_SESSION_POLICY_PATH}" <<EOF
workload=${FAKE_WORKLOAD}
replay_policy=${FAKE_REPLAY_POLICY}
behavior_policy_scope=${FAKE_POLICY_SCOPE}
replay_output_dir=${FAKE_REPLAY_DIR}
replay_server_port=${FAKE_REPLAY_PORT}
model_artifact_digest=e7c005970ade5c38ab4a3f0883deb7adf8956694d6dc30be598c18954941f3d4
EOF
if [ "${FAKE_WORKLOAD}" = "training" ]; then
    printf 'model_step=0\n' >>"${RL_SESSION_POLICY_PATH}"
    printf 'model_lineage_id=local-lineage\n' \
        >>"${RL_SESSION_POLICY_PATH}"
fi
printf '%s\n' "$@" >"${FAKE_ARGS_FILE}"
exit 0
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

arguments=(--config configs/client_config.yaml --aiserver 127.0.0.1:19002)
RL_CLIENT_BIN="${fake_client}" \
RL_REPLAY_BIN="${fake_replay}" \
FAKE_ARGS_FILE="${test_root}/training-arguments" \
FAKE_WORKLOAD=training \
FAKE_REPLAY_POLICY=disabled \
FAKE_POLICY_SCOPE=training-fragment \
FAKE_REPLAY_DIR="${test_root}/training" \
FAKE_REPLAY_PORT=19004 \
bash "${repo_dir}/run.sh" "${arguments[@]}"
printf '%s\n' "${arguments[@]}" >"${test_root}/expected-arguments"
cmp -s "${test_root}/training-arguments" \
    "${test_root}/expected-arguments"
[ ! -e "${replay_marker}" ]

cat >"${fake_replay}" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:?workload is required}" = "evaluation" ]
exec python3 -m http.server "${RL_REPLAY_PORT}" --bind 127.0.0.1
SH
chmod +x "${fake_replay}"

replay_dir="${test_root}/evaluation"
result_path="${replay_dir}/client-result.json"
replay_port="$(
    python3 - <<'PY'
import socket
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    print(listener.getsockname()[1])
PY
)"
RL_CLIENT_BIN="${fake_client}" \
RL_REPLAY_BIN="${fake_replay}" \
FAKE_ARGS_FILE="${test_root}/evaluation-arguments" \
FAKE_WORKLOAD=evaluation \
FAKE_REPLAY_POLICY=record-and-serve \
FAKE_POLICY_SCOPE=evaluation-episode \
FAKE_REPLAY_DIR="${replay_dir}" \
FAKE_REPLAY_PORT="${replay_port}" \
RL_VALIDATION_ID=local-evaluation \
RL_VALIDATION_RESULT_PATH="${result_path}" \
bash "${repo_dir}/run.sh" "${arguments[@]}" \
    >"${test_root}/evaluation.out" 2>&1 &
run_pid=$!

for _ in $(seq 1 100); do
    [ -f "${result_path}" ] && break
    if ! kill -0 "${run_pid}" 2>/dev/null; then
        wait "${run_pid}"
        exit $?
    fi
    sleep 0.02
done
[ -f "${result_path}" ]

python3 - "${result_path}" "${replay_dir}/validation-manifest.json" <<'PY'
import json
import sys
from pathlib import Path

result = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
manifest = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
assert result["workload"] == "evaluation"
assert result["exit_code"] == 0
assert manifest["parameters"]["workload"] == "evaluation"
assert len(manifest["model"]["sha256"]) == 64
assert "model_step" not in manifest["model"]
assert "model_lineage_id" not in manifest["model"]
PY

kill -TERM "${run_pid}"
wait "${run_pid}"
