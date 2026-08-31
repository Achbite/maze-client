#pragma once

#include "maze_task.pb.h"

#include <cstdint>

namespace maze_client {

namespace task = rl::task::maze::v1;

inline bool IsUpdateWait(
    const task::UpdateRsp& response,
    std::uint64_t pending_sequence,
    task::SessionPhase phase) {
    const auto& reply = response.reply();
    return reply.result() == task::COMMAND_RESULT_WAIT &&
           reply.error_code() == task::COMMAND_ERROR_CODE_UNSPECIFIED &&
           response.has_wait() && response.wait().retry_after_ms() > 0 &&
           pending_sequence >= 1 &&
           reply.applied_sequence() == pending_sequence - 1 &&
           reply.phase() == phase;
}

}  // namespace maze_client
