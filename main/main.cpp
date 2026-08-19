#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"
#include "grpc/policy_binding_contract.h"
#include "grpc/update_flow_control.h"
#include "grpc/workload_contract.h"
#include "env/maze_env.h"
#include "config/config_loader.h"
#include "viz/viz_recorder.h"
#include "log/logger.h"

#include <atomic>
#include <chrono>
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

namespace common = rl::common::v1;

namespace {

using maze_client::AcceptCommandReply;
using maze_client::AgentExecutionCursor;
using maze_client::AttachExecutedActionReceipt;
using maze_client::FillCommand;
using maze_client::IsConclusiveRejected;
using maze_client::IsCommandWait;
using maze_client::LifecycleAccepted;
using maze_client::PrepareAppliedAgentUpdate;
using maze_client::RecordExecutedAction;

constexpr const char* kDefaultConfigPath = "configs/client_config.yaml";
constexpr std::uint32_t kSessionProtocolVersion = 4;
constexpr const char* kObservationSchemaId = "maze.observation.v3";
constexpr std::uint32_t kObservationSchemaVersion = 1;
constexpr const char* kObservationSchemaDigest =
    "7cee41136020f3ffc8c6ae799f630d55d0588c6a99ab7f717eac3b3d08aa18b4";
constexpr const char* kActionSchemaId = "maze.action.v1";
constexpr std::uint32_t kActionSchemaVersion = 1;
constexpr const char* kActionSchemaDigest =
    "ce84c564e128f98adcc48fd420ac0df5acea61774a25de8705b602464009cfd8";
constexpr const char* kPolicyDistributionSchema = "categorical.logits.v1";
constexpr std::chrono::milliseconds kBeginWaitRetryInterval{100};

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

bool IsDigest(const common::ContentDigest& digest) {
    return maze_client::IsSha256Digest(digest);
}

void SetSha256(common::ContentDigest* digest, const std::string& hex) {
    digest->set_algorithm(common::DIGEST_ALGORITHM_SHA256);
    digest->set_hex(hex);
}

void FillSchema(common::SchemaIdentity* schema,
                const char* id,
                std::uint32_t version,
                const char* digest) {
    schema->set_schema_id(id);
    schema->set_schema_version(version);
    SetSha256(schema->mutable_canonical_digest(), digest);
}

bool SameSchema(const common::SchemaIdentity& schema,
                const char* id,
                std::uint32_t version,
                const char* digest) {
    return schema.schema_id() == id &&
           schema.schema_version() == version &&
           schema.canonical_digest().algorithm() ==
               common::DIGEST_ALGORITHM_SHA256 &&
           schema.canonical_digest().hex() == digest;
}

bool ValidTaskIdentity(const maze::TaskIdentity& identity) {
    return !identity.task_contract_id().empty() &&
           identity.task_revision() > 0 &&
           IsDigest(identity.task_config_digest()) &&
           !identity.fixed_map_id().empty() &&
           IsDigest(identity.fixed_map_digest());
}

bool SameTaskIdentity(const maze::TaskIdentity& lhs,
                      const maze::TaskIdentity& rhs) {
    return lhs.SerializeAsString() == rhs.SerializeAsString();
}

const char* WorkloadName(maze::WorkloadMode workload) {
    switch (workload) {
        case maze::WORKLOAD_MODE_TRAINING: return "training";
        case maze::WORKLOAD_MODE_EVALUATION: return "evaluation";
        default: return "";
    }
}

const char* ReplayPolicyName(maze::ReplayPolicy policy) {
    switch (policy) {
        case maze::REPLAY_POLICY_DISABLED: return "disabled";
        case maze::REPLAY_POLICY_RECORD_AND_SERVE: return "record-and-serve";
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

bool PublishSessionPolicy(const std::string& workload,
                          const std::string& replay_policy,
                          const maze::BehaviorPolicyBinding& policy,
                          const VizConfig& viz) {
    const char* raw_path = std::getenv("RL_SESSION_POLICY_PATH");
    if (raw_path == nullptr || raw_path[0] == '\0') return true;

    namespace fs = std::filesystem;
    const fs::path path(raw_path);
    std::error_code error;
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path(), error);
        if (error) return false;
    }
    const fs::path temporary =
        path.string() + ".tmp." + std::to_string(::getpid());
    const char* behavior_policy_scope = "none";
    const char* model_identity_role = "not-applicable";
    if (workload == "training") {
        behavior_policy_scope = "training-fragment";
        model_identity_role = "episode-start-snapshot";
    } else if (workload == "evaluation") {
        behavior_policy_scope = "evaluation-episode";
        model_identity_role = "episode-pinned";
    }
    {
        std::ofstream output(temporary);
        if (!output) return false;
        output << "workload=" << workload << "\n"
               << "replay_policy=" << replay_policy << "\n"
               << "behavior_policy_scope=" << behavior_policy_scope << "\n"
               << "model_identity_role=" << model_identity_role << "\n"
               << "replay_output_dir=" << viz.output_dir << "\n"
               << "replay_server_port=" << viz.server_port << "\n"
               << "model_artifact_digest="
               << policy.model_artifact_digest().hex() << "\n";
        if (workload == "training") {
            output << "model_lineage_id=" << policy.model_lineage_id() << "\n"
                   << "model_step=" << policy.model_step() << "\n"
                   << "model_manifest_digest="
                   << policy.model_manifest_digest().hex() << "\n";
        }
        output.flush();
        if (!output) {
            fs::remove(temporary, error);
            return false;
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
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

    maze::OpenSessionReq open_request;
    auto* client_identity = open_request.mutable_client();
    client_identity->set_component("maze-client");
    client_identity->set_instance_id(config.run.client_instance_id);
    client_identity->set_lifecycle_epoch(1);
    open_request.set_environment_instance_id(
        config.run.environment_instance_id);
    open_request.add_supported_session_protocol_versions(
        kSessionProtocolVersion);
    FillSchema(open_request.add_supported_observation_schemas(),
               kObservationSchemaId, kObservationSchemaVersion,
               kObservationSchemaDigest);
    FillSchema(open_request.add_supported_action_schemas(),
               kActionSchemaId, kActionSchemaVersion,
               kActionSchemaDigest);
    open_request.set_idempotency_key(
        config.run.client_instance_id + ":" + std::to_string(::getpid()) +
        ":open-session");

    maze::OpenSessionRsp open_response;
    if (!client.OpenSession(open_request, open_response) ||
        !LifecycleAccepted(open_response.lifecycle()) ||
        open_response.session_protocol_version() != kSessionProtocolVersion ||
        open_response.session_id().empty() ||
        open_response.lifecycle_epoch() == 0 ||
        open_response.aiserver().component() != "rl-aiserver" ||
        open_response.aiserver().instance_id().empty() ||
        !ValidTaskIdentity(open_response.task_spec().identity()) ||
        open_response.task_spec().agent_count() == 0 ||
        open_response.task_spec().fixed_map_id() !=
            open_response.task_spec().identity().fixed_map_id() ||
        open_response.task_spec().expected_map_digest().SerializeAsString() !=
            open_response.task_spec().identity().fixed_map_digest().SerializeAsString() ||
        !SameSchema(open_response.task_spec().observation_schema(),
                    kObservationSchemaId, kObservationSchemaVersion,
                    kObservationSchemaDigest) ||
        !SameSchema(open_response.task_spec().action_schema(),
                    kActionSchemaId, kActionSchemaVersion,
                    kActionSchemaDigest) ||
        open_response.task_spec().action_rule_id() !=
            "maze.action.9-way.no-corner-cut.v1" ||
        open_response.task_spec().episode_max_steps() == 0) {
        LOG_ERROR("Main", "OpenSession 返回了无效的 0.13.0 任务身份");
        Logger::Instance().Close();
        return 1;
    }

    const std::string workload = WorkloadName(open_response.workload_mode());
    const std::string replay_policy =
        ReplayPolicyName(open_response.replay_policy());
    const bool replay_expected =
        open_response.workload_mode() == maze::WORKLOAD_MODE_EVALUATION;
    if (workload.empty() || replay_policy.empty() ||
        replay_expected !=
            (open_response.replay_policy() ==
             maze::REPLAY_POLICY_RECORD_AND_SERVE)) {
        LOG_ERROR("Main", "OpenSession workload 与 Replay policy 无效");
        Logger::Instance().Close();
        return 1;
    }
    maze_client::LifecycleCursor cursor;
    cursor.task.CopyFrom(open_response.task_spec().identity());
    cursor.session_id = open_response.session_id();
    cursor.lifecycle_epoch = open_response.lifecycle_epoch();
    maze_client::UpdateCursor(cursor, open_response.lifecycle());

    const bool assignment_mismatch =
        (config.expected.map_id.has_value() &&
         open_response.task_spec().fixed_map_id() !=
             *config.expected.map_id) ||
        (config.expected.map_sha256.has_value() &&
         open_response.task_spec().expected_map_digest().hex() !=
             *config.expected.map_sha256) ||
        (config.expected.agent_count.has_value() &&
         open_response.task_spec().agent_count() !=
             static_cast<std::uint32_t>(*config.expected.agent_count));
    if (assignment_mismatch) {
        const std::string expected_agent_count =
            config.expected.agent_count.has_value()
                ? std::to_string(*config.expected.agent_count)
                : "<none>";
        LOG_ERROR(
            "Main",
            "OpenSession assignment mismatch: workload=%s "
            "expected_map=%s actual_map=%s "
            "expected_digest=%s actual_digest=%s expected_agents=%s "
            "actual_agents=%u",
            workload.c_str(),
            config.expected.map_id.has_value()
                ? config.expected.map_id->c_str() : "<none>",
            open_response.task_spec().fixed_map_id().c_str(),
            config.expected.map_sha256.has_value()
                ? config.expected.map_sha256->c_str() : "<none>",
            open_response.task_spec().expected_map_digest().hex().c_str(),
            expected_agent_count.c_str(),
            open_response.task_spec().agent_count());
        maze::CloseSessionReq close_request;
        FillCommand(cursor, "close-assignment-mismatch",
                    close_request.mutable_command());
        maze::CloseSessionRsp close_response;
        const bool close_rpc_ok =
            client.CloseSession(close_request, close_response);
        const bool close_applied =
            close_rpc_ok &&
            AcceptCommandReply(cursor, close_response.lifecycle());
        if (!close_applied) {
            LOG_ERROR(
                "Main",
                "assignment mismatch CloseSession 未收敛: seq=%llu "
                "applied=%llu outcome_unknown=%d message=%s",
                static_cast<unsigned long long>(
                    close_request.command().command_sequence()),
                static_cast<unsigned long long>(
                    close_response.lifecycle().applied_sequence()),
                (!close_rpc_ok
                     ? client.LastRpcOutcomeUnknown()
                     : !IsConclusiveRejected(
                           cursor, close_response.lifecycle()))
                    ? 1
                    : 0,
                close_response.lifecycle().message().c_str());
        }
        client.Disconnect();
        Logger::Instance().Close();
        return 1;
    }
    config.run.agent_num =
        static_cast<int>(open_response.task_spec().agent_count());
    config.run.workload = workload;
    config.viz.recording_enabled = replay_expected;

    const std::string map_file = ResolveTaskMapFile(
        config.env.map_registry_dir,
        open_response.task_spec().fixed_map_id());
    if (map_file.empty()) {
        LOG_ERROR("Main", "TaskSpec 地图未在 registry 精确命中");
        Logger::Instance().Close();
        return 1;
    }
    config.env.map_file = map_file;
    MazeEnv environment;
    if (!environment.Init(config) ||
        environment.GetMapId() != open_response.task_spec().fixed_map_id() ||
        environment.GetMapChecksum() !=
            open_response.task_spec().expected_map_digest().hex()) {
        LOG_ERROR("Main", "Client 地图 canonical 身份不匹配");
        Logger::Instance().Close();
        return 1;
    }

    maze::InitReq init_request;
    FillCommand(cursor, "init", init_request.mutable_command());
    auto* map = init_request.mutable_map();
    map->set_map_id(environment.GetMapId());
    map->set_format_version(environment.GetMapFormatVersion());
    map->set_grid_columns(environment.GetGridCols());
    map->set_grid_rows(environment.GetGridRows());
    map->set_grid_size_microunits(environment.GetGridSizeMicrounits());
    map->set_start_grid_x(environment.GetStartGridX());
    map->set_start_grid_y(environment.GetStartGridY());
    map->set_goal_grid_x(environment.GetGoalGridX());
    map->set_goal_grid_y(environment.GetGoalGridY());
    map->set_blocked_bitmap(environment.GetBlockedBitmap());
    SetSha256(map->mutable_canonical_digest(), environment.GetMapChecksum());
    map->set_shortest_action_steps(environment.GetShortestActionSteps());
    map->set_action_rule_id(environment.GetActionRuleId());

    maze::InitRsp init_response;
    const bool init_rpc_ok = client.Init(init_request, init_response);
    const bool init_applied =
        init_rpc_ok &&
        AcceptCommandReply(cursor, init_response.lifecycle());
    const bool init_payload_valid =
        init_applied &&
        init_response.accepted_map_digest().SerializeAsString() ==
            map->canonical_digest().SerializeAsString() &&
        init_response.verified_shortest_action_steps() ==
            environment.GetShortestActionSteps();
    if (!init_payload_valid) {
        const bool init_outcome_unknown =
            !init_rpc_ok
                ? client.LastRpcOutcomeUnknown()
                : !init_applied &&
                      !IsConclusiveRejected(cursor,
                                            init_response.lifecycle());
        LOG_ERROR(
            "Main",
            "Init 地图验证未收敛: seq=%llu applied=%llu "
            "outcome_unknown=%d message=%s",
            static_cast<unsigned long long>(
                init_request.command().command_sequence()),
            static_cast<unsigned long long>(
                init_response.lifecycle().applied_sequence()),
            init_outcome_unknown ? 1 : 0,
            init_response.lifecycle().message().c_str());
        if (!init_outcome_unknown) {
            maze::CloseSessionReq close_request;
            FillCommand(cursor, "close-after-init-failure",
                        close_request.mutable_command());
            maze::CloseSessionRsp close_response;
            const bool close_rpc_ok =
                client.CloseSession(close_request, close_response);
            if (!close_rpc_ok ||
                !AcceptCommandReply(cursor,
                                    close_response.lifecycle())) {
                const bool close_outcome_unknown =
                    !close_rpc_ok
                        ? client.LastRpcOutcomeUnknown()
                        : !IsConclusiveRejected(
                              cursor, close_response.lifecycle());
                LOG_ERROR(
                    "Main",
                    "Init 失败后的 CloseSession 未收敛: seq=%llu "
                    "applied=%llu outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        close_request.command().command_sequence()),
                    static_cast<unsigned long long>(
                        close_response.lifecycle().applied_sequence()),
                    close_outcome_unknown ? 1 : 0,
                    close_response.lifecycle().message().c_str());
            }
        }
        client.Disconnect();
        Logger::Instance().Close();
        return 1;
    }

    bool chain_failed = false;
    bool lifecycle_outcome_unknown = false;
    bool session_policy_published = false;
    bool episode_active = false;
    int episode_number = 0;
    VizRecorder recorder;

    while (!g_stop_requested.load()) {
        cursor.episode_id.clear();
        maze::BeginEpisodeReq begin_request;
        FillCommand(cursor, "begin-episode", begin_request.mutable_command());
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
                        begin_request.command().command_sequence()),
                    lifecycle_outcome_unknown ? 1 : 0);
                chain_failed = true;
                break;
            }
            if (IsCommandWait(cursor, begin_response.lifecycle())) {
                std::this_thread::sleep_for(kBeginWaitRetryInterval);
                continue;
            }
            begin_applied =
                AcceptCommandReply(cursor, begin_response.lifecycle());
            if (!begin_applied) {
                lifecycle_outcome_unknown =
                    !IsConclusiveRejected(cursor,
                                          begin_response.lifecycle());
                LOG_ERROR(
                    "Main",
                    "BeginEpisode 生命周期未收敛: seq=%llu applied=%llu "
                    "outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        begin_request.command().command_sequence()),
                    static_cast<unsigned long long>(
                        begin_response.lifecycle().applied_sequence()),
                    lifecycle_outcome_unknown ? 1 : 0,
                    begin_response.lifecycle().message().c_str());
                chain_failed = true;
            }
            break;
        }
        if (!begin_applied) {
            break;
        }
        const auto& assignment = begin_response.assignment();
        if (!assignment.continue_task()) {
            LOG_INFO("Main", "AIServer 已结束固定地图任务");
            break;
        }
        if (cursor.session_state == maze::SESSION_STATE_EPISODE_ACTIVE &&
            cursor.episode_state == maze::EPISODE_STATE_RUNNING) {
            cursor.episode_id = assignment.episode_id();
            episode_active = !cursor.episode_id.empty();
        }
        const std::string mode = EpisodeModeName(assignment.mode());
        const bool training_assignment =
            assignment.mode() == maze::EPISODE_MODE_TRAINING;
        const bool evaluation_assignment =
            assignment.mode() == maze::EPISODE_MODE_EVALUATION;
        if (cursor.session_state != maze::SESSION_STATE_EPISODE_ACTIVE ||
            cursor.episode_state != maze::EPISODE_STATE_RUNNING ||
            assignment.episode_id().empty() ||
            !SameTaskIdentity(assignment.task(), cursor.task) ||
            mode.empty() ||
            assignment.max_steps() !=
                open_response.task_spec().episode_max_steps() ||
            !maze_client::PolicyBindingMatchesWorkload(
                open_response.workload_mode(), assignment.behavior_policy(),
                kPolicyDistributionSchema) ||
            assignment.collect_training_samples() != training_assignment ||
            (!training_assignment && !evaluation_assignment) ||
            !maze_client::AssignmentMatchesWorkload(
                open_response.workload_mode(), assignment)) {
            LOG_ERROR("Main", "EpisodeAssignment 身份、模式或 horizon 无效");
            lifecycle_outcome_unknown = !episode_active;
            chain_failed = true;
            break;
        }
        if (!session_policy_published) {
            if (!PublishSessionPolicy(workload, replay_policy,
                                      assignment.behavior_policy(),
                                      config.viz)) {
                LOG_ERROR("Main", "无法发布已协商 Session policy");
                chain_failed = true;
                break;
            }
            session_policy_published = true;
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
        if (training_assignment) {
            LOG_INFO(
                "Main",
                "Episode %s 开始 mode=%s max_steps=%u start_model_step=%llu "
                "policy_scope=fragment",
                cursor.episode_id.c_str(), mode.c_str(),
                assignment.max_steps(),
                static_cast<unsigned long long>(
                    assignment.behavior_policy().model_step()));
        } else {
            LOG_INFO(
                "Main",
                "Episode %s 开始 mode=%s max_steps=%u pinned_model_sha=%s "
                "policy_scope=episode digest_only=1",
                cursor.episode_id.c_str(), mode.c_str(),
                assignment.max_steps(),
                assignment.behavior_policy()
                    .model_artifact_digest().hex().c_str());
        }

        while (!g_stop_requested.load()) {
            const bool terminal_report = environment.AllDone();
            maze::UpdateReq update_request;
            FillCommand(cursor, "update", update_request.mutable_command());
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
                            update_request.command().command_sequence()),
                        lifecycle_outcome_unknown ? 1 : 0);
                    chain_failed = true;
                    break;
                }
                if (update_response.environment_control() ==
                    maze::ENVIRONMENT_CONTROL_WAIT_FOR_TRAINING_CAPACITY) {
                    if (!maze_client::IsTrainingCapacityWait(
                            update_response, cursor.next_sequence,
                            cursor.task_state, cursor.session_state,
                            cursor.episode_state)) {
                        LOG_ERROR("Main", "Update WAIT 响应无效");
                        lifecycle_outcome_unknown = true;
                        chain_failed = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        update_response.retry_after_ms()));
                    continue;
                }
                std::vector<AgentExecutionCursor> candidate_execution;
                bool valid_update = false;
                if (update_response.environment_control() ==
                    maze::ENVIRONMENT_CONTROL_ADVANCE) {
                    if (update_response.task_stop_requested()) {
                        valid_update = AcceptCommandReply(
                            cursor, update_response.lifecycle());
                    } else if (PrepareAppliedAgentUpdate(
                                   update_request, update_response,
                                   agent_execution, candidate_execution) &&
                               AcceptCommandReply(
                                   cursor, update_response.lifecycle())) {
                        agent_execution = std::move(candidate_execution);
                        valid_update = true;
                    }
                }
                if (!valid_update) {
                    const auto& reply = update_response.lifecycle();
                    LOG_ERROR(
                        "Main",
                        "Update 生命周期响应无效: frame=%llu control=%d "
                        "ret=%d result=%d error=%d applied=%llu "
                        "task=%d session=%d episode=%d "
                        "actions=%d message=%s",
                        static_cast<unsigned long long>(
                            update_request.frame_id()),
                        static_cast<int>(
                            update_response.environment_control()),
                        reply.ret_code(), static_cast<int>(reply.result()),
                        static_cast<int>(reply.error_code()),
                        static_cast<unsigned long long>(
                            reply.applied_sequence()),
                        static_cast<int>(reply.task_state()),
                        static_cast<int>(reply.session_state()),
                        static_cast<int>(reply.episode_state()),
                        update_response.actions_size(),
                        reply.message().c_str());
                    if (!IsConclusiveRejected(cursor, reply)) {
                        lifecycle_outcome_unknown = true;
                    }
                    chain_failed = true;
                }
                break;
            }
            if (chain_failed || g_stop_requested.load()) break;
            if (update_response.task_stop_requested()) {
                if (update_response.task_stop_reason() !=
                        maze::MAZE_TERMINATION_REASON_TASK_STOP ||
                    update_response.actions_size() != 0) {
                    LOG_ERROR("Main", "AIServer 任务停止响应无效");
                    chain_failed = true;
                    break;
                }
                maze::AbortEpisodeReq abort_request;
                FillCommand(cursor, "abort-task-stop",
                            abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_TASK_STOP);
                abort_request.set_message("AIServer requested task stop");
                maze::AbortEpisodeRsp abort_response;
                const bool abort_rpc_ok =
                    client.AbortEpisode(abort_request, abort_response);
                const bool abort_applied =
                    abort_rpc_ok &&
                    AcceptCommandReply(cursor,
                                       abort_response.lifecycle());
                if (!abort_applied) {
                    lifecycle_outcome_unknown =
                        !abort_rpc_ok
                            ? client.LastRpcOutcomeUnknown()
                            : !IsConclusiveRejected(
                                  cursor, abort_response.lifecycle());
                    LOG_ERROR(
                        "Main",
                        "AbortEpisode(task-stop) 未收敛: seq=%llu "
                        "applied=%llu outcome_unknown=%d message=%s",
                        static_cast<unsigned long long>(
                            abort_request.command().command_sequence()),
                        static_cast<unsigned long long>(
                            abort_response.lifecycle().applied_sequence()),
                        lifecycle_outcome_unknown ? 1 : 0,
                        abort_response.lifecycle().message().c_str());
                    chain_failed = true;
                } else {
                    episode_active = false;
                }
                break;
            }
            if (terminal_report) break;
            for (const auto& action : update_response.actions()) {
                const int agent_id = static_cast<int>(action.agent_id());
                const int action_frame_id = environment.GetFrameId();
                const int result_frame_id = action_frame_id + 1;
                const auto before = environment.GetAgent(agent_id);
                environment.Step(agent_id, action.action_id());
                const auto& after = environment.GetAgent(agent_id);
                if (!RecordExecutedAction(
                        agent_execution[static_cast<std::size_t>(agent_id)],
                        action.action_id())) {
                    LOG_ERROR(
                        "Main",
                        "Agent 动作执行状态无法提交: frame=%d agent_id=%d "
                        "action_id=%d",
                        action_frame_id, agent_id, action.action_id());
                    chain_failed = true;
                    break;
                }
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
                if (after.done ||
                    result_frame_id % config.run.log_interval == 0) {
                    if (after.done) {
                        LOG_INFO(
                            "Exec",
                            "episode=%s action_frame_id=%d "
                            "result_frame_id=%d agent_id=%d state=%s "
                            "action_id=%d action=%s from=(%d,%d) "
                            "to=(%d,%d) blocked=%s terminal=true",
                            cursor.episode_id.c_str(), action_frame_id,
                            result_frame_id, agent_id,
                            AgentStateName(after.termination_reason),
                            action.action_id(),
                            kActionNames[action.action_id()], before.grid_x,
                            before.grid_y, after.grid_x, after.grid_y,
                            after.last_move_blocked ? "true" : "false");
                    } else {
                        LOG_INFO(
                            "Exec",
                            "episode=%s action_frame_id=%d "
                            "result_frame_id=%d agent_id=%d state=active "
                            "action_id=%d action=%s from=(%d,%d) "
                            "to=(%d,%d) blocked=%s",
                            cursor.episode_id.c_str(), action_frame_id,
                            result_frame_id, agent_id, action.action_id(),
                            kActionNames[action.action_id()], before.grid_x,
                            before.grid_y, after.grid_x, after.grid_y,
                            after.last_move_blocked ? "true" : "false");
                    }
                }
            }
            if (chain_failed) break;
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
                FillCommand(cursor, "abort-chain-failure",
                            abort_request.mutable_command());
                abort_request.set_reason(
                    maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE);
                abort_request.set_message(
                    "Client stopped the Episode after a chain failure");
                maze::AbortEpisodeRsp abort_response;
                const bool abort_rpc_ok =
                    client.AbortEpisode(abort_request, abort_response);
                if (abort_rpc_ok &&
                    AcceptCommandReply(cursor,
                                       abort_response.lifecycle())) {
                    episode_active = false;
                } else {
                    lifecycle_outcome_unknown =
                        !abort_rpc_ok
                            ? client.LastRpcOutcomeUnknown()
                            : !IsConclusiveRejected(
                                  cursor, abort_response.lifecycle());
                    LOG_ERROR(
                        "Main",
                        "AbortEpisode(chain-failure) 未收敛: seq=%llu "
                        "applied=%llu outcome_unknown=%d message=%s",
                        static_cast<unsigned long long>(
                            abort_request.command().command_sequence()),
                        static_cast<unsigned long long>(
                            abort_response.lifecycle().applied_sequence()),
                        lifecycle_outcome_unknown ? 1 : 0,
                        abort_response.lifecycle().message().c_str());
                }
            }
            break;
        }
        if (g_stop_requested.load() && episode_active) {
            maze::AbortEpisodeReq abort_request;
            FillCommand(cursor, "abort-client-stop",
                        abort_request.mutable_command());
            abort_request.set_reason(
                maze::MAZE_TERMINATION_REASON_CLIENT_ABORT);
            abort_request.set_message("Client received stop signal");
            maze::AbortEpisodeRsp abort_response;
            const bool abort_rpc_ok =
                client.AbortEpisode(abort_request, abort_response);
            if (!abort_rpc_ok ||
                !AcceptCommandReply(cursor, abort_response.lifecycle())) {
                lifecycle_outcome_unknown =
                    !abort_rpc_ok
                        ? client.LastRpcOutcomeUnknown()
                        : !IsConclusiveRejected(
                              cursor, abort_response.lifecycle());
                LOG_ERROR(
                    "Main",
                    "AbortEpisode(client-stop) 未收敛: seq=%llu "
                    "applied=%llu outcome_unknown=%d message=%s",
                    static_cast<unsigned long long>(
                        abort_request.command().command_sequence()),
                    static_cast<unsigned long long>(
                        abort_response.lifecycle().applied_sequence()),
                    lifecycle_outcome_unknown ? 1 : 0,
                    abort_response.lifecycle().message().c_str());
                chain_failed = true;
            } else {
                episode_active = false;
            }
            break;
        }
        if (!episode_active) break;

        maze::EndEpisodeReq end_request;
        FillCommand(cursor, "end-episode", end_request.mutable_command());
        maze::EndEpisodeRsp end_response;
        if (!client.EndEpisode(end_request, end_response) ||
            !AcceptCommandReply(cursor, end_response.lifecycle())) {
            lifecycle_outcome_unknown =
                client.LastRpcOutcomeUnknown() ||
                !IsConclusiveRejected(cursor, end_response.lifecycle());
            LOG_ERROR(
                "Main",
                "EndEpisode 生命周期未收敛: seq=%llu applied=%llu "
                "outcome_unknown=%d message=%s",
                static_cast<unsigned long long>(
                    end_request.command().command_sequence()),
                static_cast<unsigned long long>(
                    end_response.lifecycle().applied_sequence()),
                lifecycle_outcome_unknown ? 1 : 0,
                end_response.lifecycle().message().c_str());
            chain_failed = true;
            break;
        }
        episode_active = false;
        ++episode_number;
    }

    if (!episode_active && !lifecycle_outcome_unknown) {
        maze::CloseSessionReq close_request;
        FillCommand(cursor, "close-session", close_request.mutable_command());
        maze::CloseSessionRsp close_response;
        if (!client.CloseSession(close_request, close_response) ||
            !AcceptCommandReply(cursor, close_response.lifecycle())) {
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
