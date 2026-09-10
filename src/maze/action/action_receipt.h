#pragma once

#include "maze/environment/maze_env.h"
#include "proto/maze/maze.pb.h"
#include "rl_sdk/session.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace maze_client {

namespace task = rl::task::maze::v1;

struct AgentExecutionCursor {
    bool retired = false;
    std::optional<std::int32_t> last_executed_action_id;
};

inline bool IsMazeActionId(std::int32_t action_id) {
    return action_id >= 0 && action_id <= 8;
}

inline std::size_t ActiveAgentCount(
    const std::vector<AgentExecutionCursor>& agents) {
    std::size_t active = 0;
    for (const auto& agent : agents) {
        if (!agent.retired) ++active;
    }
    return active;
}

inline bool AttachExecutedActionReceipt(
    std::uint64_t frame_id,
    const AgentExecutionCursor& cursor,
    task::AgentState* state) {
    if (state == nullptr || cursor.retired) return false;
    if (frame_id == 0) {
        return !cursor.last_executed_action_id.has_value();
    }
    if (!cursor.last_executed_action_id.has_value() ||
        !IsMazeActionId(*cursor.last_executed_action_id)) {
        return false;
    }
    state->set_executed_action_id(
        static_cast<task::MazeAction>(*cursor.last_executed_action_id));
    return true;
}

inline bool RecordExecutedAction(AgentExecutionCursor& cursor,
                                 std::int32_t action_id) {
    if (cursor.retired || cursor.last_executed_action_id.has_value() ||
        !IsMazeActionId(action_id)) {
        return false;
    }
    cursor.last_executed_action_id = action_id;
    return true;
}

// Apply the exact action returned by AIServer to the Client-owned environment,
// then retain the execution receipt consumed by the next Update request.
inline bool ExecuteAssignedAction(
    MazeEnv& environment,
    AgentExecutionCursor& cursor,
    const task::AgentAction& action,
    std::string& error) {
    if (!environment.Step(
            static_cast<int>(action.agent_id()), action.action_id(), error)) {
        return false;
    }
    if (!RecordExecutedAction(cursor, action.action_id())) {
        error = "executed action cannot be committed to the Agent receipt";
        return false;
    }
    return true;
}

// Validate the complete Agent sub-transaction without mutating the live
// cursor. The caller commits this candidate only after AcceptCommandReply
// proves that the exact lifecycle command was applied.
inline bool PrepareAppliedAgentUpdate(
    const task::UpdateReq& request,
    const task::UpdateRsp& response,
    const std::vector<AgentExecutionCursor>& current,
    std::vector<AgentExecutionCursor>& candidate) {
    if (current.empty() ||
        !response.has_action_batch() ||
        !rl_sdk::CommandAccepted(response.reply()) ||
        request.agents_size() !=
            static_cast<int>(ActiveAgentCount(current))) {
        return false;
    }

    std::unordered_set<std::uint32_t> reported;
    std::unordered_set<std::uint32_t> expected_actions;
    for (const auto& state : request.agents()) {
        const auto agent_id = state.agent_id();
        if (agent_id >= current.size() || current[agent_id].retired ||
            !reported.insert(agent_id).second) {
            return false;
        }
        const auto& cursor = current[agent_id];
        if (request.frame_id() == 0) {
            if (state.has_executed_action_id() ||
                cursor.last_executed_action_id.has_value()) {
                return false;
            }
        } else if (!state.has_executed_action_id() ||
                   !IsMazeActionId(state.executed_action_id()) ||
                   !cursor.last_executed_action_id.has_value() ||
                   state.executed_action_id() !=
                       *cursor.last_executed_action_id) {
            return false;
        }
        if (!state.is_done()) expected_actions.insert(agent_id);
    }

    if (response.action_batch().actions_size() !=
        static_cast<int>(expected_actions.size())) {
        return false;
    }
    std::unordered_set<std::uint32_t> returned;
    for (const auto& action : response.action_batch().actions()) {
        if (!IsMazeActionId(action.action_id()) ||
            expected_actions.find(action.agent_id()) ==
                expected_actions.end() ||
            !returned.insert(action.agent_id()).second) {
            return false;
        }
        for (const auto& state : request.agents()) {
            if (state.agent_id() != action.agent_id() ||
                state.action_mask_size() == 0) {
                continue;
            }
            const auto action_index = static_cast<int>(action.action_id());
            if (action_index >= state.action_mask_size() ||
                !state.action_mask(action_index)) {
                return false;
            }
            break;
        }
    }

    candidate = current;
    for (const auto& state : request.agents()) {
        auto& cursor = candidate[state.agent_id()];
        cursor.last_executed_action_id.reset();
        if (state.is_done()) cursor.retired = true;
    }
    return true;
}

}  // namespace maze_client
