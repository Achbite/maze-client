#pragma once

#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace maze_client {

inline bool IsAbortWait(
    const rl_sdk::LifecycleCursor& cursor,
    const task::AbortEpisodeRsp& response) {
    return response.has_wait() &&
           response.wait().retry_after_ms() > 0 &&
           rl_sdk::IsCommandWait(cursor, response.reply());
}

// A valid WAIT proves that this AbortEpisode request was not applied. Retry
// the same immutable request within the Client-owned wait budget. A transport
// failure or malformed reply remains outcome-unknown; budget exhaustion after
// a valid WAIT is a known not-applied outcome.
inline bool ApplyAbortEpisode(
    GrpcClient& client, rl_sdk::Session& session,
    const task::AbortEpisodeReq& request, task::AbortEpisodeRsp& response,
    std::chrono::milliseconds wait_budget, bool& outcome_unknown) {
    const auto result = session.Exchange(request, response,
        [&](const auto& req, auto& rsp) { return client.AbortEpisode(req, rsp); },
        [](const auto&, const auto&) { return true; },
        [&](const auto& rsp) -> int64_t {
            if (!rsp.has_wait()) return 0;
            return IsAbortWait(session.cursor(), rsp) ? rsp.wait().retry_after_ms() : -1;
        }, [] { return false; }, wait_budget);
    outcome_unknown = result == rl_sdk::CommandOutcome::Unknown;
    return result == rl_sdk::CommandOutcome::Applied;
}
}  // namespace maze_client
