#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---- 运行参数（main 使用）----
struct RunConfig {
    // agent_num 由 AIServer EnvironmentRuntimeSpec 写入，不从本地配置读取。
    int   agent_num     = 0;
    int   log_interval  = 100;
    std::string client_instance_id;
    std::string environment_instance_id;
    std::string workload;
};

// ---- 环境参数（MazeEnv 使用）----
struct EnvConfig {
    float map_width      = 20000.0f;    // 地图宽度 (cm)
    float map_height     = 20000.0f;    // 地图高度 (cm)
    float grid_size      = 500.0f;      // 网格大小 (cm)，将连续坐标离散化为网格，支持浮点精度
    // max_steps 与 map_file 是 AIServer 分配任务后的运行时值。
    int   max_steps      = 0;
    float start_x        = 500.0f;      // 起点 X
    float start_y        = 500.0f;      // 起点 Y
    float end_x          = 19500.0f;    // 终点 X
    float end_y          = 19500.0f;    // 终点 Y
    std::string map_file;
    std::string map_registry_dir = "maps/test";
};

// ---- 网络参数 ----
struct NetworkConfig {
    std::string server_host = "127.0.0.1";  // AIServer 地址
    int         server_port = 9002;          // AIServer gRPC 端口
};

// ---- 可视化参数（帧数据记录 + HTTP 回放服务）----
struct VizConfig {
    bool        recording_enabled = false;
    std::string output_dir  = "log/viz";         // 记录文件输出目录
    int         interval    = 1;                 // 记录间隔（帧），每隔多少帧记录一次
    int         server_port = 9004;              // 可视化 HTTP 服务端口，浏览器通过此端口访问回放
};

struct ExpectedAssignmentConfig {
    std::optional<std::string> map_id;
    std::optional<std::string> map_sha256;
};

struct ClientConfigOverrides {
    std::optional<std::string> server_host;
    std::optional<int> server_port;
    std::optional<std::string> replay_output_dir;
    std::optional<int> replay_server_port;
};

// ---- 客户端完整配置 ----
struct ClientConfig {
    RunConfig     run;
    EnvConfig     env;
    NetworkConfig network;
    VizConfig     viz;
    ExpectedAssignmentConfig expected;
};

struct ClientConfigLoadReport {
    std::string config_path;
    std::vector<std::string> environment_overridden_fields;
    std::vector<std::string> cli_overridden_fields;
};

// ---- 配置加载器 ----
// 本地配置只拥有实例、网络、地图 registry 与 Replay 参数；任务参数缺失时不得回退。
bool LoadClientConfig(const std::string& yaml_path, ClientConfig& out_config);
bool LoadClientConfig(const std::string& yaml_path,
                      ClientConfig& out_config,
                      ClientConfigLoadReport& report,
                      std::string& error);
bool LoadClientConfig(const std::string& yaml_path,
                      const ClientConfigOverrides& overrides,
                      ClientConfig& out_config,
                      ClientConfigLoadReport& report,
                      std::string& error);

// 精确解析 registry 中的 <map_id>.json。registry 和地图文件都必须是
// 非符号链接，且规范化后的地图文件必须仍是 registry 的直接子文件。
std::string ResolveTaskMapFile(const std::string& registry_dir,
                               const std::string& map_id);
