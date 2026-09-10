#include "maze/protocol/client_adapter.h"
#include "maze/action/action_receipt.h"
#include "maze/episode/assignment.h"
#include "maze/environment/maze_env.h"
#include "maze/config/config_loader.h"
#include "maze/viz/viz_recorder.h"
#include "log/logger.h"

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace maze = rl::task::maze::v1;
namespace {
using maze_client::AgentExecutionCursor;
using maze_client::AttachExecutedActionReceipt;
using maze_client::PrepareAppliedAgentUpdate;
const char* kActionNames[9] = {
    "不动", "上", "右上", "右", "右下", "下", "左下", "左", "左上"
};

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

struct MazeClientAdapter::Impl {
    ClientConfig config;
    MazeEnv& environment;
    Client& client;
    std::function<bool()> stopped;
    const rl_sdk::LifecycleCursor& cursor = client.cursor();
    maze::OpenSessionRsp open_response;
    VizRecorder recorder;
    std::vector<int> last_actions;
    std::vector<AgentExecutionCursor> agent_execution;
    int episode_number = 0;
    bool chain_failed = false;
    Impl(const ClientConfig& cfg, MazeEnv& env, Client& transport, std::function<bool()> stop)
        : config(cfg), environment(env), client(transport), stopped(std::move(stop)) {}
    bool Stopped() const { return stopped(); }
    bool Active() const { return client.Active(); }
    bool Complete() const { return client.Complete(); }
    bool CanClose() const { return client.CanClose(); }

    rl_sdk::CommandOutcome Open() {
        const auto opened = client.OpenSession(open_response, [](const auto&, const auto& response) {
            return response.has_environment() && response.environment().agent_count() > 0 &&
                !response.environment().map_id().empty() && response.environment().episode_max_steps() > 0 &&
                (response.environment().action_mask_mode() == maze::ACTION_MASK_MODE_DISABLED ||
                 response.environment().action_mask_mode() == maze::ACTION_MASK_MODE_REQUIRED);
        });
        if (opened != rl_sdk::CommandOutcome::Applied) {
            LOG_ERROR("Main", "OpenSession: %s", client.error().c_str());
            return opened;
        }

        const std::string workload = WorkloadName(open_response.workload_mode());
        const bool replay_expected =
            open_response.workload_mode() == maze::WORKLOAD_MODE_EVALUATION;
        if (workload.empty()) {
            LOG_ERROR("Main", "OpenSession workload 无效");

            return rl_sdk::CommandOutcome::Unknown;
        }

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
        const auto init_result = client.Init(std::move(init_request), init_response);
        if (init_result != rl_sdk::CommandOutcome::Applied)
            LOG_ERROR("Client SDK", "Init failed: %s", init_response.reply().message().c_str());
        return init_result;
    }
    rl_sdk::CommandOutcome Begin() {
        maze::BeginEpisodeRsp begin_response;
        const auto result = client.BeginEpisode(begin_response,
            [](const auto&, const auto&) { return true; }, [&] { return Stopped(); });
        if (result != rl_sdk::CommandOutcome::Applied) return result;
        if (begin_response.has_task_complete()) {
            LOG_INFO("Client SDK", "AIServer completed the task");
            return result;
        }
        if (!begin_response.has_assignment()) return rl_sdk::CommandOutcome::Unknown;
        const auto& assignment = begin_response.assignment();
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
            const auto update_result = client.Update(update_request, update_response,
                [&](const auto& req, const auto& rsp) {
                    return PrepareAppliedAgentUpdate(req, rsp, agent_execution, candidate_execution);
                }, [&] { return Stopped(); });
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
            }, [&] { return Stopped(); });
        if (episode_result != rl_sdk::CommandOutcome::Applied &&
            episode_result != rl_sdk::CommandOutcome::Stopped) chain_failed = true;

        recorder.End();
        return episode_result;
    }
    rl_sdk::CommandOutcome End() {
        maze::EndEpisodeRsp response;
        const auto episode_id = cursor.episode_id;
        std::string outcome_log, outcome_error;
        const auto result = client.EndEpisode(response, [&](const auto&, const auto& rsp) {
            return RenderEpisodeOutcome(rsp, episode_id, environment.GetAgentNum(), outcome_log, outcome_error);
        });
        if (result != rl_sdk::CommandOutcome::Applied) {
            LOG_ERROR("Client SDK", "%s", outcome_error.empty() ? client.error().c_str() : outcome_error.c_str());
            return result;
        }
        LOG_INFO("Outcome", "%s", outcome_log.c_str());
        ++episode_number;
        return result;
    }
    rl_sdk::CommandOutcome Abort() {
        recorder.End();
        maze::AbortEpisodeReq request;
        maze::AbortEpisodeRsp response;
        request.set_reason(Stopped() ? maze::MAZE_TERMINATION_REASON_CLIENT_ABORT : maze::MAZE_TERMINATION_REASON_CHAIN_FAILURE);
        request.set_message(Stopped() ? "Client received stop signal" : "Client stopped the Episode after a chain failure");
        const auto result = client.AbortEpisode(std::move(request), response);
        if (result != rl_sdk::CommandOutcome::Applied)
            LOG_ERROR("Client SDK", "AbortEpisode: %s", client.error().c_str());
        return result;
    }
    rl_sdk::CommandOutcome Close() {
        maze::CloseSessionRsp response;
        return client.CloseSession(response);
    }
};
MazeClientAdapter::MazeClientAdapter(const ClientConfig& config, MazeEnv& environment,
                                     Client& client, std::function<bool()> stopped)
    : impl_(std::make_unique<Impl>(config, environment, client, std::move(stopped))) {}
MazeClientAdapter::~MazeClientAdapter() = default;
rl_sdk::CommandOutcome MazeClientAdapter::Run() { return rl_sdk::RunSession(*impl_); }
