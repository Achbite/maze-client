#pragma once

#include "maze_task.pb.h"

#include <cstdint>

namespace maze_client {

namespace task = rl::task::maze::v1;

inline bool IsTrainingCapacityWait(
    const task::UpdateRsp& response,
    std::uint64_t pending_sequence,
    task::TaskState task_state,
    task::SessionState session_state,
    task::EpisodeState episode_state,
    task::EvaluationState evaluation_state) {
    const auto& reply = response.lifecycle();
    return reply.ret_code() == 0 &&
           reply.result() == task::LIFECYCLE_RESULT_WAIT &&
           reply.error_code() == task::LIFECYCLE_ERROR_CODE_UNSPECIFIED &&
           response.environment_control() ==
               task::ENVIRONMENT_CONTROL_WAIT_FOR_TRAINING_CAPACITY &&
           response.actions_size() == 0 &&
           !response.task_stop_requested() &&
           response.retry_after_ms() > 0 && pending_sequence >= 1 &&
           reply.applied_sequence() == pending_sequence - 1 &&
           reply.task_state() == task_state &&
           reply.session_state() == session_state &&
           reply.episode_state() == episode_state &&
           reply.evaluation_state() == evaluation_state;
}

}  // namespace maze_client
