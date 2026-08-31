#pragma once

#include "maze_task.pb.h"

#include <string>

namespace maze_client {

namespace common = rl::common::v1;
namespace task = rl::task::maze::v1;

inline bool IsLowerSha256(const std::string& value) {
    if (value.size() != 64) return false;
    for (char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool IsSha256Digest(const common::ContentDigest& digest) {
    return digest.algorithm() == common::DIGEST_ALGORITHM_SHA256 &&
           IsLowerSha256(digest.hex());
}

inline bool AssignmentMatchesWorkload(
    task::WorkloadMode workload,
    const task::EpisodeAssignment& assignment) {
    switch (workload) {
        case task::WORKLOAD_MODE_TRAINING:
            return assignment.mode() == task::EPISODE_MODE_TRAINING &&
                   assignment.evaluation_model_artifact_digest()
                           .ByteSizeLong() == 0;
        case task::WORKLOAD_MODE_EVALUATION:
            return assignment.mode() == task::EPISODE_MODE_EVALUATION &&
                   IsSha256Digest(
                       assignment.evaluation_model_artifact_digest());
        default:
            return false;
    }
}

}  // namespace maze_client
