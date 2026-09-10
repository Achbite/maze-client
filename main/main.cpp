#include "maze/protocol/client_adapter.h"
#include "maze/config/config_loader.h"
#include "maze/environment/maze_env.h"
#include "log/logger.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
constexpr const char* kDefaultConfigPath = "configs/client_config.yaml";
constexpr const char* kManagedReadyMarker = "/run/rl/client-managed-ready";
std::atomic<bool> g_stop_requested{false};
void HandleSignal(int) { g_stop_requested.store(true); }

rl_sdk::ClientOptions ClientOptionsFor(const ClientConfig& config) {
    rl_sdk::ClientOptions options;
    options.identity.set_component("maze-client");
    options.identity.set_instance_id(config.run.client_instance_id);
    options.identity.set_lifecycle_epoch(1);
    options.environment_instance_id = config.run.environment_instance_id;
    options.open_request_id = config.run.client_instance_id + ":" + std::to_string(::getpid()) + ":open-session";
    options.abort_wait_budget = std::chrono::milliseconds(config.network.abort_wait_timeout_ms);
    return options;
}

void PrintUsage() {
    std::fputs(
        "Usage: maze_client [options]\n"
        "\n"
        "Configuration is resolved once as CLI > allowlisted environment > "
        "config.\n"
        "--config is a meta option; every business override below replaces "
        "the\n"
        "named field in the selected config. Workload and task facts still "
        "come\n"
        "from AIServer OpenSession/TaskSpec.\n"
        "\n"
        "  --config PATH             select the YAML config file\n"
        "  --aiserver HOST:PORT      -> network.server_host/server_port\n"
        "  --replay-dir PATH         -> viz.output_dir\n"
        "  --replay-port PORT        -> viz.server_port\n"
        "  --help, -h                show this help and exit\n",
        stdout);
}

bool PublishManagedReadyMarker(const std::string& aiserver_alias,
                               std::string& error) {
    const char* managed = std::getenv("RL_INFRA_MANAGED");
    if (managed == nullptr || std::string(managed) != "true") return true;
    namespace fs = std::filesystem;
    std::error_code filesystem_error;
    const fs::path destination(kManagedReadyMarker);
    fs::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "cannot create managed readiness directory: " +
                filesystem_error.message();
        return false;
    }
    const fs::path temporary =
        destination.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "cannot open managed readiness marker";
            return false;
        }
        output << "aiserver_alias=" << aiserver_alias << "\n";
        output.flush();
        if (!output) {
            fs::remove(temporary, filesystem_error);
            error = "cannot flush managed readiness marker";
            return false;
        }
    }
    fs::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
        fs::remove(temporary, filesystem_error);
        error = "cannot publish managed readiness marker: " +
                filesystem_error.message();
        return false;
    }
    return true;
}

