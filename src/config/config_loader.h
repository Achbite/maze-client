#pragma once

#include <string>
#include <unordered_map>

// ---- 运行参数（main 使用）----
struct RunConfig {
    // agent_num 与 workload 仅由 AIServer 的 TaskSpec 写入，不从本地配置读取。
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

// ---- 客户端完整配置 ----
struct ClientConfig {
    RunConfig     run;
    EnvConfig     env;
    NetworkConfig network;
    VizConfig     viz;
};

// ---- 配置加载器 ----
// 本地配置只拥有实例、网络、地图 registry 与 Replay 参数；任务参数缺失时不得回退。
bool LoadClientConfig(const std::string& yaml_path, ClientConfig& out_config);
