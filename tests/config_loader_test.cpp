#include "config/config_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream output;
    output << input.rdbuf();
    Require(static_cast<bool>(input) || input.eof(), "cannot read config");
    return output.str();
}

std::string ReplaceOnce(std::string value,
                        const std::string& from,
                        const std::string& to) {
    const auto position = value.find(from);
    Require(position != std::string::npos, "fixture token missing");
    value.replace(position, from.size(), to);
    return value;
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
    for (const auto& value : values) {
        if (value == expected) return true;
    }
    return false;
}

bool Load(const std::filesystem::path& root,
          const std::string& name,
          const std::string& content,
          ClientConfig* loaded = nullptr,
          ClientConfigLoadReport* loaded_report = nullptr,
          std::string* loaded_error = nullptr,
          const ClientConfigOverrides* overrides = nullptr) {
    const auto path = root / name;
    std::ofstream output(path, std::ios::trunc);
    output << content;
    output.close();
    ClientConfig config;
    ClientConfigLoadReport report;
    std::string error;
    const bool ok = overrides == nullptr
        ? LoadClientConfig(path.string(), config, report, error)
        : LoadClientConfig(path.string(), *overrides, config, report, error);
    if (loaded != nullptr) *loaded = config;
    if (loaded_report != nullptr) *loaded_report = report;
    if (loaded_error != nullptr) *loaded_error = error;
    return ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    Require(argc == 2, "config path is required");
    const std::string source = Read(argv[1]);
    const auto root = std::filesystem::temp_directory_path() /
                      ("maze-client-config-test-" +
                       std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    const auto registry = root / "maps";
    std::filesystem::create_directories(registry);
    {
        std::ofstream map(registry / "maze_117436372.json");
        map << "{}\n";
    }
    const std::string valid = ReplaceOnce(
        source, "  map_registry_dir: \"../maps/test\"",
        "  map_registry_dir: \"maps\"");
    Require(Load(root, "valid.yaml", valid), "valid config was rejected");

    ClientConfig configured;
    ClientConfigLoadReport report;
    ::setenv("RL_ENV_MAP_REGISTRY_DIR", registry.c_str(), 1);
    ::setenv("RL_EXPECTED_TASK_MAP_ID", "maze_117436372", 1);
    ::setenv(
        "RL_EXPECTED_TASK_MAP_SHA256",
        "861e5473b8df36dfae9c78dfefe52be931f5430e485cac0b69417c806eb1fe27",
        1);
    ::setenv("RL_EXPECTED_AGENT_COUNT", "4", 1);
    Require(Load(root, "env.yaml", valid, &configured, &report),
            "valid assignment overrides were rejected");
    Require(configured.env.map_registry_dir ==
                std::filesystem::canonical(registry).string(),
            "registry override was not normalized");
    Require(configured.expected.map_id == "maze_117436372",
            "expected map id was not retained");
    Require(configured.expected.agent_count == 4,
            "expected agent count was not retained");
    Require(Contains(report.environment_overridden_fields,
                     "env.map_registry_dir") &&
                Contains(report.environment_overridden_fields,
                         "expected.map_id") &&
                Contains(report.environment_overridden_fields,
                         "expected.map_sha256") &&
                Contains(report.environment_overridden_fields,
                         "expected.agent_count"),
            "assignment override report is incomplete");

    ClientConfigOverrides cli;
    cli.server_host = "aiserver.override";
    cli.server_port = 19002;
    cli.replay_output_dir = "replays";
    cli.replay_server_port = 19004;
    ClientConfig cli_config;
    ClientConfigLoadReport cli_report;
    Require(Load(root, "cli.yaml", valid, &cli_config, &cli_report,
                 nullptr, &cli),
            "valid CLI overrides were rejected");
    Require(cli_config.network.server_host == "aiserver.override" &&
                cli_config.network.server_port == 19002 &&
                cli_config.viz.output_dir == (root / "replays").string() &&
                cli_config.viz.server_port == 19004,
            "CLI overrides did not produce the effective Client config");
    Require(Contains(cli_report.cli_overridden_fields,
                     "network.server_host") &&
                Contains(cli_report.cli_overridden_fields,
                         "network.server_port") &&
                Contains(cli_report.cli_overridden_fields,
                         "viz.output_dir") &&
                Contains(cli_report.cli_overridden_fields,
                         "viz.server_port"),
            "CLI override report is incomplete");

    ::unsetenv("RL_ENV_MAP_REGISTRY_DIR");
    ::unsetenv("RL_EXPECTED_TASK_MAP_ID");
    ::unsetenv("RL_EXPECTED_TASK_MAP_SHA256");
    ::unsetenv("RL_EXPECTED_AGENT_COUNT");

    Require(!ResolveTaskMapFile(registry.string(), "maze_117436372").empty(),
            "regular registry map was not resolved");

    std::error_code error;
    std::filesystem::remove_all(root, error);
    return 0;
}
