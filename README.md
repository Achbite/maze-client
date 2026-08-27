# Maze Client

简体中文 | [English](README.en.md)

Maze Client 连接 AIServer 并执行环境 Episode。训练时在 Learner、AIServer ready 后启动；
评测时只需要先启动 AIServer。

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
`<map_id>.json`；`RL_EXPECTED_TASK_MAP_ID/SHA256` 只覆盖 config 中默认是 `null` 的地图断言。
Client 不再提供 Agent 数断言或覆盖；实际数量只来自 AIServer
`OpenSessionRsp.EnvironmentRuntimeSpec.agent_count`。

Infra managed 模式由 `RL_CONFIG_PATH` 标识。Client 连接配对 AIServer 后先发布
`/run/rl/readiness.json`，随后等待 Node 写入当前 attempt 的
`/run/rl/training-admission.v1.json`。`run.sh` 将 token 与
`/run/rl/execution-identity.v1.json` 做精确 schema、Allocation、NodeSession、PodAttempt、
ComponentAttempt 和 generation 校验，原子发布本进程使用的 gate；C++ Client 看到 gate 后才进入既有
`OpenSession`。非 managed 模式不要求 Infra admission，行为保持不变。

镜像 healthcheck 按同一个运行模式读取事实：managed 模式检查
`/run/rl/readiness.json`，非 managed 模式检查既有 `/tmp/rl-client-session-policy`。因此 managed Client
在等待整体训练放行时仍正确表示其“已连接、可被 Controller 放行”的 readiness，healthcheck 本身不伪造
training participation。

## 3. 查看本地回放

`evaluation` 默认使用 config 中的回放端口 `9004`，Training 不启动回放。
evaluation 的 `validation-manifest.json` 只记录所用模型的 SHA-256，不写训练 step。

Replay 由 `run.sh` 在收到 evaluation Session policy 后自动启动；`replay.sh` 只接受 C++
effective config handoff 的目录和端口，不再提供独立隐藏默认值。

当前 evaluation 由 AIServer 下发一个 Episode。Episode 完成后 Client 业务进程正常退出，
`run.sh` 继续保留 Replay HTTP 服务；按 `Ctrl-C` 后停止 Replay 并返回终端。该常驻状态不是
Client 自动重启。

浏览器打开：

```text
http://127.0.0.1:9004/
```

## 4. 构建运行镜像

在宿主机用一个新的、显式 tag 构建当前源码：

```bash
RL_CLIENT_IMAGE_TAG=p1-d3t-0.14.1 bash build_image.sh
```

构建入口不计算 source/image/binary 哈希，也不生成第二套 stack identity。它直接由 Dockerfile 编译并
封装当前 Client、配置和 component contract；若 tag 已存在则拒绝覆盖，调用方必须选择新 tag。P1 首个
D3-T 制品是 `rl-training/maze-client:p1-d3t-0.14.0`。

## 5. 清理开发容器

```bash
make dev-clean
```

## License

[MIT License](LICENSE)
