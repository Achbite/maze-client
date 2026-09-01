# Maze Client

简体中文 | [English](README.en.md)

Maze Client 连接 AIServer 并执行环境 Episode。训练时在 Learner、AIServer ready 后启动；
评测时只需要先启动 AIServer。

本地训练只运行 Learner、AIServer 和 Client 三个容器。`make shell` 是宿主机命令：没有
`client-dev` 时构建开发镜像、创建并启动容器；容器存在时只在必要时启动它，然后直接进入。
它不会同步或覆盖本仓 `proto/`。完整三容器工作区包含以下同级目录：

```text
workspace/
  rl-contracts/
  rl-sample-pool/
  rl-model-distributor/
  rl-learner/
  rl-aiserver/
  maze-client/
```

前三个仓库不增加运行容器。Sample Pool/Model Distributor 只装配到 Learner；`rl-contracts` 的
Maze Task Proto 只有在开发者显式执行协议同步时才会更新 Client/AIServer。完整启动顺序参阅
[rl-framework](https://github.com/Achbite/rl-framework)。

## 1. 开发容器、增量构建与测试

```bash
# 宿主机：自动构建或复用独立开发镜像并进入容器
make shell

# 容器内：构建与测试是两个显式入口
./build.sh
bash ./test.sh

# 宿主机也可复用同一容器执行构建
make build

# Dockerfile.dev、工具链、端口、环境变量或挂载变化后显式刷新
make dev-refresh
```

开发容器不继承旧 runtime image，使用持久 ccache。`ninja: no work to do.` 不会自动触发测试。
测试只能在仓库根通过 `bash ./test.sh` 启动；`build.sh`、Docker image 构建和其他 wrapper
不会隐式运行测试。`make shell` 只能在宿主机执行。`make dev-image` 只重建镜像，不替换已有容器；
`make dev-refresh` 才会重建镜像并重建容器。若 Client、Replay、测试或编译仍在运行，刷新会明确
失败；先停止对应进程，Replay 使用 `make replay-stop`。

## 2. 启动 Client

训练时先启动 Learner，再启动 AIServer，最后打开第三个宿主终端启动 Client：

```bash
# 宿主机
cd /path/to/workspace/maze-client
make shell

# 以下命令在 Client 容器内执行
./build.sh
./run.sh --help
./run.sh --config configs/client_config.yaml --aiserver maze-aiserver:9002
```

`--help` 只打印实际支持的覆盖项及对应 config 字段，不加载地图、连接 AIServer 或启动 Replay。

Client 不接受 workload 参数；实际模式由 AIServer 的 `OpenSession` 响应决定。
`configs/client_config.yaml` 提供完整网络与 Replay 默认值；`--aiserver`、`--replay-dir`、
`--replay-port` 只覆盖既有 `network.*`/`viz.*` 字段。相对目录相对所选 config 文件解析。
`run.sh` 不解析这些业务参数，而是逐字节转发给 C++ 配置层；Client 只消费这一个
`OpenSession` 事实，不再生成本地 Session-policy 文件。Training 的行为模型由 AIServer
按 Agent segment 绑定；evaluation 模型也只由 AIServer 内部固定，Client 不接收或校验模型身份。

Client 从 config 默认值或 `RL_ENV_MAP_REGISTRY_DIR` 覆盖的 registry 精确加载 `OpenSession` 下发的
`<map_id>.json`。跨团队协议只下发 `map_id`；地图文件内容、自描述 checksum、网格合法性和可达性
由 Client 环境加载边界负责，不回传给 AIServer 重复证明。Client 不再提供 Agent 数断言或覆盖；
实际数量只来自 AIServer `OpenSessionRsp.environment.agent_count`。

`OpenSessionRsp.environment.action_mask_mode` 明确选择 mask 为 `disabled` 或 `required`。关闭时
`AgentState.action_mask` 必须为空；开启时 Client 只按当前环境生成可执行动作事实并随状态回传，
不推断 AIServer 的策略或 Learner 训练逻辑。

Infra managed 模式由 `RL_CONFIG_PATH` 标识。Client 连接配对 AIServer 后先发布
`/run/rl/readiness.json`，随后等待 Node 写入当前 attempt 的
`/run/rl/training-admission.json`。`run.sh` 将 token 与
`/run/rl/execution-identity.json` 做精确 schema、Allocation、NodeSession、PodAttempt、
ComponentAttempt 和 generation 校验，原子发布本进程使用的 gate；C++ Client 看到 gate 后才进入既有
`OpenSession`。非 managed 模式不要求 Infra admission，行为保持不变。

镜像 healthcheck 按运行模式检查事实：managed 模式检查 `/run/rl/readiness.json`，非 managed
模式检查 Client 业务进程仍在运行。managed Client 在等待整体训练放行时仍正确表示其“已连接、
可被 Controller 放行”的 readiness，healthcheck 本身不伪造 training participation。

## 3. 查看本地回放

`evaluation` 使用 Client config 中的目录记录回放帧；Training 不记录回放。Replay HTTP 服务是独立
常驻工具，不参与 Client↔AIServer 协议，也不由 `run.sh` 自动启动。进入 Client 开发容器后执行：

```bash
bash ./run_replay.sh
```

脚本读取 `configs/client_config.yaml` 的 `viz.output_dir` 与 `viz.server_port`，确认 HTTP socket 已绑定后
只输出一行启动回执并转入后台；服务详细日志写入 `log/replay-server.log`。它可以在 evaluation 前、期间或
结束后随时启动，并持续监控同一回放目录。停止该实例：

```bash
bash ./run_replay.sh -stop
```

需要选择其他 Client config 或显式覆盖本次 Replay 目录、端口时使用同名参数：

```bash
bash ./run_replay.sh \
  --config configs/client_config.yaml \
  --replay-dir /absolute/path/to/viz \
  --replay-port 9004
```

宿主机通过开发容器启动或停止同一个入口时，分别使用 `make replay` 与 `make replay-stop`；不要在
宿主仓库直接另起一个 9004 服务与容器端口转发竞争。当前
evaluation 由 AIServer 下发一个 Episode；Client 完成后正常退出，后台 Replay 服务保持独立，直到
显式执行 `-stop`。

浏览器打开：

```text
http://127.0.0.1:9004/
```

## 4. 构建运行镜像

运行镜像直接编译仓库本地 `proto/`。普通构建不读取 Contracts 仓，也不比较 Client/AIServer 的
源码、生成器、哈希或平台。只有明确决定采用 Contracts 仓当前 Maze release 时，才从 Framework
执行以下命令并审查本仓 diff：

```bash
(cd ../rl-framework && bash sync_maze_protocol.sh)
```

随后在宿主机用项目 tag 构建当前源码：

```bash
RL_PROJECT_IMAGE_TAG=maze-tag-001 bash build_image.sh
```

构建入口不计算 source/image/binary 哈希，也不生成第二套 stack identity。它直接由 Dockerfile 编译并
封装当前 Client、配置和 component contract。同名 tag 允许由后续微调构建直接覆盖；完整镜像引用为
`rl-training/maze-client:maze-tag-001`。

## 5. 刷新与清理开发容器

```bash
make dev-refresh
make dev-clean
```

`dev-refresh` 保留源码与 ccache volume，只替换开发镜像/容器环境；`dev-clean` 删除开发容器。
两者都不会同步协议。

## License

[MIT License](LICENSE)
