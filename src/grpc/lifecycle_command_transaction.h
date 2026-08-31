#pragma once

#include "env/maze_env.h"
#include "maze_task.pb.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace maze_client {

namespace task = rl::task::maze::v1;

struct LifecycleCursor {
    std::string session_id;
    std::string episode_id;
    std::uint64_t session_epoch = 0;
    std::uint64_t next_sequence = 1;
    task::SessionPhase phase = task::SESSION_PHASE_UNSPECIFIED;
};

inline void UpdateCursor(LifecycleCursor& cursor,
                         const task::CommandReply& reply) {
    cursor.phase = reply.phase();
}

// Constructing a command is side-effect free. The sequence is committed only
// after the server proves that this exact command was applied.
inline void FillCommand(const LifecycleCursor& cursor,
                        task::CommandIdentity* command) {
    command->set_session_id(cursor.session_id);
    command->set_session_epoch(cursor.session_epoch);
    command->set_sequence(cursor.next_sequence);
    command->set_episode_id(cursor.episode_id);
}

inline bool HasConcretePhase(const task::CommandReply& reply) {
    return reply.phase() >= task::SESSION_PHASE_OPEN &&
           reply.phase() <= task::SESSION_PHASE_ABORTED;
}

inline bool CommandAccepted(const task::CommandReply& reply) {
    return (reply.result() == task::COMMAND_RESULT_APPLIED ||
            reply.result() == task::COMMAND_RESULT_ALREADY_APPLIED) &&
           reply.error_code() == task::COMMAND_ERROR_CODE_UNSPECIFIED;
}

inline bool AcceptCommandReply(LifecycleCursor& cursor,
                               const task::CommandReply& reply) {
    const std::uint64_t expected_sequence = cursor.next_sequence;
    if (!CommandAccepted(reply) ||
        reply.applied_sequence() != expected_sequence ||
        !HasConcretePhase(reply)) {
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
                          const task::CommandReply& reply) {
    return cursor.next_sequence > 0 &&
           reply.result() == task::COMMAND_RESULT_WAIT &&
           reply.error_code() == task::COMMAND_ERROR_CODE_UNSPECIFIED &&
           reply.applied_sequence() == cursor.next_sequence - 1 &&
           HasConcretePhase(reply) && reply.phase() == cursor.phase;
}

// A rejected command is safe to replace with another operation at the same
// sequence only when the server proves that it committed no lifecycle state.
inline bool IsConclusiveRejected(const LifecycleCursor& cursor,
                                 const task::CommandReply& reply) {
    return cursor.next_sequence > 0 &&
           reply.result() == task::COMMAND_RESULT_REJECTED &&
           reply.error_code() != task::COMMAND_ERROR_CODE_UNSPECIFIED &&
           reply.applied_sequence() == cursor.next_sequence - 1 &&
           HasConcretePhase(reply) && reply.phase() == cursor.phase;
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
        !CommandAccepted(response.reply()) ||
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
