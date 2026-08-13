# Maze Client

简体中文 | [English](README.en.md)

Maze Client 连接 AIServer 并执行 Episode。完整链路请从 [rl-framework](../rl-framework/README.md) 启动。

## 1. 准备 Contracts 并构建镜像

```bash
(cd ../rl-contracts && bash build_artifact.sh)
bash scripts/sync_contract_snapshot.sh
RL_CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

## 2. 增量构建与测试

```bash
# 构建开发镜像
RL_CLIENT_IMAGE_TAG=training-001 make dev-image

# 只增量编译主程序，不运行 CTest
make build

# 构建测试目标并运行 CTest
make test

# 聚焦测试
TEST_PATTERN=lifecycle make test

# 完整构建、完整 CTest 和辅助检查
make verify
```

开发容器使用持久 ccache。`ninja: no work to do.` 不会自动触发测试。

## 3. 手工连接 AIServer

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

Client 不接受 workload 参数；实际模式由 AIServer 的 `OpenSession` 响应决定。

## 4. 查看本地回放

`local-test` 和 `model-evaluation` 使用回放端口 `9004`，Training 不启动回放。

```bash
bash ./replay.sh local-test
```

浏览器打开：

```text
http://127.0.0.1:9004/
```

## 5. 清理开发容器

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
