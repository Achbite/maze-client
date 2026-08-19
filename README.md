# Maze Client

简体中文 | [English](README.en.md)

Maze Client 连接 AIServer 并执行环境 Episode。A3 本地链由开发者分别启动 Learner、
AIServer 与 Client；Framework 不再编排运行时。

## 1. 开发容器、增量构建与测试

```bash
# 宿主机：自动构建或复用独立开发镜像并进入容器
make shell

# 容器内：构建与测试是两个显式入口
./build.sh
bash ./test.sh

# 宿主机也可复用同一容器执行构建
make build
```

开发容器不继承旧 runtime image，使用持久 ccache。`ninja: no work to do.` 不会自动触发测试。
测试只能在仓库根通过 `bash ./test.sh` 启动；`build.sh`、Docker image 构建和其他 wrapper
不会隐式运行测试。`make shell` 只能在宿主机执行。

## 2. 手工连接 AIServer

```bash
make shell
./build.sh
./run.sh --help
./run.sh --config configs/client_config.yaml --aiserver maze-aiserver:9002
```

`--help` 只打印实际支持的覆盖项及对应 config 字段，不加载地图、连接 AIServer 或启动 Replay。

Client 不接受 workload 参数；实际模式由 AIServer 的 `OpenSession` 响应决定。
`configs/client_config.yaml` 提供完整网络与 Replay 默认值；`--aiserver`、`--replay-dir`、
`--replay-port` 只覆盖既有 `network.*`/`viz.*` 字段。相对目录相对所选 config 文件解析。
`run.sh` 不解析这些业务参数，而是逐字节转发给 C++ 配置层；C++ 再把最终 Replay 目录和端口
随 Session policy 交给监督器，因此记录器和 Replay HTTP 服务不会使用两套默认值。
Training assignment 必须携带 lineage、显式 `model_step` 与模型/manifest digest；evaluation
assignment 只携带模型文件 digest，不伪造训练 step 或 lineage。

Client 从 config 默认值或 `RL_ENV_MAP_REGISTRY_DIR` 覆盖的 registry 精确加载 TaskSpec 下发的
`<map_id>.json`；`RL_EXPECTED_TASK_MAP_ID/SHA256` 与 `RL_EXPECTED_AGENT_COUNT` 只覆盖
config 中默认是 `null` 的 `expected.*` 断言，不改变 AIServer 下发的运行事实。

## 3. 查看本地回放

`evaluation` 默认使用 config 中的回放端口 `9004`，Training 不启动回放。
evaluation 的 `validation-manifest.json` 只记录所用模型的 SHA-256，不写训练 step。

Replay 由 `run.sh` 在收到 evaluation Session policy 后自动启动；`replay.sh` 只接受 C++
effective config handoff 的目录和端口，不再提供独立隐藏默认值。

浏览器打开：

```text
http://127.0.0.1:9004/
```

## 4. 正式制品与镜像

只有 Level 1/2 通过、用户 Review 并形成 clean savepoint 后，才同步正式 Contracts artifact
并在宿主机执行 `bash build_image.sh`。正式构建不读取开发 artifact 或开发容器 build 目录。

## 5. 清理开发容器

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
