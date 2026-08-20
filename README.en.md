# Maze Client

[简体中文](README.md) | English

Maze Client connects to AIServer and executes environment Episodes. In training,
start it after Learner and AIServer are ready. Evaluation requires only AIServer
to be started first.

## 1. Development container, incremental build, and tests

```bash
# Host: build or reuse the independent development image and enter it
make shell

# Inside the container: build and test are explicit, separate entrypoints
./build.sh
bash ./test.sh

# The host can also reuse the same container for a build
make build
```

The development image does not inherit an old runtime image and uses persistent
ccache. `ninja: no work to do.` does not automatically run tests. Tests may be
started only from the repository root with `bash ./test.sh`; `build.sh`, Docker
image builds, and other wrappers do not run them implicitly. Run `make shell`
only on the host.

## 2. Connect to AIServer manually

```bash
make shell
./build.sh
./run.sh --help
./run.sh --config configs/client_config.yaml --aiserver maze-aiserver:9002
```

`--help` prints the supported overrides and their config fields without
loading a map, connecting to AIServer, or starting Replay.

Client does not accept a workload argument. The actual mode comes from the AIServer `OpenSession` response.
The config file provides complete network and Replay defaults. `--aiserver`,
`--replay-dir`, and `--replay-port` only override existing `network.*` and
`viz.*` fields. `run.sh` forwards arguments byte-for-byte; the C++ config layer
publishes the final Replay directory and port back to the supervisor through
the Session-policy handoff.
Training assignments require lineage, an explicitly present `model_step`, and
model/manifest digests. Evaluation assignments carry only the selected model
file digest and never fabricate a training step or lineage.

Client loads the TaskSpec-selected `<map_id>.json` exactly from the config
default or `RL_ENV_MAP_REGISTRY_DIR`. The expected map/digest variables only
override map assertions that default to `null`. Client has no Agent-count
assertion or override; the actual count comes only from AIServer
`OpenSessionRsp.EnvironmentRuntimeSpec.agent_count`.

## 3. View a local replay

`evaluation` uses config's default replay port `9004`; Training does not start
replay. `run.sh` starts replay only after an evaluation Session policy, and the
internal `replay.sh` has no independent directory or port fallback. The
evaluation `validation-manifest.json` records only the model SHA-256, not a
training step.

AIServer currently assigns one evaluation Episode. After it completes, the
Client process exits normally while `run.sh` keeps the Replay HTTP service
available. Press `Ctrl-C` to stop Replay and return to the shell. This persistent
wrapper state is not a Client restart.

Open:

```text
http://127.0.0.1:9004/
```

## 4. Build the runtime image

The runtime image accepts only clean source and a synchronized formal Contracts
artifact. Run from the host:

```bash
bash scripts/sync_contract_snapshot.sh
bash build_image.sh
```

The build never consumes development artifacts or a development-container build
tree. It prints the image reference derived from the current stack source
identity.

## 5. Remove the development container

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
