#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"
#include "grpc/update_flow_control.h"
#include "grpc/workload_contract.h"
#include "env/maze_env.h"
#include "config/config_loader.h"
#include "viz/viz_recorder.h"
#include "log/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using maze_client::AcceptCommandReply;
using maze_client::AgentExecutionCursor;
using maze_client::AttachExecutedActionReceipt;
using maze_client::FillCommand;
using maze_client::IsConclusiveRejected;
using maze_client::IsCommandWait;
using maze_client::PrepareAppliedAgentUpdate;
using maze_client::RecordExecutedAction;

constexpr const char* kDefaultConfigPath = "configs/client_config.yaml";
constexpr const char* kManagedReadyMarker =
    "/run/rl/client-managed-ready";
constexpr const char* kTrainingAdmittedMarker =
    "/run/rl/client-training-admitted";
constexpr std::chrono::milliseconds kBeginWaitRetryInterval{100};
constexpr const char* kTaskProtocolId = "rl.task.maze";
constexpr std::uint32_t kTaskProtocolVersion = 1;

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

const char* kActionNames[9] = {
    "不动", "上", "右上", "右", "右下", "下", "左下", "左", "左上"
};

std::atomic<bool> g_stop_requested{false};

void HandleSignal(int) { g_stop_requested.store(true); }

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

void RemoveManagedReadyMarker() {
    const char* managed = std::getenv("RL_INFRA_MANAGED");
    if (managed == nullptr || std::string(managed) != "true") return;
    std::error_code ignored;
    std::filesystem::remove(kManagedReadyMarker, ignored);
}

bool TrainingAdmissionReady(std::string& error) {
    namespace fs = std::filesystem;
    std::error_code filesystem_error;
    const fs::path marker(kTrainingAdmittedMarker);
    const auto status = fs::symlink_status(marker, filesystem_error);
    if (filesystem_error) {
        if (filesystem_error == std::errc::no_such_file_or_directory) {
            return false;
        }
        error = "cannot inspect training admission marker: " +
                filesystem_error.message();
        return false;
    }
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    if (!fs::is_regular_file(status)) {
        error = "training admission marker is not a regular file";
        return false;
    }
    const auto size = fs::file_size(marker, filesystem_error);
    if (filesystem_error || size == 0 || size > 4096) {
        error = "training admission marker size is invalid";
        return false;
    }
    return true;
}

const char* WorkloadName(maze::WorkloadMode workload) {
    switch (workload) {
        case maze::WORKLOAD_MODE_TRAINING: return "training";
        case maze::WORKLOAD_MODE_EVALUATION: return "evaluation";
        default: return "";
    }
}

const char* EpisodeModeName(maze::EpisodeMode mode) {
    switch (mode) {
        case maze::EPISODE_MODE_TRAINING: return "training";
        case maze::EPISODE_MODE_EVALUATION: return "evaluation";
        default: return "";
    }
}

maze::MazeTerminationReason ToProtoTerminationReason(
    AgentTerminationReason reason) {
    switch (reason) {
        case AgentTerminationReason::GoalReached:
            return maze::MAZE_TERMINATION_REASON_GOAL_REACHED;
        case AgentTerminationReason::TimeLimit:
            return maze::MAZE_TERMINATION_REASON_TIME_LIMIT;
        case AgentTerminationReason::Active:
        default:
            return maze::MAZE_TERMINATION_REASON_ACTIVE;
    }
}

const char* AgentStateName(AgentTerminationReason reason) {
    switch (reason) {
        case AgentTerminationReason::GoalReached:
            return "goal_reached";
        case AgentTerminationReason::TimeLimit:
            return "time_limit";
        case AgentTerminationReason::Active:
        default:
            return "active";
    }
}

const char* OutcomeStateName(maze::MazeTerminationReason reason) {
    switch (reason) {
        case maze::MAZE_TERMINATION_REASON_GOAL_REACHED:
            return "goal_reached";
        case maze::MAZE_TERMINATION_REASON_TIME_LIMIT:
            return "time_limit";
        case maze::MAZE_TERMINATION_REASON_CLIENT_ABORT:
            return "client_abort";
        case maze::MAZE_TERMINATION_REASON_TASK_STOP:
            return "task_stop";
        case maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE:
            return "chain_failure";
        case maze::MAZE_TERMINATION_REASON_ACTIVE:
            return "active";
        case maze::MAZE_TERMINATION_REASON_UNSPECIFIED:
        default:
            return "unspecified";
    }
}

