#include "env/maze_env.h"
#include "config/config_loader.h"
#include "log/logger.h"

#include <cmath>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <deque>
#include <limits>

// ---- 从完整配置初始化环境 ----
bool MazeEnv::Init(const ClientConfig& config) {
    const EnvConfig& env = config.env;

    // 地图必须由 AIServer TaskSpec 精确解析，禁止随机与默认回退。
    map_file_ = env.map_file;
    if (map_file_.empty() || config.run.agent_num <= 0) {
        LOG_ERROR("MazeEnv", "缺少 AIServer 分配的地图或 Agent 数");
        return false;
    }

    // 加载地图参数（先设默认值，地图文件可能覆盖）
    map_width_     = env.map_width;
    map_height_    = env.map_height;
    max_steps_     = env.max_steps;
    start_x_       = env.start_x;
    start_y_       = env.start_y;
    end_x_         = env.end_x;
    end_y_         = env.end_y;
    grid_size_     = env.grid_size;
    grid_size_microunits_ = 0;

    // 计算网格尺寸（默认值，地图文件可能覆盖）
    grid_cols_ = static_cast<int>(std::ceil(map_width_ / grid_size_));
    grid_rows_ = static_cast<int>(std::ceil(map_height_ / grid_size_));

    // 初始化网格障碍物
    blocked_.assign(grid_cols_ * grid_rows_, false);
    walls_.clear();
    map_id_.clear();
    loaded_map_path_.clear();
    shortest_action_steps_ = -1;
    has_authoritative_grid_ = false;

    if (!LoadMapFromFile(map_file_)) {
        LOG_ERROR("MazeEnv", "任务地图加载失败: %s", map_file_.c_str());
        return false;
    }
    loaded_map_path_ = map_file_;
    LOG_INFO("MazeEnv", "任务地图加载成功: %s", map_file_.c_str());

    // 计算起点/终点的网格坐标（地图文件可能已覆盖起终点坐标）
    const int position_start_gx = ToGridX(start_x_);
    const int position_start_gy = ToGridY(start_y_);
    const int position_end_gx = ToGridX(end_x_);
    const int position_end_gy = ToGridY(end_y_);
    if (has_authoritative_grid_) {
        if (start_gx_ != position_start_gx ||
            start_gy_ != position_start_gy ||
            end_gx_ != position_end_gx ||
            end_gy_ != position_end_gy) {
            LOG_ERROR("MazeEnv", "地图网格起终点与可视化坐标不一致");
            return false;
        }
    } else {
        start_gx_ = position_start_gx;
        start_gy_ = position_start_gy;
        end_gx_ = position_end_gx;
        end_gy_ = position_end_gy;
    }

    if (!IsWalkable(start_gx_, start_gy_) ||
        !IsWalkable(end_gx_, end_gy_)) {
        LOG_ERROR("MazeEnv", "地图阻塞了起点或终点");
        return false;
    }
    const int computed_shortest = ComputeShortestActionSteps();
    if (computed_shortest <= 0) {
        LOG_ERROR("MazeEnv", "地图起点无法到达终点");
        return false;
    }
    shortest_action_steps_ = computed_shortest;

    // 初始化 Agent
    int agent_num = config.run.agent_num;
    agents_.resize(agent_num);
    for (int i = 0; i < agent_num; ++i) {
        agents_[i].id     = i;
        agents_[i].grid_x = start_gx_;
        agents_[i].grid_y = start_gy_;
        agents_[i].done   = false;
        agents_[i].last_move_blocked = false;
        agents_[i].termination_reason = AgentTerminationReason::Active;
    }

    frame_id_ = 0;

    LOG_INFO("MazeEnv", "初始化完成: agent_num=%d, grid=%dx%d (格子=%.2fcm), "
                "start_grid=(%d,%d), end_grid=(%d,%d), max_steps=%d",
                agent_num, grid_cols_, grid_rows_, grid_size_,
                start_gx_, start_gy_, end_gx_, end_gy_, max_steps_);
    return true;
}

void MazeEnv::SetMaxSteps(int max_steps) {
    if (max_steps > 0) max_steps_ = max_steps;
}

