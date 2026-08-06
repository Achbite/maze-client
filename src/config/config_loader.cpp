#include "config/config_loader.h"
#include "log/logger.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <limits>

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
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(val, &consumed);
        return consumed == val.size() ? value
                                      : std::numeric_limits<int>::min();
    } catch (...) {
        return std::numeric_limits<int>::min();
    }
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
        LOG_ERROR("Config", "无法打开配置文件: %s", yaml_path.c_str());
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
    out_config.run.log_interval = SafeInt(FindValue(entries, "run", "log_interval"), 100);
    out_config.run.client_instance_id =
        FindValue(entries, "run", "client_instance_id");
    out_config.run.environment_instance_id =
        FindValue(entries, "run", "environment_instance_id");

    // --- env ---
    std::string registry = FindValue(entries, "env", "map_registry_dir");
    if (!registry.empty()) out_config.env.map_registry_dir = registry;

    // --- network ---
    std::string host = FindValue(entries, "network", "server_host");
    if (!host.empty()) {
        out_config.network.server_host = host;
    }
out_config.network.server_port = SafeInt(FindValue(entries, "network", "server_port"), 9002);

    // --- viz ---
    std::string viz_output_dir = FindValue(entries, "viz", "output_dir");
    if (!viz_output_dir.empty()) {
        out_config.viz.output_dir = viz_output_dir;
    }
    out_config.viz.interval    = SafeInt(FindValue(entries, "viz", "interval"), 1);
    out_config.viz.server_port = SafeInt(FindValue(entries, "viz", "server_port"), 9004);

    std::string env_host = GetEnvValue("RL_AISERVER_HOST");
    if (!env_host.empty()) {
        out_config.network.server_host = env_host;
    }
    std::string env_port = GetEnvValue("RL_AISERVER_PORT");
    if (!env_port.empty()) {
        out_config.network.server_port = SafeInt(env_port, out_config.network.server_port);
    }
    std::string env_client_id = GetEnvValue("RL_CLIENT_INSTANCE_ID");
    if (!env_client_id.empty()) {
        out_config.run.client_instance_id = env_client_id;
    }
    std::string env_env_id = GetEnvValue("RL_ENVIRONMENT_INSTANCE_ID");
    if (!env_env_id.empty()) {
        out_config.run.environment_instance_id = env_env_id;
    }
    std::string env_viz_output_dir = GetEnvValue("RL_VIZ_OUTPUT_DIR");
    if (!env_viz_output_dir.empty()) {
        out_config.viz.output_dir = env_viz_output_dir;
    }
    std::string env_viz_interval = GetEnvValue("RL_VIZ_INTERVAL");
    if (!env_viz_interval.empty()) {
        out_config.viz.interval = SafeInt(
            env_viz_interval, out_config.viz.interval);
    }
    std::string env_replay_port = GetEnvValue("RL_REPLAY_PORT");
    if (!env_replay_port.empty()) {
        out_config.viz.server_port = SafeInt(
            env_replay_port, out_config.viz.server_port);
    }
    if (out_config.run.client_instance_id.empty() ||
        out_config.run.environment_instance_id.empty() ||
        out_config.env.map_registry_dir.empty() ||
        out_config.network.server_host.empty() ||
        out_config.network.server_port <= 0 ||
        out_config.network.server_port > 65535 ||
        out_config.run.log_interval <= 0 ||
        out_config.viz.interval <= 0 ||
        out_config.viz.server_port <= 0 ||
        out_config.viz.server_port > 65535) {
        LOG_ERROR("Config", "本地实例、registry、网络或记录配置无效");
        return false;
    }
    LOG_INFO("Config", "run: client_instance_id=%s, environment_instance_id=%s, log_interval=%d",
             out_config.run.client_instance_id.c_str(),
             out_config.run.environment_instance_id.c_str(),
             out_config.run.log_interval);
    LOG_INFO("Config", "env: map_registry_dir=%s",
             out_config.env.map_registry_dir.c_str());
    LOG_INFO("Config", "network: %s:%d",
             out_config.network.server_host.c_str(), out_config.network.server_port);
    LOG_INFO("Config", "viz: output_dir=%s, interval=%d, server_port=%d",
             out_config.viz.output_dir.c_str(), out_config.viz.interval,
             out_config.viz.server_port);
    return true;
}
