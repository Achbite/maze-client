#pragma once

#include <vector>
#include <cstdint>
#include <string>

// 前向声明
struct EnvConfig;

// ---- 墙壁线段数据（从地图 JSON 加载）----
struct WallSegment {
    float x1, y1, x2, y2;      // 线段端点坐标 (cm)
    float thickness;            // 墙壁厚度 (cm)
};

// ---- 8 方向射线检测结果 ----
struct RayResult {
    float distances[8];         // 8 方向归一化射线距离（0=紧贴墙壁，1=最大探测距离内无障碍）
};

enum class AgentTerminationReason {
    Active,
    GoalReached,
    TimeLimit,
};

// --- 网格级动作方向表 ---
// action_id 0-8 对应的网格偏移 (dx, dy)，一次移动即 +1/-1
constexpr int kGridActionDirs[9][2] = {
    { 0,  0},         // 0: 不动
    { 0,  1},         // 1: 上 (Y+)
    { 1,  1},         // 2: 右上
    { 1,  0},         // 3: 右 (X+)
    { 1, -1},         // 4: 右下
    { 0, -1},         // 5: 下 (Y-)
    {-1, -1},         // 6: 左下
    {-1,  0},         // 7: 左 (X-)
    {-1,  1},         // 8: 左上
};

// ---- 单个 Agent 的运行时状态 ----
struct AgentInfo {
    int   id      = 0;
    int   grid_x  = 0;         // 网格 X 坐标
    int   grid_y  = 0;         // 网格 Y 坐标
    bool  done    = false;     // 是否已结束（到达终点或超时）
    bool  last_move_blocked = false;
    AgentTerminationReason termination_reason = AgentTerminationReason::Active;
};

// ---- 客户端完整配置（前向声明引用）----
struct ClientConfig;

// ---- 迷宫环境模拟器 ----
class MazeEnv {
public:
    // 初始化（从配置加载所有参数）
    bool Init(const ClientConfig& config);               // 从完整配置初始化
    void Reset();                                         // 重置所有 Agent 到起点
    void SetMaxSteps(int max_steps);

    // 帧更新
    void Step(int agent_id, int action_id);               // 执行网格级移动，检查可达性
    void AdvanceFrame();                                  // 帧号递增（所有 Agent Step 完后调用）

    // 查询
    const AgentInfo& GetAgent(int agent_id) const;        // 获取 Agent 状态
    int   GetFrameId() const;                             // 当前帧号
    bool  AllDone() const;                                // 所有 Agent 是否都已结束
    int   GetAgentNum() const;                             // Agent 数量

    // 地图参数
    float GetMapWidth()  const { return map_width_; }
    float GetMapHeight() const { return map_height_; }
    float GetStartX() const { return start_x_; }
    float GetStartY() const { return start_y_; }
    float GetEndX()   const { return end_x_; }
    float GetEndY()   const { return end_y_; }
    float GetGridSize() const { return grid_size_; }
    std::uint32_t GetGridSizeMicrounits() const;
    int   GetGridCols() const { return grid_cols_; }
    int   GetGridRows() const { return grid_rows_; }
    int   GetStartGridX() const { return start_gx_; }
    int   GetStartGridY() const { return start_gy_; }
    int   GetGoalGridX() const { return end_gx_; }
    int   GetGoalGridY() const { return end_gy_; }
    int   GetMapFormatVersion() const { return map_format_version_; }
    int   GetShortestActionSteps() const { return shortest_action_steps_; }
    const std::string& GetMapChecksum() const { return map_checksum_sha256_; }
    const std::string& GetActionRuleId() const { return action_rule_id_; }
    std::string GetBlockedBitmap() const;

    // 网格坐标 → 连续坐标（网格中心，用于可视化和通信）
    float GetWorldX(int gx) const { return (gx + 0.5f) * grid_size_; }
    float GetWorldY(int gy) const { return (gy + 0.5f) * grid_size_; }

    // 连续坐标 → 网格坐标
    int ToGridX(float x) const;
    int ToGridY(float y) const;

    // 网格是否可通行
    bool IsWalkable(int gx, int gy) const;

    // 8 方向射线检测（返回归一化距离，用于构建 obs 特征）
    RayResult CastRays(int gx, int gy, int max_range = 10) const;

    // 获取墙壁线段列表（用于可视化 JSON 输出）
    const std::vector<WallSegment>& GetWalls() const { return walls_; }

    // 获取当前加载的地图 ID（来自 AIServer 指定的 registry 文件）
    const std::string& GetMapId() const { return map_id_; }

    // 获取当前加载的地图文件路径
    const std::string& GetMapFilePath() const { return loaded_map_path_; }

private:
    std::vector<AgentInfo> agents_;

    // --- 地图参数（从配置加载）---
    float map_width_      = 20000.0f;   // 地图宽度 (cm)
    float map_height_     = 20000.0f;   // 地图高度 (cm)
    int   max_steps_      = 0;          // 由 BeginEpisode 分配
    float start_x_        = 500.0f;     // 起点 X（连续坐标）
    float start_y_        = 500.0f;     // 起点 Y（连续坐标）
    float end_x_          = 19500.0f;   // 终点 X（连续坐标）
    float end_y_          = 19500.0f;   // 终点 Y（连续坐标）
    int   frame_id_       = 0;          // 当前帧号

    // --- 网格参数 ---
    float grid_size_      = 500.0f;     // 网格大小 (cm)，支持浮点精度
    std::uint32_t grid_size_microunits_ = 0;  // canonical v4 精确整数值
    int   grid_cols_      = 0;          // 网格列数
    int   grid_rows_      = 0;          // 网格行数
    int   start_gx_       = 0;          // 起点网格 X
    int   start_gy_       = 0;          // 起点网格 Y
    int   end_gx_         = 0;          // 终点网格 X
    int   end_gy_         = 0;          // 终点网格 Y

    // --- 网格障碍物（true=不可通行）---
    std::vector<bool> blocked_;

    // --- 墙壁线段列表（从地图 JSON 加载，用于可视化输出）---
    std::vector<WallSegment> walls_;

    // --- AIServer 分配的地图文件 ---
    std::string map_file_;
    std::string map_id_;                // 当前地图 ID（来自 JSON 的 map_id 字段）
    std::string loaded_map_path_;       // 实际加载的地图文件完整路径
    int map_format_version_ = 0;
    int shortest_action_steps_ = -1;
    std::string map_checksum_sha256_;
    std::string action_rule_id_ = "maze.action.9-way.no-corner-cut.v1";
    bool has_authoritative_grid_ = false;

    // 从 JSON 文件加载地图数据（墙壁、起终点、尺寸、grid_size、grid_count 等）
    bool LoadMapFromFile(const std::string& filepath);

    // 终止判定
    bool CheckGoalReached(const AgentInfo& agent) const;  // 是否到达终点网格
    bool CheckTimeout() const;                             // 是否超时
    int ComputeShortestActionSteps() const;
    std::string ComputeCanonicalChecksum() const;
};
