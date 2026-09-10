# Maze Client

[简体中文](README.md) | English

Maze Client connects to AIServer and executes environment Episodes. In training,
start it after Learner and AIServer are ready. Evaluation requires only AIServer
to be started first.

Local training runs only three containers: Learner, AIServer, and Client.
`make shell` is a host command. When `client-dev` is absent it builds the
development image and creates and starts the container. When the container
exists, it starts it only if needed and enters it directly. It never synchronizes
or replaces this checkout's `proto/`. A complete three-container workspace has
these sibling directories:

```text
workspace/
  rl-contracts/
  rl-sample-pool/
  rl-model-distributor/
  rl-learner/
  rl-aiserver/
  maze-client/
```

The first three repositories add no runtime container. Sample Pool and Model
Distributor are staged only into Learner. `rl-contracts` changes the Client and
AIServer Maze Task Proto only through an explicit protocol-sync command. See
[rl-framework](https://github.com/Achbite/rl-framework) for the startup order.

Maze-specific code lives in `src/maze/`: `protocol/client_adapter.*` maps Proto fields to the
environment, `action/action_receipt.h` handles actions and execution receipts, and
`episode/assignment.h` handles episode assignments. `environment/`, `config/` and `viz/` contain the
Maze simulation, configuration and replay recording with map assets. `main/main.cpp` owns
configuration, signals, logging and the SDK connection. The shared SDK's `TaskClient` / `RunSession`
own RPC and lifecycle handling, with no duplicate transport implementation in this project.
Proto and its compiled artifacts remain in `proto/maze/` as the only shared wire contract;
`maze.sdk.pb.h` is generated, and task adapters do not implement gRPC calls.

## 1. Development container, incremental build, and tests

```bash
# Host: build or reuse the independent development image and enter it
make shell

# Inside the container: build and test are explicit, separate entrypoints
./build.sh
bash ./test.sh

# The host can also reuse the same container for a build
make build

# Explicitly refresh after Dockerfile.dev, toolchain, port, environment, or mount changes
make dev-refresh
```

The development image does not inherit an old runtime image and uses persistent
ccache. `ninja: no work to do.` does not automatically run tests. Tests may be
started only from the repository root with `bash ./test.sh`; `build.sh`, Docker
image builds, and other wrappers do not run them implicitly. Run `make shell`
only on the host. `make dev-image` rebuilds only the image and never replaces an
existing container. `make dev-refresh` rebuilds the image and recreates the
container. It refuses while Client, Replay, tests, or a build are active; stop
Replay with `make replay-stop` first.

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
models are also pinned internally by AIServer, so Client neither receives nor
validates model identity.

Client loads the `OpenSession`-selected `<map_id>.json` exactly from the config
default or `RL_ENV_MAP_REGISTRY_DIR`. The cross-team protocol sends only
`map_id`; map-file content, grid validity, and reachability belong to Client's
environment-load boundary. Client computes or
echoes no content hash to AIServer as a second proof. Client has no Agent-count
assertion or override; the
actual count comes only from AIServer `OpenSessionRsp.environment.agent_count`.

`OpenSessionRsp.environment.action_mask_mode` explicitly selects `disabled` or
`required`. When disabled, `AgentState.action_mask` must be empty. When required,
Client reports only the actions executable in the current environment and does
not infer AIServer policy or Learner training behavior.

## 3. View a local replay

`evaluation` records replay frames in the directory from the Client config;
Training does not record replay. The Replay HTTP service is an independent,
resident tool. It does not participate in the Client-AIServer protocol and is
not launched by `run.sh`. From inside the Client development container, run:

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
inside the development container. Do not start another host-side 9004 service
that competes with the container port forwarding. AIServer currently assigns one evaluation
Episode. Client exits normally after it; the background Replay service remains
independent until `-stop` is invoked explicitly.

Open:

```text
http://127.0.0.1:9004/
```

## 4. Build the runtime image

The runtime image compiles the repository-local `proto/`. Normal builds do not
read the Contracts repository or compare Client/AIServer source, generator,
hash, or platform identity. Only when intentionally adopting the current Maze
release should you run the Framework command and review this repository's diff:

```bash
(cd ../rl-framework && bash sync_maze_protocol.sh)
```

Then build the current source with a project tag from the host:

```bash
RL_PROJECT_IMAGE_TAG=maze-tag-001 bash build_image.sh
```

The build entrypoint does not compute source, image, or binary hashes and does
not create a second stack identity. The Dockerfile compiles and packages the
current Client, configuration, maps, and Replay tools. A later tuning build may
overwrite the same tag; the full image reference is
`rl-training/maze-client:maze-tag-001`.

## 5. Refresh or remove the development container

```bash
make dev-refresh
make dev-clean
```

`dev-refresh` preserves source and the ccache volume while replacing the
development image/container environment. `dev-clean` removes the development
container. Neither command synchronizes protocols.

## License

[MIT License](LICENSE)