bool RenderEpisodeOutcome(const maze::EndEpisodeRsp& response,
                          const std::string& episode_id,
                          int expected_agent_count,
                          std::string& rendered,
                          std::string& error) {
    error.clear();
    if (!response.has_outcome() ||
        response.outcome().episode_id() != episode_id ||
        response.outcome().agents_size() != expected_agent_count) {
        error = "EndEpisode outcome identity or Agent count is invalid";
        return false;
    }

    std::unordered_set<std::uint32_t> seen;
    std::vector<const maze::AgentEpisodeOutcome*> agents;
    agents.reserve(static_cast<std::size_t>(expected_agent_count));
    for (const auto& agent : response.outcome().agents()) {
        const bool goal = agent.termination_reason() ==
                          maze::MAZE_TERMINATION_REASON_GOAL_REACHED;
        if (agent.agent_id() >=
                static_cast<std::uint32_t>(expected_agent_count) ||
            !seen.insert(agent.agent_id()).second ||
            !agent.has_final_position() ||
            agent.termination_reason() ==
                maze::MAZE_TERMINATION_REASON_UNSPECIFIED ||
            agent.termination_reason() ==
                maze::MAZE_TERMINATION_REASON_ACTIVE ||
            goal != agent.has_goal_rank_group()) {
            error = "EndEpisode outcome contains an invalid Agent result";
            return false;
        }
        agents.push_back(&agent);
    }
    std::sort(agents.begin(), agents.end(), [](const auto* left,
                                                const auto* right) {
        return left->agent_id() < right->agent_id();
    });

    std::ostringstream output;
    output << "episode=" << episode_id << " agents=[";
    for (std::size_t index = 0; index < agents.size(); ++index) {
        if (index != 0) output << " | ";
        const auto& agent = *agents[index];
        output << "agent_id=" << agent.agent_id()
               << " state=" << OutcomeStateName(agent.termination_reason())
               << " terminal_frame_id=" << agent.terminal_frame_id()
               << " final=(" << agent.final_position().x() << ','
               << agent.final_position().y() << ')';
        if (agent.has_goal_rank_group()) {
            output << " goal_rank_group=" << agent.goal_rank_group();
        }
    }
    output << ']';
    rendered = output.str();
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

std::string BuildVizJson(const MazeEnv& env,
                         int frame_id,
                         int episode_number,
                         const std::vector<int>& actions) {
    std::ostringstream output;
    output << "{\"type\":\"frame_update\",\"frame_id\":" << frame_id
           << ",\"episode_id\":" << episode_number
           << ",\"map_id\":\"" << env.GetMapId() << "\",\"agents\":[";
    for (int index = 0; index < env.GetAgentNum(); ++index) {
        const auto& agent = env.GetAgent(index);
        if (index > 0) output << ',';
        output << "{\"id\":" << agent.id
               << ",\"x\":" << env.GetWorldX(agent.grid_x)
               << ",\"y\":" << env.GetWorldY(agent.grid_y)
               << ",\"done\":" << (agent.done ? "true" : "false")
               << ",\"action_id\":"
               << (index < static_cast<int>(actions.size()) ? actions[index] : 0)
               << '}';
    }
    output << "]}";
    return output.str();
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

    GrpcClient client;
    if (!client.Connect(config.network.server_host,
                        config.network.server_port)) {
        Logger::Instance().Close();
        return 1;
    }

    const char* managed = std::getenv("RL_INFRA_MANAGED");
    if (managed != nullptr && std::string(managed) == "true") {
        std::string readiness_error;
        if (!PublishManagedReadyMarker(config.network.server_host,
                                       readiness_error)) {
            LOG_ERROR("Main", "Client managed readiness 发布失败: %s",
                      readiness_error.c_str());
            client.Disconnect();
            Logger::Instance().Close();
            return 1;
        }
        LOG_INFO(
            "Main",
            "Client managed-ready; waiting for exact training admission "
            "before OpenSession");
        while (!g_stop_requested.load()) {
            std::string admission_error;
            if (TrainingAdmissionReady(admission_error)) {
                LOG_INFO("Main", "Training admission validated; entering OpenSession");
                break;
            }
            if (!admission_error.empty()) {
                LOG_ERROR("Main", "Training admission marker invalid: %s",
                          admission_error.c_str());
                RemoveManagedReadyMarker();
                client.Disconnect();
                Logger::Instance().Close();
                return 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (g_stop_requested.load()) {
            RemoveManagedReadyMarker();
            client.Disconnect();
            Logger::Instance().Close();
            return 0;
        }
    }

    maze::OpenSessionReq open_request;
    auto* client_identity = open_request.mutable_client();
    client_identity->set_component("maze-client");
    client_identity->set_instance_id(config.run.client_instance_id);
    client_identity->set_lifecycle_epoch(1);
    open_request.set_environment_instance_id(
        config.run.environment_instance_id);
    open_request.set_request_id(
        config.run.client_instance_id + ":" + std::to_string(::getpid()) +
        ":open-session");
    open_request.mutable_task_protocol()->set_protocol_id(kTaskProtocolId);
    open_request.mutable_task_protocol()->set_protocol_version(
        kTaskProtocolVersion);

    maze::OpenSessionRsp open_response;
    if (!client.OpenSession(open_request, open_response) ||
        !maze_client::CommandAccepted(open_response.reply()) ||
        open_response.reply().applied_sequence() != 0 ||
        open_response.reply().phase() != maze::SESSION_PHASE_OPEN ||
        open_response.session_id().empty() ||
        open_response.session_epoch() == 0 ||
        open_response.aiserver().component() != "rl-aiserver" ||
        open_response.aiserver().instance_id().empty() ||
        open_response.task_protocol().protocol_id() != kTaskProtocolId ||
        open_response.task_protocol().protocol_version() !=
            kTaskProtocolVersion ||
        !open_response.has_environment() ||
        open_response.environment().agent_count() == 0 ||
        open_response.environment().map_id().empty() ||
        open_response.environment().episode_max_steps() == 0 ||
        (open_response.environment().action_mask_mode() !=
             maze::ACTION_MASK_MODE_DISABLED &&
         open_response.environment().action_mask_mode() !=
             maze::ACTION_MASK_MODE_REQUIRED)) {
        LOG_ERROR("Main", "OpenSession 返回了无效的 Environment 身份");
        Logger::Instance().Close();
        return 1;
    }

    const std::string workload = WorkloadName(open_response.workload_mode());
    const bool replay_expected =
        open_response.workload_mode() == maze::WORKLOAD_MODE_EVALUATION;
    if (workload.empty()) {
        LOG_ERROR("Main", "OpenSession workload 无效");
        Logger::Instance().Close();
        return 1;
    }
    maze_client::LifecycleCursor cursor;
    cursor.session_id = open_response.session_id();
    cursor.session_epoch = open_response.session_epoch();
    maze_client::UpdateCursor(cursor, open_response.reply());

    config.run.agent_num =
        static_cast<int>(open_response.environment().agent_count());
    config.run.workload = workload;
    config.viz.recording_enabled = replay_expected;

    const std::string map_file = ResolveTaskMapFile(
        config.env.map_registry_dir,
        open_response.environment().map_id());
    if (map_file.empty()) {
        LOG_ERROR("Main", "TaskSpec 地图未在 registry 精确命中");
        Logger::Instance().Close();
        return 1;
    }
    config.env.map_file = map_file;
    MazeEnv environment;
    if (!environment.Init(config) ||
        environment.GetMapId() != open_response.environment().map_id()) {
        LOG_ERROR("Main", "Client 地图 registry 身份不匹配");
        Logger::Instance().Close();
        return 1;
    }

    maze::InitReq init_request;
    FillCommand(cursor, init_request.mutable_command());
    auto* map = init_request.mutable_map();
    map->set_map_id(environment.GetMapId());
    map->set_grid_columns(environment.GetGridCols());
    map->set_grid_rows(environment.GetGridRows());
    map->set_grid_size_microunits(environment.GetGridSizeMicrounits());
    map->set_start_grid_x(environment.GetStartGridX());
    map->set_start_grid_y(environment.GetStartGridY());
    map->set_goal_grid_x(environment.GetGoalGridX());
    map->set_goal_grid_y(environment.GetGoalGridY());
    map->set_blocked_bitmap(environment.GetBlockedBitmap());

    maze::InitRsp init_response;
    const bool init_rpc_ok = client.Init(init_request, init_response);
    const bool init_applied =
        init_rpc_ok &&
        AcceptCommandReply(cursor, init_response.reply());
    const bool init_payload_valid = init_applied;
    if (!init_payload_valid) {
        const bool init_outcome_unknown =
            !init_rpc_ok
                ? client.LastRpcOutcomeUnknown()
                : !init_applied &&
                      !IsConclusiveRejected(cursor,
                                            init_response.reply());
        LOG_ERROR(
            "Main",
            "Init 地图验证未收敛: seq=%llu applied=%llu "
            "outcome_unknown=%d message=%s",
            static_cast<unsigned long long>(
                init_request.command().sequence()),
            static_cast<unsigned long long>(
                init_response.reply().applied_sequence()),
            init_outcome_unknown ? 1 : 0,
            init_response.reply().message().c_str());
        if (!init_outcome_unknown) {
            maze::CloseSessionReq close_request;
            FillCommand(cursor, close_request.mutable_command());
            maze::CloseSessionRsp close_response;
            const bool close_rpc_ok =
                client.CloseSession(close_request, close_response);
            if (!close_rpc_ok ||
                !AcceptCommandReply(cursor,
                                    close_response.reply())) {
                const bool close_outcome_unknown =
                    !close_rpc_ok
                        ? client.LastRpcOutcomeUnknown()
                        : !IsConclusiveRejected(
                              cursor, close_response.reply());
                LOG_ERROR(
                    "Main",
                    "Init 失败后的 CloseSession 未收敛: seq=%llu "
                    "applied=%llu outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        close_request.command().sequence()),
                    static_cast<unsigned long long>(
                        close_response.reply().applied_sequence()),
                    close_outcome_unknown ? 1 : 0,
                    close_response.reply().message().c_str());
            }
        }
        client.Disconnect();
        Logger::Instance().Close();
        return 1;
    }

    bool chain_failed = false;
    bool lifecycle_outcome_unknown = false;
    bool episode_active = false;
    int episode_number = 0;
    VizRecorder recorder;

    while (!g_stop_requested.load()) {
        cursor.episode_id.clear();
        maze::BeginEpisodeReq begin_request;
        FillCommand(cursor, begin_request.mutable_command());
        maze::BeginEpisodeRsp begin_response;
        bool begin_applied = false;
        while (!g_stop_requested.load()) {
            begin_response.Clear();
            const bool begin_rpc_ok =
                client.BeginEpisode(begin_request, begin_response);
            if (!begin_rpc_ok) {
                lifecycle_outcome_unknown =
                    client.LastRpcOutcomeUnknown();
                LOG_ERROR(
                    "Main",
                    "BeginEpisode RPC 未收敛: seq=%llu "
                        "outcome_unknown=%d",
                    static_cast<unsigned long long>(
                        begin_request.command().sequence()),
                    lifecycle_outcome_unknown ? 1 : 0);
                chain_failed = true;
                break;
            }
            if (IsCommandWait(cursor, begin_response.reply())) {
                std::this_thread::sleep_for(kBeginWaitRetryInterval);
                continue;
            }
            begin_applied =
                AcceptCommandReply(cursor, begin_response.reply());
            if (!begin_applied) {
                lifecycle_outcome_unknown =
                    !IsConclusiveRejected(cursor,
                                          begin_response.reply());
                LOG_ERROR(
                    "Main",
                    "BeginEpisode 生命周期未收敛: seq=%llu applied=%llu "
                    "outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        begin_request.command().sequence()),
                    static_cast<unsigned long long>(
                        begin_response.reply().applied_sequence()),
                    lifecycle_outcome_unknown ? 1 : 0,
                    begin_response.reply().message().c_str());
                chain_failed = true;
            }
            break;
        }
        if (!begin_applied) {
            break;
        }
        if (begin_response.has_task_complete()) {
            if (cursor.phase != maze::SESSION_PHASE_TASK_COMPLETE) {
                LOG_ERROR("Main", "BeginEpisode task-complete phase 无效");
                chain_failed = true;
            }
            LOG_INFO("Main", "AIServer 已结束固定地图任务");
            break;
        }
        if (!begin_response.has_assignment()) {
            LOG_ERROR("Main", "BeginEpisode 缺少 assignment/task_complete");
            chain_failed = true;
            break;
        }
        const auto& assignment = begin_response.assignment();
        if (cursor.phase == maze::SESSION_PHASE_EPISODE_RUNNING) {
            cursor.episode_id = assignment.episode_id();
            episode_active = !cursor.episode_id.empty();
        }
        const std::string mode = EpisodeModeName(assignment.mode());
        const bool training_assignment =
            assignment.mode() == maze::EPISODE_MODE_TRAINING;
        const bool evaluation_assignment =
            assignment.mode() == maze::EPISODE_MODE_EVALUATION;
        if (cursor.phase != maze::SESSION_PHASE_EPISODE_RUNNING ||
            assignment.episode_id().empty() ||
            mode.empty() ||
            assignment.max_steps() !=
                open_response.environment().episode_max_steps() ||
            (!training_assignment && !evaluation_assignment) ||
            !maze_client::AssignmentMatchesWorkload(
                open_response.workload_mode(), assignment)) {
            LOG_ERROR("Main", "EpisodeAssignment 身份、模式或 horizon 无效");
            lifecycle_outcome_unknown = !episode_active;
            chain_failed = true;
            break;
        }
        environment.SetMaxSteps(static_cast<int>(assignment.max_steps()));
        environment.Reset();
        std::vector<int> last_actions(config.run.agent_num, 0);
        std::vector<AgentExecutionCursor> agent_execution(
            static_cast<std::size_t>(environment.GetAgentNum()));
        if (config.viz.recording_enabled &&
            !recorder.Begin(config.viz.output_dir, episode_number,
                            environment.GetMapId(),
                            environment.GetMapFilePath())) {
            LOG_ERROR("Main", "Replay 帧记录器启动失败");
            chain_failed = true;
            break;
        }
        LOG_INFO(
            "Main",
            "Episode %s 开始 mode=%s max_steps=%u",
            cursor.episode_id.c_str(), mode.c_str(),
            assignment.max_steps());

        while (!g_stop_requested.load()) {
            const bool terminal_report = environment.AllDone();
            maze::UpdateReq update_request;
            FillCommand(cursor, update_request.mutable_command());
            update_request.set_frame_id(environment.GetFrameId());
            for (int index = 0; index < environment.GetAgentNum(); ++index) {
                if (agent_execution[static_cast<std::size_t>(index)].retired) {
                    continue;
                }
                const auto& agent = environment.GetAgent(index);
                auto* state = update_request.add_agents();
                state->set_agent_id(agent.id);
                state->mutable_position()->set_x(
                    static_cast<float>(agent.grid_x));
                state->mutable_position()->set_y(
                    static_cast<float>(agent.grid_y));
                state->set_is_done(agent.done);
                state->set_termination_reason(
                    ToProtoTerminationReason(agent.termination_reason));
                state->set_last_move_blocked(agent.last_move_blocked);
                if (!agent.done &&
                    open_response.environment().action_mask_mode() ==
                        maze::ACTION_MASK_MODE_REQUIRED) {
                    const auto action_mask = environment.GetActionMask(index);
                    for (const bool available : action_mask) {
                        state->add_action_mask(available);
                    }
                }
                if (!AttachExecutedActionReceipt(
                        update_request.frame_id(),
                        agent_execution[static_cast<std::size_t>(index)],
                        state)) {
                    LOG_ERROR(
                        "Main",
                        "无法构造 Agent 动作执行回执: frame=%llu agent_id=%d",
                        static_cast<unsigned long long>(
                            update_request.frame_id()),
                        index);
                    chain_failed = true;
                    break;
                }
            }
            if (chain_failed) break;

            maze::UpdateRsp update_response;
            while (!g_stop_requested.load()) {
                update_response.Clear();
                if (!client.Update(update_request, update_response)) {
                    lifecycle_outcome_unknown =
                        client.LastRpcOutcomeUnknown();
                    LOG_ERROR(
                        "Main",
                        "Update RPC 失败: seq=%llu outcome_unknown=%d",
                        static_cast<unsigned long long>(
                            update_request.command().sequence()),
                        lifecycle_outcome_unknown ? 1 : 0);
                    chain_failed = true;
                    break;
                }
                if (update_response.has_wait()) {
                    if (!maze_client::IsUpdateWait(
                            update_response, cursor.next_sequence,
                            cursor.phase)) {
                        LOG_ERROR("Main", "Update WAIT 响应无效");
                        lifecycle_outcome_unknown = true;
                        chain_failed = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        update_response.wait().retry_after_ms()));
                    continue;
                }
                std::vector<AgentExecutionCursor> candidate_execution;
                bool valid_update = false;
                if (update_response.has_stop()) {
                    valid_update = AcceptCommandReply(
                        cursor, update_response.reply());
                } else if (PrepareAppliedAgentUpdate(
                               update_request, update_response,
                               agent_execution, candidate_execution) &&
                           AcceptCommandReply(
                               cursor, update_response.reply())) {
                    agent_execution = std::move(candidate_execution);
                    valid_update = true;
                }
                if (!valid_update) {
                    const auto& reply = update_response.reply();
                    LOG_ERROR(
                        "Main",
                        "Update 生命周期响应无效: frame=%llu result=%d "
                        "error=%d applied=%llu phase=%d actions=%d "
                        "message=%s",
                        static_cast<unsigned long long>(
                            update_request.frame_id()),
                        static_cast<int>(reply.result()),
                        static_cast<int>(reply.error_code()),
                        static_cast<unsigned long long>(
                            reply.applied_sequence()),
                        static_cast<int>(reply.phase()),
                        update_response.has_action_batch()
                            ? update_response.action_batch().actions_size()
                            : 0,
                        reply.message().c_str());
                    if (!IsConclusiveRejected(cursor, reply)) {
                        lifecycle_outcome_unknown = true;
                    }
                    chain_failed = true;
                }
                break;
            }
            if (chain_failed || g_stop_requested.load()) break;
            if (update_response.has_stop()) {
                if (update_response.stop().reason() !=
                    maze::MAZE_TERMINATION_REASON_TASK_STOP) {
                    LOG_ERROR("Main", "AIServer 任务停止响应无效");
                    chain_failed = true;
                    break;
                }
                maze::AbortEpisodeReq abort_request;
                FillCommand(cursor, abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_TASK_STOP);
                abort_request.set_message("AIServer requested task stop");
                maze::AbortEpisodeRsp abort_response;
                const bool abort_rpc_ok =
                    client.AbortEpisode(abort_request, abort_response);
                const bool abort_applied =
                    abort_rpc_ok &&
                    AcceptCommandReply(cursor,
                                       abort_response.reply());
                if (!abort_applied) {
                    lifecycle_outcome_unknown =
                        !abort_rpc_ok
                            ? client.LastRpcOutcomeUnknown()
                            : !IsConclusiveRejected(
                                  cursor, abort_response.reply());
                    LOG_ERROR(
                        "Main",
                        "AbortEpisode(task-stop) 未收敛: seq=%llu "
                        "applied=%llu outcome_unknown=%d message=%s",
                        static_cast<unsigned long long>(
                            abort_request.command().sequence()),
                        static_cast<unsigned long long>(
                            abort_response.reply().applied_sequence()),
                        lifecycle_outcome_unknown ? 1 : 0,
                        abort_response.reply().message().c_str());
                    chain_failed = true;
                } else {
                    episode_active = false;
                    cursor.episode_id.clear();
                }
                break;
            }
            if (terminal_report) break;
            const int action_frame_id = environment.GetFrameId();
            const int result_frame_id = action_frame_id + 1;
            const bool grouped_console_frame =
                result_frame_id % config.run.log_interval == 0;
            std::ostringstream grouped_actions;
            bool first_grouped_action = true;
            for (const auto& action :
                 update_response.action_batch().actions()) {
                const int agent_id = static_cast<int>(action.agent_id());
                const auto before = environment.GetAgent(agent_id);
                std::string step_error;
                if (!ExecuteAssignedAction(
                        environment,
                        agent_execution[static_cast<std::size_t>(agent_id)],
                        action,
                        step_error)) {
                    LOG_ERROR(
                        "Main",
                        "Client 动作执行链拒绝动作: episode=%s frame=%d "
                        "agent_id=%d action_id=%d error=%s",
                        cursor.episode_id.c_str(), action_frame_id,
                        agent_id, action.action_id(), step_error.c_str());
                    chain_failed = true;
                    break;
                }
                const auto& after = environment.GetAgent(agent_id);
                last_actions[agent_id] = action.action_id();
                LOG_FILE(
                    "Frame",
                    "episode=%s action_frame_id=%d result_frame_id=%d "
                    "agent_id=%d state=%s action_id=%d action=%s "
                    "from=(%d,%d) to=(%d,%d) blocked=%s terminal=%s",
                    cursor.episode_id.c_str(), action_frame_id,
                    result_frame_id, agent_id,
                    AgentStateName(after.termination_reason),
                    action.action_id(), kActionNames[action.action_id()],
                    before.grid_x, before.grid_y, after.grid_x, after.grid_y,
                    after.last_move_blocked ? "true" : "false",
                    after.done ? "true" : "false");
                if (!first_grouped_action) grouped_actions << " | ";
                first_grouped_action = false;
                grouped_actions
                    << "agent_id=" << agent_id
                    << " state=" << AgentStateName(after.termination_reason)
                    << " action_id=" << action.action_id()
                    << " action=" << kActionNames[action.action_id()]
                    << " from=(" << before.grid_x << ',' << before.grid_y
                    << ") to=(" << after.grid_x << ',' << after.grid_y
                    << ") blocked="
                    << (after.last_move_blocked ? "true" : "false");
                if (after.done && !grouped_console_frame) {
                    LOG_INFO(
                        "Exec",
                        "episode=%s action_frame_id=%d "
                        "result_frame_id=%d agent_id=%d state=%s "
                        "action_id=%d action=%s from=(%d,%d) "
                        "to=(%d,%d) blocked=%s terminal=true",
                        cursor.episode_id.c_str(), action_frame_id,
                        result_frame_id, agent_id,
                        AgentStateName(after.termination_reason),
                        action.action_id(), kActionNames[action.action_id()],
                        before.grid_x, before.grid_y, after.grid_x,
                        after.grid_y,
                        after.last_move_blocked ? "true" : "false");
                }
            }
            if (chain_failed) break;
            if (grouped_console_frame && !first_grouped_action) {
                LOG_INFO(
                    "Exec",
                    "episode=%s action_frame_id=%d result_frame_id=%d "
                    "agents=[%s]",
                    cursor.episode_id.c_str(), action_frame_id,
                    result_frame_id, grouped_actions.str().c_str());
            }
            environment.AdvanceFrame();
            if (config.viz.recording_enabled &&
                environment.GetFrameId() % config.viz.interval == 0) {
                recorder.RecordFrame(BuildVizJson(
                    environment, environment.GetFrameId(), episode_number,
                    last_actions));
            }
        }

        recorder.End();
        if (chain_failed) {
            if (episode_active && !lifecycle_outcome_unknown) {
                maze::AbortEpisodeReq abort_request;
                FillCommand(cursor, abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE);
                abort_request.set_message(
                    "Client stopped the Episode after a chain failure");
                maze::AbortEpisodeRsp abort_response;
                const bool abort_rpc_ok =
                    client.AbortEpisode(abort_request, abort_response);
                if (abort_rpc_ok &&
                    AcceptCommandReply(cursor,
                                       abort_response.reply())) {
                    episode_active = false;
                    cursor.episode_id.clear();
                } else {
                    lifecycle_outcome_unknown =
                        !abort_rpc_ok
                            ? client.LastRpcOutcomeUnknown()
                            : !IsConclusiveRejected(
                                  cursor, abort_response.reply());
                    LOG_ERROR(
                        "Main",
                        "AbortEpisode(chain-failure) 未收敛: seq=%llu "
                        "applied=%llu outcome_unknown=%d message=%s",
                        static_cast<unsigned long long>(
                            abort_request.command().sequence()),
                        static_cast<unsigned long long>(
                            abort_response.reply().applied_sequence()),
                        lifecycle_outcome_unknown ? 1 : 0,
                        abort_response.reply().message().c_str());
                }
            }
            break;
        }
        if (g_stop_requested.load() && episode_active) {
            maze::AbortEpisodeReq abort_request;
            FillCommand(cursor, abort_request.mutable_command());
            abort_request.set_reason(
                maze::MAZE_TERMINATION_REASON_CLIENT_ABORT);
            abort_request.set_message("Client received stop signal");
            maze::AbortEpisodeRsp abort_response;
            const bool abort_rpc_ok =
                client.AbortEpisode(abort_request, abort_response);
            if (!abort_rpc_ok ||
                !AcceptCommandReply(cursor, abort_response.reply())) {
                lifecycle_outcome_unknown =
                    !abort_rpc_ok
                        ? client.LastRpcOutcomeUnknown()
                        : !IsConclusiveRejected(
                              cursor, abort_response.reply());
                LOG_ERROR(
                    "Main",
                    "AbortEpisode(client-stop) 未收敛: seq=%llu "
                    "applied=%llu outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        abort_request.command().sequence()),
                    static_cast<unsigned long long>(
                        abort_response.reply().applied_sequence()),
                    lifecycle_outcome_unknown ? 1 : 0,
                    abort_response.reply().message().c_str());
                chain_failed = true;
            } else {
                episode_active = false;
                cursor.episode_id.clear();
            }
            break;
        }
        if (!episode_active) break;

        maze::EndEpisodeReq end_request;
        FillCommand(cursor, end_request.mutable_command());
        maze::EndEpisodeRsp end_response;
        if (!client.EndEpisode(end_request, end_response) ||
            !AcceptCommandReply(cursor, end_response.reply())) {
            lifecycle_outcome_unknown =
                client.LastRpcOutcomeUnknown() ||
                !IsConclusiveRejected(cursor, end_response.reply());
            LOG_ERROR(
                "Main",
                "EndEpisode 生命周期未收敛: seq=%llu applied=%llu "
                "outcome_unknown=%d message=%s",
                static_cast<unsigned long long>(
                    end_request.command().sequence()),
                static_cast<unsigned long long>(
                    end_response.reply().applied_sequence()),
                lifecycle_outcome_unknown ? 1 : 0,
                end_response.reply().message().c_str());
            chain_failed = true;
            break;
        }
        episode_active = false;
        std::string outcome_log;
        std::string outcome_error;
        if (!RenderEpisodeOutcome(
                end_response, cursor.episode_id,
                environment.GetAgentNum(), outcome_log, outcome_error)) {
            LOG_ERROR("Main", "%s", outcome_error.c_str());
            chain_failed = true;
            break;
        }
        LOG_INFO("Outcome", "%s", outcome_log.c_str());
        cursor.episode_id.clear();
        ++episode_number;
    }

    if (!episode_active && !lifecycle_outcome_unknown) {
        maze::CloseSessionReq close_request;
        FillCommand(cursor, close_request.mutable_command());
        maze::CloseSessionRsp close_response;
        if (!client.CloseSession(close_request, close_response) ||
            !AcceptCommandReply(cursor, close_response.reply())) {
            LOG_ERROR("Main", "CloseSession 生命周期未收敛");
            chain_failed = true;
        }
    } else if (lifecycle_outcome_unknown) {
        LOG_ERROR(
            "Main",
            "生命周期远端结果未知，禁止改用 Abort/Close: "
            "pending_seq=%llu episode=%s",
            static_cast<unsigned long long>(cursor.next_sequence),
            cursor.episode_id.c_str());
    }
    client.Disconnect();
    Logger::Instance().Close();
    return chain_failed ? 1 : 0;
}
