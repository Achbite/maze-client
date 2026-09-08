#include "task/maze_client_binding.h"
#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"
#include "grpc/update_flow_control.h"
#include "grpc/workload_contract.h"
#include "grpc/abort_episode_transaction.h"
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

using maze_client::AgentExecutionCursor;
using maze_client::AttachExecutedActionReceipt;
using rl_sdk::IsConclusiveRejected;
using maze_client::PrepareAppliedAgentUpdate;
using maze_client::RecordExecutedAction;

constexpr const char* kDefaultConfigPath = "configs/client_config.yaml";
constexpr const char* kManagedReadyMarker =
    "/run/rl/client-managed-ready";
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

struct MazeClientBinding::Impl {
    ClientConfig config;
    MazeEnv& environment;
    GrpcClient client;
    rl_sdk::Session session;
    const rl_sdk::LifecycleCursor& cursor = session.cursor();
    maze::OpenSessionRsp open_response;
    VizRecorder recorder;
    std::vector<int> last_actions;
    std::vector<AgentExecutionCursor> agent_execution;
    int episode_number = 0;
    bool chain_failed = false;
    bool task_complete = false;
    Impl(const ClientConfig& cfg, MazeEnv& env) : config(cfg), environment(env) {}
    bool Stopped() const { return g_stop_requested.load(); }
    bool Active() const { return cursor.phase == rl::session::v1::SESSION_PHASE_EPISODE_RUNNING; }
    bool Complete() const { return task_complete; }
    bool CanClose() const { return !cursor.session_id.empty() && !Active() &&
        cursor.phase != rl::session::v1::SESSION_PHASE_CLOSED; }

