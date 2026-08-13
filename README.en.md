# Maze Client

[简体中文](README.md) | English

Maze Client connects to AIServer and executes Episodes. Start the full chain from [rl-framework](../rl-framework/README.en.md).

## 1. Prepare Contracts and build the image

```bash
(cd ../rl-contracts && bash build_artifact.sh)
bash scripts/sync_contract_snapshot.sh
RL_CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

## 2. Incremental build and tests

```bash
# Build the development image
RL_CLIENT_IMAGE_TAG=training-001 make dev-image

# Incrementally build only the main executable; do not run CTest
make build

# Build test targets and run CTest
make test

# Focus tests
TEST_PATTERN=lifecycle make test

# Full build, full CTest, and auxiliary checks
make verify
```

The development container uses persistent ccache. `ninja: no work to do.` does not automatically run tests.

## 3. Connect to AIServer manually

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

Client does not accept a workload argument. The actual mode comes from the AIServer `OpenSession` response.

## 4. View a local replay

`local-test` and `model-evaluation` use replay port `9004`; Training does not start replay.

```bash
bash ./replay.sh local-test
```

Open:

```text
http://127.0.0.1:9004/
```

## 5. Remove the development container

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