std::uint32_t MazeEnv::GetGridSizeMicrounits() const {
    return grid_size_microunits_;
}

std::string MazeEnv::GetBlockedBitmap() const {
    std::string bitmap;
    bitmap.resize(blocked_.size());
    for (std::size_t index = 0; index < blocked_.size(); ++index) {
        bitmap[index] = blocked_[index] ? '\x01' : '\x00';
    }
    return bitmap;
}

int MazeEnv::ComputeShortestActionSteps() const {
    if (grid_cols_ <= 0 || grid_rows_ <= 0) return -1;
    const int start = start_gy_ * grid_cols_ + start_gx_;
    const int goal = end_gy_ * grid_cols_ + end_gx_;
    std::vector<int> distance(
        static_cast<std::size_t>(grid_cols_ * grid_rows_), -1);
    std::deque<int> queue;
    distance[start] = 0;
    queue.push_back(start);
    while (!queue.empty()) {
        const int current = queue.front();
        queue.pop_front();
        if (current == goal) return distance[current];
        const int gx = current % grid_cols_;
        const int gy = current / grid_cols_;
        for (int action = 1; action < 9; ++action) {
            const int dx = kGridActionDirs[action][0];
            const int dy = kGridActionDirs[action][1];
            const int nx = gx + dx;
            const int ny = gy + dy;
            if (!IsWalkable(nx, ny)) continue;
            if (dx != 0 && dy != 0 &&
                (!IsWalkable(gx + dx, gy) ||
                 !IsWalkable(gx, gy + dy))) {
                continue;
            }
            const int next = ny * grid_cols_ + nx;
            if (distance[next] >= 0) continue;
            distance[next] = distance[current] + 1;
            queue.push_back(next);
        }
    }
    return -1;
}

// ---- 重置所有 Agent 到起点 ----
void MazeEnv::Reset() {
    for (auto& agent : agents_) {
        agent.grid_x = start_gx_;
        agent.grid_y = start_gy_;
        agent.done   = false;
        agent.last_move_blocked = false;
        agent.termination_reason = AgentTerminationReason::Active;
    }
    frame_id_ = 0;
}

// ---- 执行网格级移动 ----
bool MazeEnv::Step(int agent_id, int action_id, std::string& error) {
    error.clear();
    if (agent_id < 0 || agent_id >= static_cast<int>(agents_.size())) {
        error = "agent_id is outside the environment assignment";
        return false;
    }

    AgentInfo& agent = agents_[agent_id];

    // 已结束的 Agent 不再移动
    if (agent.done) {
        error = "AIServer returned an action for a terminal Agent";
        return false;
    }

    // 动作范围校验
    if (action_id < 0 || action_id > 8) {
        error = "action_id is outside the Maze action contract";
        return false;
    }

    // ---- 1. 计算目标网格 ----
    int dx = kGridActionDirs[action_id][0];
    int dy = kGridActionDirs[action_id][1];
    int new_gx = agent.grid_x + dx;
    int new_gy = agent.grid_y + dy;

    // ---- 2. 可达性检查 ----
    const bool can_move = IsActionAvailable(agent, action_id);

    // ---- 3. 执行移动 ----
    if (can_move) {
        agent.grid_x = new_gx;
        agent.grid_y = new_gy;
    }
    agent.last_move_blocked = action_id != 0 && !can_move;
    // 不可达则保持原位，AIServer 下一帧会根据新状态重新决策

    // ---- 4. 终止判定 ----
    if (CheckGoalReached(agent)) {
        agent.done = true;
        agent.termination_reason = AgentTerminationReason::GoalReached;
    } else if (CheckTimeout()) {
        agent.done = true;
        agent.termination_reason = AgentTerminationReason::TimeLimit;
    }
    return true;
}

// ---- 帧号递增 ----
void MazeEnv::AdvanceFrame() {
    ++frame_id_;
}

// ---- 获取 Agent 状态 ----
const AgentInfo& MazeEnv::GetAgent(int agent_id) const {
    return agents_[agent_id];
}

// ---- 当前帧号 ----
int MazeEnv::GetFrameId() const {
    return frame_id_;
}

