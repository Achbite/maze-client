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

Infra managed mode is selected by `RL_CONFIG_PATH`. After connecting to its
paired AIServer, Client first publishes `/run/rl/readiness.json` and waits for
the owning Node to write `/run/rl/training-admission.v1.json` for the current
attempt. `run.sh` validates that token exactly against
`/run/rl/execution-identity.v1.json`, including the schema, Allocation,
NodeSession, PodAttempt, ComponentAttempt, and generation. It atomically
publishes the process gate, and the C++ Client enters the existing OpenSession
only after that gate appears. Unmanaged execution does not require Infra
admission and retains its existing behavior.

The image healthcheck follows the same mode boundary: managed execution checks
`/run/rl/readiness.json`, while unmanaged execution checks the existing
`/tmp/rl-client-session-policy`. A managed Client can therefore report that it
is connected and admissible while waiting for the topology-wide release; the
healthcheck does not claim training participation.

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

Build the current source under a new explicit tag from the host:

```bash
RL_CLIENT_IMAGE_TAG=p1-d3t-0.14.1 bash build_image.sh
```

The build entrypoint does not compute source, image, or binary hashes and does
not create a second stack identity. The Dockerfile compiles and packages the
current Client, configuration, and component contract. An existing tag is never
overwritten; callers must choose a new tag. The first P1 D3-T artifact is
`rl-training/maze-client:p1-d3t-0.14.0`.

## 5. Remove the development container

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
