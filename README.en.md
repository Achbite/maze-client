# Maze Client

[简体中文](README.md) | English

Maze Client connects to AIServer and executes environment Episodes. In training,
start it after Learner and AIServer are ready. Evaluation requires only AIServer
to be started first.

Local training runs only three containers: Learner, AIServer, and Client.
`make shell` is a host command that prepares development artifacts from sibling
source repositories; it does not download those repositories. A fresh workspace
therefore needs at least these sibling directories:

```text
workspace/
  rl-contracts/
  rl-sample-pool/
  rl-model-distributor/
  rl-learner/
  rl-aiserver/
  maze-client/
```

The first three repositories supply development artifacts only and do not add
runtime containers. See [rl-framework](https://github.com/Achbite/rl-framework)
for the complete three-container startup order.

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

## 2. Start Client

For training, start Learner first and AIServer second, then open a third host
terminal for Client:

```bash
# Host
cd /path/to/workspace/maze-client
make shell

# Run the following commands inside the Client container
./build.sh
./run.sh --help
./run.sh --config configs/client_config.yaml --aiserver maze-aiserver:9002
```

`--help` prints the supported overrides and their config fields without
loading a map, connecting to AIServer, or starting Replay.

Client does not accept a workload argument. The actual mode comes from the AIServer `OpenSession` response.
The config file provides complete network and Replay defaults. `--aiserver`,
`--replay-dir`, and `--replay-port` only override existing `network.*` and
`viz.*` fields. `run.sh` forwards arguments byte-for-byte. Client consumes the
single `OpenSession` fact and no longer publishes a local Session-policy file.
AIServer binds the training behavior model per Agent segment; evaluation
assignments expose only the selected model-file digest to Client.

Client loads the `OpenSession`-selected `<map_id>.json` exactly from the config
default or `RL_ENV_MAP_REGISTRY_DIR`. The expected map/digest variables only
override map assertions that default to `null`. Client has no Agent-count
assertion or override; the actual count comes only from AIServer
`OpenSessionRsp.environment.agent_count`.

Infra managed mode is selected by `RL_CONFIG_PATH`. After connecting to its
paired AIServer, Client first publishes `/run/rl/readiness.json` and waits for
the owning Node to write `/run/rl/training-admission.v1.json` for the current
attempt. `run.sh` validates that token exactly against
`/run/rl/execution-identity.v1.json`, including the schema, Allocation,
NodeSession, PodAttempt, ComponentAttempt, and generation. It atomically
publishes the process gate, and the C++ Client enters the existing OpenSession
only after that gate appears. Unmanaged execution does not require Infra
admission and retains its existing behavior.

The image healthcheck follows the mode boundary: managed execution checks
`/run/rl/readiness.json`, while unmanaged execution checks that the Client
business process is still running. A managed Client can therefore report that
it is connected and admissible while waiting for the topology-wide release;
the healthcheck does not claim training participation.

## 3. View a local replay

`evaluation` records replay frames in the directory from the Client config;
Training does not record replay. The Replay HTTP service is an independent,
resident tool. It does not participate in the Client-AIServer protocol and is
not launched by `run.sh`. From the Client checkout or development container,
run:

```bash
bash ./run_replay.sh
```

The script reads `viz.output_dir` and `viz.server_port` from
`configs/client_config.yaml`. After proving that the HTTP socket is bound, it
prints one startup receipt and continues in the background. Detailed service
output is written to `log/replay-server.log`. It may be started before, during,
or after evaluation and keeps monitoring the same replay directory. Stop the
owned instance with:

```bash
bash ./run_replay.sh -stop
```

Select another Client config or override this Replay directory and port with
the corresponding options:

```bash
bash ./run_replay.sh \
  --config configs/client_config.yaml \
  --replay-dir /absolute/path/to/viz \
  --replay-port 9004
```

From the host, `make replay` and `make replay-stop` invoke the same entrypoint
inside the development container. AIServer currently assigns one evaluation
Episode. Client exits normally after it; the background Replay service remains
independent until `-stop` is invoked explicitly.

Open:

```text
http://127.0.0.1:9004/
```

## 4. Build the runtime image

Build the current source with a project tag from the host:

```bash
RL_PROJECT_IMAGE_TAG=maze-tag-001 bash build_image.sh
```

The build entrypoint does not compute source, image, or binary hashes and does
not create a second stack identity. The Dockerfile compiles and packages the
current Client, configuration, and component contract. A later tuning build may
overwrite the same tag; the full image reference is
`rl-training/maze-client:maze-tag-001`.

## 5. Remove the development container

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