bool ParseCommandLine(int argc,
                      char* argv[],
                      std::string& config_path,
                      ClientConfigOverrides& overrides,
                      std::string& error) {
    config_path = kDefaultConfigPath;
    bool config_seen = false;
    bool aiserver_seen = false;
    bool replay_dir_seen = false;
    bool replay_port_seen = false;
    const auto require_value = [&](int index, const char* option) {
        if (index + 1 >= argc || argv[index + 1][0] == '\0') {
            error = std::string(option) +
                    " requires exactly one non-empty value";
            return false;
        }
        return true;
    };
    const auto parse_port = [&](const std::string& value,
                                const char* option,
                                int& port) {
        try {
            std::size_t consumed = 0;
            port = std::stoi(value, &consumed);
            if (consumed != value.size() || std::to_string(port) != value ||
                port <= 0 || port > 65535) {
                throw std::invalid_argument("port");
            }
        } catch (...) {
            error = std::string(option) + " port must be in [1, 65535]";
            return false;
        }
        return true;
    };
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--config") {
            if (config_seen || !require_value(index, "--config")) {
                error = "--config requires exactly one non-empty value";
                return false;
            }
            config_seen = true;
            config_path = argv[++index];
        } else if (argument == "--aiserver") {
            if (aiserver_seen || !require_value(index, "--aiserver")) {
                error = "--aiserver requires exactly one host:port";
                return false;
            }
            aiserver_seen = true;
            const std::string address(argv[++index]);
            const auto separator = address.rfind(':');
            if (separator == std::string::npos || separator == 0 ||
                separator + 1 >= address.size() ||
                address.find_first_of(" \t\r\n") != std::string::npos) {
                error = "--aiserver requires exactly one host:port";
                return false;
            }
            int port = 0;
            if (!parse_port(address.substr(separator + 1), "--aiserver",
                            port)) {
                return false;
            }
            overrides.server_host = address.substr(0, separator);
            overrides.server_port = port;
        } else if (argument == "--replay-dir") {
            if (replay_dir_seen || !require_value(index, "--replay-dir")) {
                error = "--replay-dir requires exactly one non-empty path";
                return false;
            }
            replay_dir_seen = true;
            const std::string value(argv[++index]);
            if (value.front() == ' ' || value.back() == ' ' ||
                value.find_first_of("\r\n") != std::string::npos) {
                error = "--replay-dir path is invalid";
                return false;
            }
            overrides.replay_output_dir = value;
        } else if (argument == "--replay-port") {
            if (replay_port_seen || !require_value(index, "--replay-port")) {
                error = "--replay-port requires exactly one port";
                return false;
            }
            replay_port_seen = true;
            int port = 0;
            if (!parse_port(argv[++index], "--replay-port", port)) {
                return false;
            }
            overrides.replay_server_port = port;
        } else if (argument.rfind("--", 0) == 0) {
            error = "unknown argument: " + argument;
            return false;
        } else {
            error = "positional arguments are not supported: " + argument;
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 &&
        (std::string(argv[1]) == "--help" ||
         std::string(argv[1]) == "-h")) {
        PrintUsage();
        return 0;
    }
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    Logger::Instance().Init("log");
    Logger::Instance().SetConsoleLevel(LogLevel::INFO);
    Logger::Instance().SetFileLevel(LogLevel::DEBUG);

    std::string config_path;
    ClientConfigOverrides config_overrides;
    std::string argument_error;
    if (!ParseCommandLine(argc, argv, config_path, config_overrides,
                          argument_error)) {
        LOG_ERROR("Main", "命令参数无效: %s", argument_error.c_str());
        Logger::Instance().Close();
        return 2;
    }
    ClientConfig config;
    ClientConfigLoadReport config_report;
    std::string config_error;
    if (!LoadClientConfig(config_path, config_overrides, config,
                          config_report, config_error)) {
        LOG_ERROR("Main", "配置加载失败: %s (%s)", config_path.c_str(),
                  config_error.empty() ? "see config diagnostics"
                                       : config_error.c_str());
        Logger::Instance().Close();
        return 2;
    }

    MazeEnv environment;
    rl_sdk::TaskClient<rl::task::maze::v1::MazeTaskServiceProtocol> client(ClientOptionsFor(config));
    if (!client.Connect(config.network.server_host + ":" + std::to_string(config.network.server_port))) {
        LOG_ERROR("Client SDK", "%s", client.error().c_str());
        Logger::Instance().Close();
        return 1;
    }
    std::string readiness_error;
    if (!PublishManagedReadyMarker(config.network.server_host, readiness_error)) {
        LOG_ERROR("Main", "Client managed readiness 发布失败: %s", readiness_error.c_str());
        Logger::Instance().Close();
        return 1;
    }
    MazeClientAdapter binding(config, environment, client,
                              [] { return g_stop_requested.load(); });
    const auto result = binding.Run();
    const bool clean = result == rl_sdk::CommandOutcome::Applied || result == rl_sdk::CommandOutcome::Stopped;
    if (!clean) LOG_ERROR("Client SDK", "Session ended: outcome=%d", static_cast<int>(result));
    Logger::Instance().Close();
    return clean ? 0 : 1;
}
