# Maze Client

English | [简体中文](README.md)

C++ maze environment client. It obtains the workload and replay policy from AIServer through `OpenSession`, then executes episodes. `local-test` and `model-evaluation` automatically expose local replay on `9004`; `training` does not start replay.

## Quick Start

Synchronize the Contracts snapshot from the immutable artifact selected by an
explicit version and platform, then build the image:

```bash
(cd ../rl-contracts && bash build_artifact.sh)
bash scripts/sync_contract_snapshot.sh
RL_CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

The synchronization entrypoint reads the explicit `0.10.0` and `linux/arm64`
identity from `artifact_versions.env`, verifies the manifest, every artifact
file, and the staged snapshot before and after replacement. It neither discovers
`latest` nor invokes a host `protoc` to regenerate code.

Enter the development container and start the inference smoke test:

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

The Client does not accept a workload argument; the connected AIServer returns the active mode.
The launcher also validates Behavior Policy scope: training requires
`training-fragment`, while local test and model evaluation require
`evaluation-episode`. `start_model` in training logs is only the model snapshot
at Episode assignment; `pinned_model` in evaluation logs remains fixed for the
whole Episode.

View previously recorded replay files:

```bash
bash ./replay.sh local-test
```

Browser URL:

```text
http://127.0.0.1:9004/
```

## Tests

```bash
make test
```

## License

[MIT License](LICENSE)