// ---- 所有 Agent 是否都已结束 ----
bool MazeEnv::AllDone() const {
    for (const auto& agent : agents_) {
        if (!agent.done) return false;
    }
    return true;
}

// ---- Agent 数量 ----
int MazeEnv::GetAgentNum() const {
    return static_cast<int>(agents_.size());
}

std::vector<bool> MazeEnv::GetActionMask(int agent_id) const {
    std::vector<bool> mask(9, false);
    if (agent_id < 0 || agent_id >= static_cast<int>(agents_.size())) {
        return mask;
    }
    const auto& agent = agents_[static_cast<std::size_t>(agent_id)];
    for (int action_id = 0; action_id < 9; ++action_id) {
        mask[static_cast<std::size_t>(action_id)] =
            IsActionAvailable(agent, action_id);
    }
    return mask;
}

// ---- 连续坐标 → 网格 X ----
int MazeEnv::ToGridX(float x) const {
    int gx = static_cast<int>(std::floor(x / grid_size_));
    return std::max(0, std::min(grid_cols_ - 1, gx));
}

// ---- 连续坐标 → 网格 Y ----
int MazeEnv::ToGridY(float y) const {
    int gy = static_cast<int>(std::floor(y / grid_size_));
    return std::max(0, std::min(grid_rows_ - 1, gy));
}

// ---- 网格是否可通行 ----
bool MazeEnv::IsWalkable(int gx, int gy) const {
    if (gx < 0 || gx >= grid_cols_ || gy < 0 || gy >= grid_rows_) {
        return false;
    }
    return !blocked_[gy * grid_cols_ + gx];
}

bool MazeEnv::IsActionAvailable(const AgentInfo& agent, int action_id) const {
    if (agent.done || action_id < 0 || action_id >= 9) return false;
    const int dx = kGridActionDirs[action_id][0];
    const int dy = kGridActionDirs[action_id][1];
    const int target_x = agent.grid_x + dx;
    const int target_y = agent.grid_y + dy;
    if (!IsWalkable(target_x, target_y)) return false;
    if (dx != 0 && dy != 0 &&
        (!IsWalkable(agent.grid_x + dx, agent.grid_y) ||
         !IsWalkable(agent.grid_x, agent.grid_y + dy))) {
        return false;
    }
    return true;
}

// ---- 是否到达终点网格 ----
bool MazeEnv::CheckGoalReached(const AgentInfo& agent) const {
    return agent.grid_x == end_gx_ && agent.grid_y == end_gy_;
}

// ---- 是否超时 ----
bool MazeEnv::CheckTimeout() const {
    return frame_id_ + 1 >= max_steps_;
}