    rl_sdk::CommandOutcome Open() {
    if (!client.Connect(config.network.server_host,
                        config.network.server_port)) {
        return rl_sdk::CommandOutcome::Rejected;
    }

    const char* managed = std::getenv("RL_INFRA_MANAGED");
    if (managed != nullptr && std::string(managed) == "true") {
        std::string readiness_error;
        if (!PublishManagedReadyMarker(config.network.server_host,
                                       readiness_error)) {
            LOG_ERROR("Main", "Client managed readiness 发布失败: %s",
                      readiness_error.c_str());
            client.Disconnect();

            return rl_sdk::CommandOutcome::Unknown;
        }
        LOG_INFO("Main", "Client managed-ready; entering OpenSession");
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
    if (!client.OpenSession(open_request, open_response) ||
        !rl_sdk::CommandAccepted(open_response.reply()) ||
        open_response.reply().applied_sequence() != 0 ||
        open_response.reply().phase() != rl::session::v1::SESSION_PHASE_OPEN ||
        open_response.session_id().empty() ||
        open_response.session_epoch() == 0 ||
        open_response.aiserver().component().empty() ||
        open_response.aiserver().instance_id().empty() ||
        open_response.aiserver().lifecycle_epoch() == 0 ||
        !open_response.has_environment() ||
        open_response.environment().agent_count() == 0 ||
        open_response.environment().map_id().empty() ||
        open_response.environment().episode_max_steps() == 0 ||
        (open_response.environment().action_mask_mode() !=
             maze::ACTION_MASK_MODE_DISABLED &&
         open_response.environment().action_mask_mode() !=
             maze::ACTION_MASK_MODE_REQUIRED)) {
        LOG_ERROR("Main", "OpenSession 返回了无效的 Environment 身份");

        return rl_sdk::CommandOutcome::Unknown;
    }

    const std::string workload = WorkloadName(open_response.workload_mode());
    const bool replay_expected =
        open_response.workload_mode() == maze::WORKLOAD_MODE_EVALUATION;
    if (workload.empty()) {
        LOG_ERROR("Main", "OpenSession workload 无效");

        return rl_sdk::CommandOutcome::Unknown;
    }
    session.Bind(open_response.session_id(), open_response.session_epoch(), open_response.reply());

    config.run.agent_num =
        static_cast<int>(open_response.environment().agent_count());
    config.run.workload = workload;
    config.viz.recording_enabled = replay_expected;

        return rl_sdk::CommandOutcome::Applied;
    }
    rl_sdk::CommandOutcome Initialize() {
    const std::string map_file = ResolveTaskMapFile(
        config.env.map_registry_dir,
        open_response.environment().map_id());
    if (map_file.empty()) {
        LOG_ERROR("Main", "TaskSpec 地图未在 registry 精确命中");

        return rl_sdk::CommandOutcome::Rejected;
    }
    config.env.map_file = map_file;
    if (!environment.Init(config) ||
        environment.GetMapId() != open_response.environment().map_id()) {
        LOG_ERROR("Main", "Client 地图 registry 身份不匹配");

        return rl_sdk::CommandOutcome::Rejected;
    }

    maze::InitReq init_request;
    session.Prepare(init_request);
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
    const auto init_result = session.Exchange(init_request, init_response,
        [&](const auto& req, auto& rsp) { return client.Init(req, rsp); },
        [](const auto&, const auto&) { return true; },
        [](const auto&) { return 0; });
        if (init_result != rl_sdk::CommandOutcome::Applied)
            LOG_ERROR("Client SDK", "Init failed: %s", init_response.reply().message().c_str());
        return init_result;
    }
    rl_sdk::CommandOutcome Begin() {
        session.SetEpisode({});
        maze::BeginEpisodeReq request;
        maze::BeginEpisodeRsp begin_response;
        session.Prepare(request);
        const auto result = session.Exchange(request, begin_response,
            [&](const auto& req, auto& rsp) { return client.BeginEpisode(req, rsp); },
            [](const auto&, const auto&) { return true; },
            [](const auto& rsp) { return rsp.reply().result() == rl::session::v1::COMMAND_RESULT_WAIT ? 100 : 0; },
            [&] { return Stopped(); });
        if (result != rl_sdk::CommandOutcome::Applied) return result;
        if (begin_response.has_task_complete()) {
            if (cursor.phase != rl::session::v1::SESSION_PHASE_TASK_COMPLETE) return rl_sdk::CommandOutcome::Unknown;
            task_complete = true;
            LOG_INFO("Client SDK", "AIServer completed the task");
            return result;
        }
        if (!begin_response.has_assignment()) return rl_sdk::CommandOutcome::Unknown;
        const auto& assignment = begin_response.assignment();
        if (cursor.phase == rl::session::v1::SESSION_PHASE_EPISODE_RUNNING) {
            session.SetEpisode(assignment.episode_id());
        }
        const std::string mode = EpisodeModeName(assignment.mode());
        const bool training_assignment =
            assignment.mode() == maze::EPISODE_MODE_TRAINING;
        const bool evaluation_assignment =
            assignment.mode() == maze::EPISODE_MODE_EVALUATION;
        if (cursor.phase != rl::session::v1::SESSION_PHASE_EPISODE_RUNNING ||
            assignment.episode_id().empty() ||
            mode.empty() ||
            assignment.max_steps() !=
                open_response.environment().episode_max_steps() ||
            (!training_assignment && !evaluation_assignment) ||
            !maze_client::AssignmentMatchesWorkload(
                open_response.workload_mode(), assignment)) {
            LOG_ERROR("Main", "EpisodeAssignment 身份、模式或 horizon 无效");
            return rl_sdk::CommandOutcome::Rejected;
        }
        environment.SetMaxSteps(static_cast<int>(assignment.max_steps()));
        environment.Reset();
        last_actions.assign(config.run.agent_num, 0);
        agent_execution.assign(static_cast<std::size_t>(environment.GetAgentNum()), {});
        if (config.viz.recording_enabled &&
            !recorder.Begin(config.viz.output_dir, episode_number,
                            environment.GetMapId(),
                            environment.GetMapFilePath())) {
            LOG_ERROR("Main", "Replay 帧记录器启动失败");
            return rl_sdk::CommandOutcome::Rejected;
        }
        LOG_INFO(
            "Main",
            "Episode %s 开始 mode=%s max_steps=%u",
            cursor.episode_id.c_str(), mode.c_str(),
            assignment.max_steps());

        return rl_sdk::CommandOutcome::Applied;
    }
    rl_sdk::CommandOutcome RunEpisode() {
        maze::UpdateRsp update_response;
        const auto episode_result = rl_sdk::RunEpisode(
            [&] {
            maze::UpdateReq update_request;
            session.Prepare(update_request);
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
            return update_request;

            },
            [&](const maze::UpdateReq& update_request) {
            if (chain_failed) return rl_sdk::CommandOutcome::Rejected;
            std::vector<AgentExecutionCursor> candidate_execution;
            const auto update_result = session.Exchange(update_request, update_response,
                [&](const auto& req, auto& rsp) { return client.Update(req, rsp); },
                [&](const auto& req, const auto& rsp) {
                    return PrepareAppliedAgentUpdate(req, rsp, agent_execution, candidate_execution);
                },
                [&](const auto& rsp) -> int64_t {
                    if (!rsp.has_wait()) return 0;
                    return maze_client::IsUpdateWait(rsp, cursor.next_sequence, cursor.phase)
                        ? rsp.wait().retry_after_ms() : -1;
                }, [] { return g_stop_requested.load(); });
            if (update_result == rl_sdk::CommandOutcome::Applied) {
                agent_execution = std::move(candidate_execution);
            } else if (update_result != rl_sdk::CommandOutcome::Stopped) {
                const bool lifecycle_outcome_unknown = update_result == rl_sdk::CommandOutcome::Unknown;
                LOG_ERROR("Main", "Update failed: outcome_unknown=%d message=%s",
                          lifecycle_outcome_unknown, update_response.reply().message().c_str());
                chain_failed = true;
            }
            return update_result;
            },
            [&] { return environment.AllDone(); },
            [&] {
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
                    return false;
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
            if (chain_failed) return false;
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
            return true;
            }, [] { return g_stop_requested.load(); });
        if (episode_result != rl_sdk::CommandOutcome::Applied &&
            episode_result != rl_sdk::CommandOutcome::Stopped) chain_failed = true;

        recorder.End();
        return episode_result;
    }
    rl_sdk::CommandOutcome End() {
        maze::EndEpisodeReq request;
        maze::EndEpisodeRsp response;
        session.Prepare(request);
        const auto result = session.Exchange(request, response,
            [&](const auto& req, auto& rsp) { return client.EndEpisode(req, rsp); },
            [](const auto&, const auto&) { return true; }, [](const auto&) { return 0; });
        if (result != rl_sdk::CommandOutcome::Applied) return result;
        std::string outcome_log, outcome_error;
        if (!RenderEpisodeOutcome(response, cursor.episode_id, environment.GetAgentNum(), outcome_log, outcome_error)) {
            LOG_ERROR("Client SDK", "%s", outcome_error.c_str());
            return rl_sdk::CommandOutcome::Rejected;
        }
        LOG_INFO("Outcome", "%s", outcome_log.c_str());
        session.SetEpisode({});
        ++episode_number;
        return result;
    }
    rl_sdk::CommandOutcome Abort() {
        recorder.End();
        maze::AbortEpisodeReq request;
        maze::AbortEpisodeRsp response;
        session.Prepare(request);
        request.set_reason(Stopped() ? maze::MAZE_TERMINATION_REASON_CLIENT_ABORT : maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE);
        request.set_message(Stopped() ? "Client received stop signal" : "Client stopped the Episode after a chain failure");
        bool unknown = false;
        if (!maze_client::ApplyAbortEpisode(client, session, request, response,
                std::chrono::milliseconds(config.network.abort_wait_timeout_ms), unknown)) {
            LOG_ERROR("Client SDK", "AbortEpisode failed: %s", response.reply().message().c_str());
            return unknown ? rl_sdk::CommandOutcome::Unknown : rl_sdk::CommandOutcome::Rejected;
        }
        session.SetEpisode({});
        return rl_sdk::CommandOutcome::Applied;
    }
    rl_sdk::CommandOutcome Close() {
        maze::CloseSessionReq request;
        maze::CloseSessionRsp response;
        session.Prepare(request);
        return session.Exchange(request, response,
            [&](const auto& req, auto& rsp) { return client.CloseSession(req, rsp); },
            [](const auto&, const auto&) { return true; }, [](const auto&) { return 0; });
    }
};
MazeClientBinding::MazeClientBinding(const ClientConfig& config, MazeEnv& environment)
    : impl_(std::make_unique<Impl>(config, environment)) {}
MazeClientBinding::~MazeClientBinding() = default;
rl_sdk::CommandOutcome MazeClientBinding::Run() { return rl_sdk::RunSession(*impl_); }

int RunMazeClient(int argc, char* argv[]) {
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
    MazeClientBinding binding(config, environment);
    const auto result = binding.Run();
    const bool clean = result == rl_sdk::CommandOutcome::Applied || result == rl_sdk::CommandOutcome::Stopped;
    if (!clean) LOG_ERROR("Client SDK", "Session ended: outcome=%d", static_cast<int>(result));
    Logger::Instance().Close();
    return clean ? 0 : 1;
}
