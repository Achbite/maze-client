#include "maze/config/config_loader.h"
#include "log/logger.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

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

static bool ReadEnvironment(const char* name,
                            std::optional<std::string>& value,
                            std::string& error) {
    const char* raw = std::getenv(name);
    if (!raw) return true;
    if (*raw == '\0') {
        error = std::string(name) + " must not be empty";
        return false;
    }
    const std::string candidate(raw);
    if (candidate != Trim(candidate)) {
        error = std::string(name) +
                " must not contain surrounding whitespace";
        return false;
    }
    value = candidate;
    return true;
}

static bool ReadEnvironmentInt(const char* name,
                               std::optional<int>& value,
                               std::string& error) {
    std::optional<std::string> raw;
    if (!ReadEnvironment(name, raw, error) || !raw.has_value()) {
        return error.empty();
    }
    const int parsed = SafeInt(*raw, std::numeric_limits<int>::min());
    if (parsed == std::numeric_limits<int>::min()) {
        error = std::string(name) + " must be an integer";
        return false;
    }
    value = parsed;
    return true;
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

// ---- 从 YAML 文件加载配置并应用白名单环境覆盖 ----
bool LoadClientConfig(const std::string& yaml_path,
                      const ClientConfigOverrides& overrides,
                      ClientConfig& out_config,
                      ClientConfigLoadReport& report,
                      std::string& error) {
    namespace fs = std::filesystem;
    out_config = ClientConfig{};
    report = ClientConfigLoadReport{};
    error.clear();

    std::error_code fs_error;
    fs::path config_path = fs::absolute(fs::path(yaml_path), fs_error);
    if (fs_error) {
        error = "cannot resolve config path: " + yaml_path;
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    const auto configured_status = fs::symlink_status(config_path, fs_error);
    if (fs_error || fs::is_symlink(configured_status) ||
        !fs::is_regular_file(configured_status)) {
        error = "config must be a regular, non-symlink file";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    config_path = fs::weakly_canonical(config_path, fs_error);
    if (fs_error) {
        error = "cannot canonicalize config path: " + yaml_path;
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    report.config_path = config_path.string();

    std::ifstream ifs(config_path);
    if (!ifs.is_open()) {
        error = "cannot open config file: " + config_path.string();
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }

    // 读取整个文件
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    ifs.close();

    LOG_INFO("Config", "加载配置文件: %s", config_path.c_str());

    // 解析 YAML
    std::vector<YamlEntry> entries = ParseYaml(content);
    static const std::set<std::string> allowed_config_fields = {
        "run.client_instance_id",
        "run.environment_instance_id",
        "run.log_interval",
        "env.map_registry_dir",
        "network.server_host",
        "network.server_port",
        "network.abort_wait_timeout_ms",
        "viz.output_dir",
        "viz.interval",
        "viz.server_port",
    };
    for (const auto& entry : entries) {
        const std::string field = entry.section + "." + entry.key;
        if (allowed_config_fields.count(field) == 0) {
            error = "unknown Client config field: " + field;
            LOG_ERROR("Config", "%s", error.c_str());
            return false;
        }
    }
    const std::pair<const char*, const char*> required[] = {
        {"run", "client_instance_id"},
        {"run", "environment_instance_id"},
        {"run", "log_interval"},
        {"env", "map_registry_dir"},
        {"network", "server_host"},
        {"network", "server_port"},
        {"network", "abort_wait_timeout_ms"},
        {"viz", "output_dir"},
        {"viz", "interval"},
        {"viz", "server_port"},
    };
    for (const auto& field : required) {
        if (FindValue(entries, field.first, field.second).empty()) {
            error = std::string("missing Client config field: ") +
                    field.first + "." + field.second;
            LOG_ERROR("Config", "%s", error.c_str());
            return false;
        }
    }

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
    out_config.network.server_port =
        SafeInt(FindValue(entries, "network", "server_port"), 9002);
    out_config.network.abort_wait_timeout_ms = SafeInt(
        FindValue(entries, "network", "abort_wait_timeout_ms"), 30000);

    // --- viz ---
    std::string viz_output_dir = FindValue(entries, "viz", "output_dir");
    if (!viz_output_dir.empty()) {
        out_config.viz.output_dir = viz_output_dir;
    }
    out_config.viz.interval    = SafeInt(FindValue(entries, "viz", "interval"), 1);
    out_config.viz.server_port = SafeInt(FindValue(entries, "viz", "server_port"), 9004);

    const auto record_override = [&](const char* field) {
        report.environment_overridden_fields.emplace_back(field);
    };
    const auto apply_string = [&](const char* name,
                                  std::string& target,
                                  const char* field) {
        std::optional<std::string> value;
        if (!ReadEnvironment(name, value, error)) return false;
        if (value.has_value()) {
            target = *value;
            record_override(field);
        }
        return true;
    };
    if (!apply_string("RL_ENV_MAP_REGISTRY_DIR",
                      out_config.env.map_registry_dir,
                      "env.map_registry_dir")) {
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }

    std::optional<std::string> platform_pod_id;
    if (!ReadEnvironment("RL_INFRA_POD_ID", platform_pod_id, error)) {
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    if (platform_pod_id.has_value()) {
        out_config.run.client_instance_id = *platform_pod_id + "-client";
        out_config.run.environment_instance_id = *platform_pod_id + "-env";
        record_override("run.client_instance_id");
        record_override("run.environment_instance_id");
    }

    if (overrides.server_host.has_value() !=
        overrides.server_port.has_value()) {
        error = "--aiserver requires one complete host:port override";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    if ((overrides.server_host.has_value() &&
         (overrides.server_host->empty() ||
          *overrides.server_port <= 0 || *overrides.server_port > 65535)) ||
        (overrides.replay_output_dir.has_value() &&
         overrides.replay_output_dir->empty()) ||
        (overrides.replay_server_port.has_value() &&
         (*overrides.replay_server_port <= 0 ||
          *overrides.replay_server_port > 65535))) {
        error = "Client CLI override is invalid";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    const auto record_cli_override = [&](const char* field) {
        report.cli_overridden_fields.emplace_back(field);
    };
    if (overrides.server_host.has_value()) {
        out_config.network.server_host = *overrides.server_host;
        out_config.network.server_port = *overrides.server_port;
        record_cli_override("network.server_host");
        record_cli_override("network.server_port");
    }
    if (overrides.replay_output_dir.has_value()) {
        out_config.viz.output_dir = *overrides.replay_output_dir;
        record_cli_override("viz.output_dir");
    }
    if (overrides.replay_server_port.has_value()) {
        out_config.viz.server_port = *overrides.replay_server_port;
        record_cli_override("viz.server_port");
    }

    fs::path registry_path(out_config.env.map_registry_dir);
    if (registry_path.is_relative()) {
        registry_path = config_path.parent_path() / registry_path;
    }
    const auto registry_status = fs::symlink_status(registry_path, fs_error);
    if (fs_error || fs::is_symlink(registry_status) ||
        !fs::is_directory(registry_status)) {
        error = "map registry must be a regular, non-symlink directory: " +
                registry_path.string();
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    registry_path = fs::weakly_canonical(registry_path, fs_error);
    if (fs_error) {
        error = "cannot canonicalize map registry";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    out_config.env.map_registry_dir = registry_path.string();

    fs::path replay_path(out_config.viz.output_dir);
    if (replay_path.is_relative()) {
        replay_path = config_path.parent_path() / replay_path;
    }
    replay_path = fs::absolute(replay_path, fs_error).lexically_normal();
    if (fs_error) {
        error = "cannot resolve Replay output directory";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    const bool replay_exists = fs::exists(replay_path, fs_error);
    if (fs_error) {
        error = "cannot inspect Replay output directory";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    if (replay_exists) {
        const auto replay_status = fs::symlink_status(replay_path, fs_error);
        if (fs_error || fs::is_symlink(replay_status) ||
            !fs::is_directory(replay_status)) {
            error = "Replay output must be a directory or an absent path: " +
                    replay_path.string();
            LOG_ERROR("Config", "%s", error.c_str());
            return false;
        }
        replay_path = fs::weakly_canonical(replay_path, fs_error);
        if (fs_error) {
            error = "cannot canonicalize Replay output directory";
            LOG_ERROR("Config", "%s", error.c_str());
            return false;
        }
    }
    out_config.viz.output_dir = replay_path.string();
    if (out_config.run.client_instance_id.empty() ||
        out_config.run.environment_instance_id.empty() ||
        out_config.env.map_registry_dir.empty() ||
        out_config.network.server_host.empty() ||
        out_config.network.server_port <= 0 ||
        out_config.network.server_port > 65535 ||
        out_config.network.abort_wait_timeout_ms <= 0 ||
        out_config.network.abort_wait_timeout_ms > 300000 ||
        out_config.run.log_interval <= 0 ||
        out_config.viz.interval <= 0 ||
        out_config.viz.server_port <= 0 ||
        out_config.viz.server_port > 65535) {
        error = "local instance, registry, network or recording config is invalid";
        LOG_ERROR("Config", "%s", error.c_str());
        return false;
    }
    LOG_INFO("Config", "config source: %s", report.config_path.c_str());
    if (report.environment_overridden_fields.empty()) {
        LOG_INFO("Config", "environment overrides: none");
    } else {
        std::ostringstream fields;
        for (std::size_t index = 0;
             index < report.environment_overridden_fields.size(); ++index) {
            if (index > 0) fields << ',';
            fields << report.environment_overridden_fields[index];
        }
        LOG_INFO("Config", "environment overrides: %s",
                 fields.str().c_str());
    }
    if (report.cli_overridden_fields.empty()) {
        LOG_INFO("Config", "CLI overrides: none");
    } else {
        std::ostringstream fields;
        for (std::size_t index = 0;
             index < report.cli_overridden_fields.size(); ++index) {
            if (index > 0) fields << ',';
            fields << report.cli_overridden_fields[index];
        }
        LOG_INFO("Config", "CLI overrides: %s", fields.str().c_str());
    }
    LOG_INFO("Config", "run: client_instance_id=%s, environment_instance_id=%s, log_interval=%d",
             out_config.run.client_instance_id.c_str(),
             out_config.run.environment_instance_id.c_str(),
             out_config.run.log_interval);
    LOG_INFO("Config", "env: map_registry_dir=%s",
             out_config.env.map_registry_dir.c_str());
    LOG_INFO("Config", "network: %s:%d abort_wait_timeout_ms=%d",
             out_config.network.server_host.c_str(), out_config.network.server_port,
             out_config.network.abort_wait_timeout_ms);
    LOG_INFO("Config", "viz: output_dir=%s, interval=%d, server_port=%d",
             out_config.viz.output_dir.c_str(), out_config.viz.interval,
             out_config.viz.server_port);
    error.clear();
    return true;
}

bool LoadClientConfig(const std::string& yaml_path,
                      ClientConfig& out_config,
                      ClientConfigLoadReport& report,
                      std::string& error) {
    return LoadClientConfig(
        yaml_path, ClientConfigOverrides{}, out_config, report, error);
}

bool LoadClientConfig(const std::string& yaml_path,
                      ClientConfig& out_config) {
    ClientConfigLoadReport report;
    std::string error;
    return LoadClientConfig(yaml_path, out_config, report, error);
}

std::string ResolveTaskMapFile(const std::string& registry_dir,
                               const std::string& map_id) {
    namespace fs = std::filesystem;
    if (registry_dir.empty() || map_id.empty()) return "";
    static const std::regex map_id_pattern("[A-Za-z0-9_-]+");
    if (!std::regex_match(map_id, map_id_pattern)) return "";

    std::error_code error;
    const fs::path configured_root(registry_dir);
    const auto root_status = fs::symlink_status(configured_root, error);
    if (error || fs::is_symlink(root_status) ||
        !fs::is_directory(root_status)) {
        return "";
    }
    const fs::path root = fs::weakly_canonical(configured_root, error);
    if (error) return "";

    const fs::path raw_candidate = root / (map_id + ".json");
    const auto candidate_status = fs::symlink_status(raw_candidate, error);
    if (error || fs::is_symlink(candidate_status) ||
        !fs::is_regular_file(candidate_status)) {
        return "";
    }
    const fs::path candidate = fs::weakly_canonical(raw_candidate, error);
    if (error || candidate.parent_path() != root) return "";
    return candidate.string();
}