// ---- 从 JSON 文件加载地图数据 ----
bool MazeEnv::LoadMapFromFile(const std::string& filepath) {
    std::ifstream ifs(filepath);
    if (!ifs.is_open()) {
        LOG_WARN("MazeEnv", "无法打开地图文件: %s", filepath.c_str());
        return false;
    }

    // 读取整个文件内容
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    ifs.close();

    // 轻量 JSON 解析：提取 start_pos、end_pos、bounds、walls。
    // canonical 字段先以 double 解析，避免经过 float 后改变整数化结果。
    auto findDouble = [&](const std::string& text,
                          const std::string& key) -> double {
        std::string pattern = "\"" + key + "\"";
        size_t pos = text.find(pattern);
        if (pos == std::string::npos) return -1.0;
        pos = text.find(':', pos + pattern.size());
        if (pos == std::string::npos) return -1.0;
        pos++;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) pos++;
        size_t end = pos;
        while (end < text.size() && (std::isdigit(text[end]) || text[end] == '.' || text[end] == '-')) end++;
        if (end == pos) return -1.0;
        try {
            std::size_t consumed = 0;
            const std::string token = text.substr(pos, end - pos);
            const double value = std::stod(token, &consumed);
            return consumed == token.size() && std::isfinite(value)
                       ? value
                       : -1.0;
        } catch (...) {
            return -1.0;
        }
    };
    auto findNumber = [&](const std::string& text,
                          const std::string& key) -> float {
        return static_cast<float>(findDouble(text, key));
    };

    auto findString = [&](const std::string& text,
                          const std::string& key) -> std::string {
        const std::string pattern = "\"" + key + "\"";
        size_t pos = text.find(pattern);
        if (pos == std::string::npos) return "";
        pos = text.find(':', pos + pattern.size());
        if (pos == std::string::npos) return "";
        const size_t begin = text.find('"', pos + 1);
        if (begin == std::string::npos) return "";
        const size_t end = text.find('"', begin + 1);
        return end == std::string::npos
                   ? ""
                   : text.substr(begin + 1, end - begin - 1);
    };

    if (findString(content, "blocked_bitmap_encoding") !=
            "row-major-u8-0-open-1-blocked") {
        LOG_WARN("MazeEnv", "地图 blocked bitmap 编码不受支持");
        return false;
    }

    // 解析 start_pos
    size_t sp_pos = content.find("\"start_pos\"");
    if (sp_pos != std::string::npos) {
        size_t brace = content.find('{', sp_pos);
        size_t brace_end = content.find('}', brace);
        if (brace != std::string::npos && brace_end != std::string::npos) {
            std::string block = content.substr(brace, brace_end - brace + 1);
            float sx = findNumber(block, "x");
            float sy = findNumber(block, "y");
            if (sx >= 0 && sy >= 0) {
                start_x_ = sx;
                start_y_ = sy;
                LOG_INFO("MazeEnv", "地图起点: (%.0f, %.0f)", start_x_, start_y_);
            }
        }
    }

    // 解析 end_pos
    size_t ep_pos = content.find("\"end_pos\"");
    if (ep_pos != std::string::npos) {
        size_t brace = content.find('{', ep_pos);
        size_t brace_end = content.find('}', brace);
        if (brace != std::string::npos && brace_end != std::string::npos) {
            std::string block = content.substr(brace, brace_end - brace + 1);
            float ex = findNumber(block, "x");
            float ey = findNumber(block, "y");
            if (ex >= 0 && ey >= 0) {
                end_x_ = ex;
                end_y_ = ey;
                LOG_INFO("MazeEnv", "地图终点: (%.0f, %.0f)", end_x_, end_y_);
            }
        }
    }

    // 解析 map_id（用于可视化回放时独立加载地图文件）
    size_t mid_pos = content.find("\"map_id\"");
    if (mid_pos != std::string::npos) {
        size_t colon = content.find(':', mid_pos);
        if (colon != std::string::npos) {
            size_t q1 = content.find('"', colon + 1);
            size_t q2 = (q1 != std::string::npos) ? content.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos) {
                map_id_ = content.substr(q1 + 1, q2 - q1 - 1);
                LOG_INFO("MazeEnv", "地图 ID: %s", map_id_.c_str());
            }
        }
    }

    // 解析 grid_size（覆盖配置值）
    const double json_grid_size = findDouble(content, "grid_size");
    const double json_grid_size_microunits =
        std::round(json_grid_size * 1000000.0);
    if (json_grid_size > 0.0 &&
        json_grid_size_microunits > 0.0 &&
        json_grid_size_microunits <=
            static_cast<double>(
                std::numeric_limits<std::uint32_t>::max())) {
        grid_size_microunits_ =
            static_cast<std::uint32_t>(json_grid_size_microunits);
        grid_size_ = static_cast<float>(json_grid_size);
        LOG_INFO("MazeEnv", "地图 grid_size: %.2f", grid_size_);
    } else {
        return false;
    }

    const int json_grid_cols =
        static_cast<int>(findNumber(content, "grid_cols"));
    const int json_grid_rows =
        static_cast<int>(findNumber(content, "grid_rows"));

    // 解析 bounds（可选，覆盖地图尺寸）
    size_t bounds_pos = content.find("\"bounds\"");
    if (bounds_pos != std::string::npos) {
        size_t brace = content.find('{', bounds_pos);
        size_t brace_end = content.find('}', brace);
        if (brace != std::string::npos && brace_end != std::string::npos) {
            std::string block = content.substr(brace, brace_end - brace + 1);
            float x_max = findNumber(block, "x_max");
            float y_max = findNumber(block, "y_max");
            if (x_max > 0) map_width_ = x_max;
            if (y_max > 0) map_height_ = y_max;
        }
    }

    if (json_grid_cols <= 0 || json_grid_rows <= 0) return false;
    grid_cols_ = json_grid_cols;
    grid_rows_ = json_grid_rows;
    blocked_.assign(grid_cols_ * grid_rows_, false);

    auto readGridPoint = [&](const std::string& key, int& gx, int& gy) {
        const size_t position = content.find("\"" + key + "\"");
        if (position == std::string::npos) return false;
        const size_t begin = content.find('{', position);
        const size_t end = content.find('}', begin);
        if (begin == std::string::npos || end == std::string::npos) return false;
        const std::string block = content.substr(begin, end - begin + 1);
        gx = static_cast<int>(findNumber(block, "x"));
        gy = static_cast<int>(findNumber(block, "y"));
        return gx >= 0 && gy >= 0;
    };
    if (!readGridPoint("start_grid", start_gx_, start_gy_) ||
        !readGridPoint("goal_grid", end_gx_, end_gy_)) {
        return false;
    }
    has_authoritative_grid_ = true;

    const std::string bitmap_hex = findString(content, "blocked_bitmap_hex");
    if (bitmap_hex.size() != blocked_.size() * 2) return false;
    auto hexValue = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < blocked_.size(); ++index) {
        const int high = hexValue(bitmap_hex[index * 2]);
        const int low = hexValue(bitmap_hex[index * 2 + 1]);
        if (high != 0 || (low != 0 && low != 1)) return false;
        blocked_[index] = low == 1;
    }

    // 解析 walls 数组
    size_t walls_pos = content.find("\"walls\"");
    if (walls_pos == std::string::npos) {
        LOG_WARN("MazeEnv", "地图文件缺少 walls 字段");
        return false;
    }

    // 找到 walls 数组的起始 '['
    size_t arr_start = content.find('[', walls_pos);
    if (arr_start == std::string::npos) return false;

    // 逐个解析墙壁对象 {...}
    int wall_count = 0;
    size_t search_pos = arr_start;
    while (true) {
        size_t obj_start = content.find('{', search_pos);
        if (obj_start == std::string::npos) break;

        // 检查是否已超出 walls 数组（遇到 ']'）
        size_t arr_end_check = content.find(']', search_pos);
        if (arr_end_check != std::string::npos && arr_end_check < obj_start) break;

        size_t obj_end = content.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string wall_str = content.substr(obj_start, obj_end - obj_start + 1);

        float x1 = findNumber(wall_str, "x1");
        float y1 = findNumber(wall_str, "y1");
        float x2 = findNumber(wall_str, "x2");
        float y2 = findNumber(wall_str, "y2");
        float t  = findNumber(wall_str, "thickness");
        if (t < 0) t = 10.0f;  // 默认厚度（与地图生成器一致）

        if (x1 >= 0 && y1 >= 0 && x2 >= 0 && y2 >= 0) {
            walls_.push_back({x1, y1, x2, y2, t});
            wall_count++;
        }

        search_pos = obj_end + 1;
    }

    LOG_INFO("MazeEnv", "地图文件加载完成: %d 面墙壁, 地图尺寸=%.0fx%.0f",
             wall_count, map_width_, map_height_);
    return wall_count > 0;
}

// ---- 8 方向射线检测 ----
RayResult MazeEnv::CastRays(int gx, int gy, int max_range) const {
    // 方向顺序：上、右上、右、右下、下、左下、左、左上（与 AIServer BuildObs 一致）
    static const int ray_dx[8] = { 0,  1,  1,  1,  0, -1, -1, -1};
    static const int ray_dy[8] = { 1,  1,  0, -1, -1, -1,  0,  1};

    RayResult result;
    for (int d = 0; d < 8; ++d) {
        int ray_dist = 0;
        for (int step = 1; step <= max_range; ++step) {
            int nx = gx + ray_dx[d] * step;
            int ny = gy + ray_dy[d] * step;
            if (!IsWalkable(nx, ny)) break;
            ray_dist = step;
        }
        result.distances[d] = static_cast<float>(ray_dist) / max_range;
    }
    return result;
}
