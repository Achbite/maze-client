# Maze Client

简体中文 | [English](README.en.md)

C++ 迷宫环境客户端。它通过 `OpenSession` 从 AIServer 获取 workload 与回放策略并执行 Episode；`local-test` 与 `model-evaluation` 会自动在 `9004` 提供本地回放，`training` 不启动回放。

## 快速开始

先显式更新 Contracts 快照，再构建镜像：

```bash
(cd ../rl-contracts && bash build_artifact.sh)
cp ../.workspace/artifacts/rl-contracts/0.6.0/linux-arm64/maze.proto proto/
cp ../.workspace/artifacts/rl-contracts/0.6.0/linux-arm64/cpp/* proto/
cp ../.workspace/artifacts/rl-contracts/0.6.0/linux-arm64/manifest.json proto/
CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

进入开发容器并启动推理测试：

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

Client 不接受 workload 参数；实际模式由已连接的 AIServer 返回。

单独查看已经保存的回放：

```bash
bash ./replay.sh local-test
```

浏览器地址：

```text
http://127.0.0.1:9004/
```

## 测试

```bash
make test
```

## License

[MIT License](LICENSE)
