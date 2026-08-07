# Maze Client

简体中文 | [English](README.en.md)

C++ 迷宫环境客户端。它通过 `OpenSession` 从 AIServer 获取 workload 与回放策略并执行 Episode；`local-test` 与 `model-evaluation` 会自动在 `9004` 提供本地回放，`training` 不启动回放。

## 快速开始

先从固定版本和平台的不可变制品同步 Contracts 快照，再构建镜像：

```bash
(cd ../rl-contracts && bash build_artifact.sh)
bash scripts/sync_contract_snapshot.sh
RL_CLIENT_IMAGE_TAG=training-001 bash build_image.sh
```

同步入口从 `artifact_versions.env` 读取显式的 `0.9.1` 与 `linux/arm64`，在替换前后
校验 manifest、全部制品文件和仓库快照。它不会发现 `latest`，也不会调用本机
`protoc` 重新生成代码。

进入开发容器并启动推理测试：

```bash
make shell
bash ./run.sh --aiserver maze-aiserver:9002
```

Client 不接受 workload 参数；实际模式由已连接的 AIServer 返回。
启动器同时校验 Behavior Policy scope：训练必须为 `training-fragment`，本地测试和
模型评测必须为 `evaluation-episode`。训练日志中的 `start_model` 仅表示 Episode
assignment 时的模型快照；评测日志中的 `pinned_model` 在整个 Episode 内固定。

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
