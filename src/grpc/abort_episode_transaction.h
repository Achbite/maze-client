#pragma once

#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace maze_client {

inline bool IsAbortWait(
    const LifecycleCursor& cursor,
    const task::AbortEpisodeRsp& response) {
    return response.has_wait() &&
           response.wait().retry_after_ms() > 0 &&
           IsCommandWait(cursor, response.reply());
}

// A valid WAIT proves that this AbortEpisode request was not applied. Retry
// the same immutable request within the Client-owned wait budget. A transport
// failure or malformed reply remains outcome-unknown; budget exhaustion after
// a valid WAIT is a known not-applied outcome.
inline bool ApplyAbortEpisode(
    GrpcClient& client,
    LifecycleCursor& cursor,
    const task::AbortEpisodeReq& request,
    task::AbortEpisodeRsp& response,
    std::chrono::milliseconds wait_budget,
    bool& outcome_unknown) {
    outcome_unknown = false;
    if (wait_budget <= std::chrono::milliseconds::zero()) return false;
    const auto deadline = std::chrono::steady_clock::now() + wait_budget;
    while (std::chrono::steady_clock::now() < deadline) {
        response.Clear();
        if (!client.AbortEpisode(request, response)) {
            outcome_unknown = client.LastRpcOutcomeUnknown();
            return false;
        }
        if (!response.has_wait() &&
            AcceptCommandReply(cursor, response.reply())) {
            return true;
        }
        if (!IsAbortWait(cursor, response)) {
            outcome_unknown =
                !IsConclusiveRejected(cursor, response.reply());
            return false;
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) return false;
        std::this_thread::sleep_for(std::min(
            remaining,
            std::chrono::milliseconds(response.wait().retry_after_ms())));
    }
    return false;
}

}  // namespace maze_client
