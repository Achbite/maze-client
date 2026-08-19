#pragma once

#include "maze_task.pb.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace maze_client {

namespace task = rl::task::maze::v1;

struct LifecycleCursor {
    task::TaskIdentity task;
    std::string session_id;
    std::string episode_id;
    std::uint64_t lifecycle_epoch = 0;
    std::uint64_t next_sequence = 1;
    task::TaskState task_state = task::TASK_STATE_UNSPECIFIED;
    task::SessionState session_state = task::SESSION_STATE_UNSPECIFIED;
    task::EpisodeState episode_state = task::EPISODE_STATE_UNSPECIFIED;
};

inline void UpdateCursor(LifecycleCursor& cursor,
                         const task::LifecycleReply& reply) {
    cursor.task_state = reply.task_state();
    cursor.session_state = reply.session_state();
    cursor.episode_state = reply.episode_state();
}

// Constructing a command is side-effect free. The sequence is committed only
// after the server proves that this exact command was applied.
inline void FillCommand(const LifecycleCursor& cursor,
                        const std::string& operation,
                        task::LifecycleCommand* command) {
    command->mutable_task()->CopyFrom(cursor.task);
    command->set_session_id(cursor.session_id);
    command->set_episode_id(cursor.episode_id);
    command->set_lifecycle_epoch(cursor.lifecycle_epoch);
    command->set_command_sequence(cursor.next_sequence);
    command->set_idempotency_key(
        cursor.session_id + ":" + std::to_string(cursor.lifecycle_epoch) +
        ":" + std::to_string(cursor.next_sequence) + ":" + operation);
    command->set_expected_task_state(cursor.task_state);
    command->set_expected_session_state(cursor.session_state);
    command->set_expected_episode_state(cursor.episode_state);
}

inline bool HasConcreteLifecycleState(const task::LifecycleReply& reply) {
    const bool task_known =
        reply.task_state() >= task::TASK_STATE_CREATED &&
        reply.task_state() <= task::TASK_STATE_FAILED;
    const bool session_known =
        reply.session_state() >= task::SESSION_STATE_OPENED &&
        reply.session_state() <= task::SESSION_STATE_CLOSED;
    const bool episode_known =
        reply.episode_state() >= task::EPISODE_STATE_UNSPECIFIED &&
        reply.episode_state() <= task::EPISODE_STATE_ABORTED;
    if (!task_known || !session_known || !episode_known) {
        return false;
    }
    if (reply.session_state() == task::SESSION_STATE_EPISODE_ACTIVE) {
        return reply.episode_state() == task::EPISODE_STATE_RUNNING ||
               reply.episode_state() ==
                   task::EPISODE_STATE_TERMINAL_REPORTED;
    }
    return reply.episode_state() == task::EPISODE_STATE_UNSPECIFIED ||
           reply.episode_state() == task::EPISODE_STATE_COMMITTED ||
           reply.episode_state() == task::EPISODE_STATE_ABORTED;
}

inline bool LifecycleAccepted(const task::LifecycleReply& reply) {
    return reply.ret_code() == 0 &&
           (reply.result() == task::LIFECYCLE_RESULT_APPLIED ||
            reply.result() == task::LIFECYCLE_RESULT_ALREADY_APPLIED) &&
           reply.error_code() == task::LIFECYCLE_ERROR_CODE_UNSPECIFIED;
}

inline bool AcceptCommandReply(LifecycleCursor& cursor,
                               const task::LifecycleReply& reply) {
    const std::uint64_t expected_sequence = cursor.next_sequence;
    if (!LifecycleAccepted(reply) ||
        reply.applied_sequence() != expected_sequence ||
        !HasConcreteLifecycleState(reply)) {
        return false;
    }
    UpdateCursor(cursor, reply);
    cursor.next_sequence = expected_sequence + 1;
    return true;
}

// WAIT is an authoritative proof that this exact command was not applied. It
// is safe only when the server echoes the committed lifecycle cursor and the
// last applied sequence; callers must retry the unchanged command bytes.
inline bool IsCommandWait(const LifecycleCursor& cursor,
                          const task::LifecycleReply& reply) {
    return cursor.next_sequence > 0 && reply.ret_code() == 0 &&
           reply.result() == task::LIFECYCLE_RESULT_WAIT &&
           reply.error_code() ==
               task::LIFECYCLE_ERROR_CODE_UNSPECIFIED &&
           reply.applied_sequence() == cursor.next_sequence - 1 &&
           HasConcreteLifecycleState(reply) &&
           reply.task_state() == cursor.task_state &&
           reply.session_state() == cursor.session_state &&
           reply.episode_state() == cursor.episode_state;
}

// A rejected command is safe to replace with another operation at the same
// sequence only when the server proves that it committed no lifecycle state.
inline bool IsConclusiveRejected(const LifecycleCursor& cursor,
                                 const task::LifecycleReply& reply) {
    return cursor.next_sequence > 0 &&
           reply.ret_code() != 0 &&
           reply.result() == task::LIFECYCLE_RESULT_REJECTED &&
           reply.error_code() !=
               task::LIFECYCLE_ERROR_CODE_UNSPECIFIED &&
           reply.applied_sequence() == cursor.next_sequence - 1 &&
           HasConcreteLifecycleState(reply) &&
           reply.task_state() == cursor.task_state &&
           reply.session_state() == cursor.session_state &&
           reply.episode_state() == cursor.episode_state;
}

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
    state->set_executed_action_id(*cursor.last_executed_action_id);
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

// Validate the complete Agent sub-transaction without mutating the live
// cursor. The caller commits this candidate only after AcceptCommandReply
// proves that the exact lifecycle command was applied.
inline bool PrepareAppliedAgentUpdate(
    const task::UpdateReq& request,
    const task::UpdateRsp& response,
    const std::vector<AgentExecutionCursor>& current,
    std::vector<AgentExecutionCursor>& candidate) {
    if (current.empty() ||
        response.environment_control() !=
            task::ENVIRONMENT_CONTROL_ADVANCE ||
        response.task_stop_requested() ||
        !LifecycleAccepted(response.lifecycle()) ||
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

    if (response.actions_size() !=
        static_cast<int>(expected_actions.size())) {
        return false;
    }
    std::unordered_set<std::uint32_t> returned;
    for (const auto& action : response.actions()) {
        if (!IsMazeActionId(action.action_id()) ||
            expected_actions.find(action.agent_id()) ==
                expected_actions.end() ||
            !returned.insert(action.agent_id()).second) {
            return false;
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
