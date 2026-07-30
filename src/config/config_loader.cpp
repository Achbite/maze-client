#include "config/config_loader.h"
#include "log/logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cctype>
#include <cstdlib>

// ---- 去除字符串首尾空白 ----
static std::string Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

// ---- 去除字符串值的引号包裹 ----
static std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// ---- 安全转换辅助 ----
static int SafeInt(const std::string& val, int def) {
    if (val.empty()) return def;
    try { return std::stoi(val); } catch (...) { return def; }
}

static bool SafeBool(const std::string& val, bool def) {
    if (val.empty()) return def;
    std::string v = val;
    for (char& ch : v) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
    if (v == "false" || v == "0" || v == "no" || v == "off") return false;
    return def;
}

static float SafeFloat(const std::string& val, float def) {
    if (val.empty()) return def;
    try { return std::stof(val); } catch (...) { return def; }
}

static std::string GetEnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : "";
}

// ---- YAML 键值对 ----
struct YamlEntry {
    std::string section;    // 所属 section 名
    std::string key;        // 键名
    std::string value;      // 值（字符串形式）
};

// ---- 解析 YAML 文本为键值对列表 ----
static std::vector<YamlEntry> ParseYaml(const std::string& text) {
    std::vector<YamlEntry> entries;
    std::istringstream stream(text);
    std::string line;
    std::string current_section;

    while (std::getline(stream, line)) {
        // 去除行内注释（# 之后的内容），但保留引号内的 #
        size_t comment_pos = std::string::npos;
        bool in_quotes = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"' || line[i] == '\'') {
                in_quotes = !in_quotes;
            } else if (line[i] == '#' && !in_quotes) {
                comment_pos = i;
                break;
            }
        }
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        std::string trimmed = Trim(line);
        if (trimmed.empty()) continue;

        // 查找冒号位置
        size_t colon_pos = trimmed.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string key_part = Trim(trimmed.substr(0, colon_pos));
        std::string val_part = Trim(trimmed.substr(colon_pos + 1));

        // 判断是 section 还是 key-value
        // section：行首无缩进 且 冒号后无值
        bool has_indent = (!line.empty() && (line[0] == ' ' || line[0] == '\t'));

        if (!has_indent && val_part.empty()) {
            // 这是一个 section 声明
            current_section = key_part;
        } else if (has_indent && !key_part.empty()) {
            // 这是一个 key: value 对
            YamlEntry entry;
            entry.section = current_section;
            entry.key     = key_part;
            entry.value   = StripQuotes(val_part);
            entries.push_back(entry);
        }
    }

    return entries;
}

// ---- 在解析结果中查找指定 section.key 的值 ----
static std::string FindValue(const std::vector<YamlEntry>& entries,
                             const std::string& section,
                             const std::string& key) {
    for (const auto& e : entries) {
        if (e.section == section && e.key == key) {
            return e.value;
        }
    }
    return "";
}

