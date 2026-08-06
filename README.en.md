# Maze Client

English | [简体中文](README.md)

C++ maze environment client. It obtains the workload and replay policy from AIServer through `OpenSession`, then executes episodes. `local-test` and `model-evaluation` automatically expose local replay on `9004`; `training` does not start replay.

## Quick Start

Explicitly refresh the Contracts snapshot, then build the image:

```bash
(cd ../rl-contracts && bash build_artifact.sh)
artifact=../.workspace/artifacts/rl-contracts/0.8.0/linux-arm64
cp "${artifact}/common.proto" "${artifact}/maze_task.proto" proto/
cp "${artifact}"/cpp/common.pb.{cc,h} proto/
cp "${artifact}"/cpp/maze_task.pb.{cc,h} proto/
cp "${artifact}"/cpp/maze_task.grpc.pb.{cc,h} proto/
cp "${artifact}/manifest.json" proto/
RL_CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

Enter the development container and start the inference smoke test:

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

The Client does not accept a workload argument; the connected AIServer returns the active mode.

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