// ---- 从 YAML 文件加载配置 ----
bool LoadClientConfig(const std::string& yaml_path, ClientConfig& out_config) {
    std::ifstream ifs(yaml_path);
    if (!ifs.is_open()) {
        LOG_WARN("Config", "无法打开配置文件: %s，使用默认值", yaml_path.c_str());
        out_config = ClientConfig{};
        return false;
    }

    // 读取整个文件
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    ifs.close();

    LOG_INFO("Config", "加载配置文件: %s", yaml_path.c_str());

    // 解析 YAML
    std::vector<YamlEntry> entries = ParseYaml(content);

    // --- run ---
    out_config.run.agent_num    = SafeInt(FindValue(entries, "run", "agent_num"),    1);
    out_config.run.max_episodes = SafeInt(FindValue(entries, "run", "max_episodes"), 100);
    out_config.run.log_interval = SafeInt(FindValue(entries, "run", "log_interval"), 100);
    std::string run_id = FindValue(entries, "run", "run_id");
    std::string client_id = FindValue(entries, "run", "client_id");
    std::string env_id = FindValue(entries, "run", "env_id");
    if (!run_id.empty()) out_config.run.run_id = run_id;
    if (!client_id.empty()) out_config.run.client_id = client_id;
    if (!env_id.empty()) out_config.run.env_id = env_id;
    out_config.run.session_id = SafeInt(FindValue(entries, "run", "session_id"), 0);

    // --- env ---
    out_config.env.map_width      = SafeFloat(FindValue(entries, "env", "map_width"),      20000.0f);
    out_config.env.map_height     = SafeFloat(FindValue(entries, "env", "map_height"),     20000.0f);
    out_config.env.grid_size      = SafeFloat(FindValue(entries, "env", "grid_size"),      500.0f);
    out_config.env.max_steps      = SafeInt(FindValue(entries, "env", "max_steps"),        10000);
    out_config.env.start_x        = SafeFloat(FindValue(entries, "env", "start_x"),        500.0f);
    out_config.env.start_y        = SafeFloat(FindValue(entries, "env", "start_y"),        500.0f);
    out_config.env.end_x          = SafeFloat(FindValue(entries, "env", "end_x"),          19500.0f);
    out_config.env.end_y          = SafeFloat(FindValue(entries, "env", "end_y"),          19500.0f);

    // 地图文件路径（为空则从 map_dir 随机选取或使用默认墙壁）
    std::string map_file = FindValue(entries, "env", "map_file");
    if (!map_file.empty()) {
        out_config.env.map_file = map_file;
    }

    // 地图目录路径（存在 .json 文件则随机选取一个）
    std::string map_dir = FindValue(entries, "env", "map_dir");
    if (!map_dir.empty()) {
        out_config.env.map_dir = map_dir;
    }

    // --- network ---
    std::string host = FindValue(entries, "network", "server_host");
    if (!host.empty()) {
        out_config.network.server_host = host;
    }
out_config.network.server_port = SafeInt(FindValue(entries, "network", "server_port"), 9002);

    // --- viz ---
    std::string viz_enabled = FindValue(entries, "viz", "enabled");
    if (viz_enabled == "false" || viz_enabled == "0") {
        out_config.viz.enabled = false;
    }
    std::string viz_output_dir = FindValue(entries, "viz", "output_dir");
    if (!viz_output_dir.empty()) {
        out_config.viz.output_dir = viz_output_dir;
    }
    out_config.viz.interval    = SafeInt(FindValue(entries, "viz", "interval"), 1);
    out_config.viz.server_port = SafeInt(FindValue(entries, "viz", "server_port"), 9004);

    std::string env_host = GetEnvValue("MAZE_AISERVER_HOST");
    if (!env_host.empty()) {
        out_config.network.server_host = env_host;
    }
    std::string env_port = GetEnvValue("MAZE_AISERVER_PORT");
    if (!env_port.empty()) {
        out_config.network.server_port = SafeInt(env_port, out_config.network.server_port);
    }
    std::string env_agent_num = GetEnvValue("MAZE_AGENT_NUM");
    if (!env_agent_num.empty()) {
        out_config.run.agent_num = SafeInt(env_agent_num, out_config.run.agent_num);
    }
    std::string env_max_episodes = GetEnvValue("MAZE_MAX_EPISODES");
    if (!env_max_episodes.empty()) {
        out_config.run.max_episodes = SafeInt(env_max_episodes, out_config.run.max_episodes);
    }
    std::string env_max_steps = GetEnvValue("MAZE_MAX_STEPS");
    if (!env_max_steps.empty()) {
        out_config.env.max_steps = SafeInt(env_max_steps, out_config.env.max_steps);
    }
    std::string env_run_id = GetEnvValue("MAZE_RUN_ID");
    if (!env_run_id.empty()) {
        out_config.run.run_id = env_run_id;
    }
    std::string env_client_id = GetEnvValue("MAZE_CLIENT_ID");
    if (!env_client_id.empty()) {
        out_config.run.client_id = env_client_id;
    }
    std::string env_env_id = GetEnvValue("MAZE_ENV_ID");
    if (!env_env_id.empty()) {
        out_config.run.env_id = env_env_id;
    }
    std::string env_session_id = GetEnvValue("MAZE_SESSION_ID");
    if (!env_session_id.empty()) {
        out_config.run.session_id = SafeInt(env_session_id, out_config.run.session_id);
    }
    std::string env_viz_enabled = GetEnvValue("MAZE_VIZ_ENABLED");
    if (!env_viz_enabled.empty()) {
        out_config.viz.enabled = SafeBool(env_viz_enabled, out_config.viz.enabled);
    }
    std::string env_viz_output_dir = GetEnvValue("MAZE_VIZ_OUTPUT_DIR");
    if (!env_viz_output_dir.empty()) {
        out_config.viz.output_dir = env_viz_output_dir;
    }
    std::string env_viz_interval = GetEnvValue("MAZE_VIZ_INTERVAL");
    if (!env_viz_interval.empty()) {
        out_config.viz.interval = SafeInt(
            env_viz_interval, out_config.viz.interval);
    }
    std::string env_replay_port = GetEnvValue("MAZE_REPLAY_PORT");
    if (!env_replay_port.empty()) {
        out_config.viz.server_port = SafeInt(
            env_replay_port, out_config.viz.server_port);
    }

    LOG_INFO("Config", "run: run_id=%s, client_id=%s, env_id=%s, session_id=%d, agent_num=%d, max_episodes=%d, log_interval=%d",
             out_config.run.run_id.c_str(), out_config.run.client_id.c_str(),
             out_config.run.env_id.c_str(), out_config.run.session_id,
             out_config.run.agent_num, out_config.run.max_episodes, out_config.run.log_interval);
    LOG_INFO("Config", "env: map=%.0fx%.0f, start=(%.0f,%.0f), end=(%.0f,%.0f), map_file=%s, map_dir=%s",
             out_config.env.map_width, out_config.env.map_height,
             out_config.env.start_x, out_config.env.start_y,
             out_config.env.end_x, out_config.env.end_y,
             out_config.env.map_file.empty() ? "(默认)" : out_config.env.map_file.c_str(),
             out_config.env.map_dir.empty() ? "(未设置)" : out_config.env.map_dir.c_str());
    LOG_INFO("Config", "network: %s:%d",
             out_config.network.server_host.c_str(), out_config.network.server_port);
    LOG_INFO("Config", "viz: enabled=%s, output_dir=%s, interval=%d, server_port=%d",
             out_config.viz.enabled ? "true" : "false",
             out_config.viz.output_dir.c_str(), out_config.viz.interval,
             out_config.viz.server_port);
    return true;
}
