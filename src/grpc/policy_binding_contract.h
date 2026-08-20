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

inline bool IsAbsentDigest(const common::ContentDigest& digest) {
    return digest.ByteSizeLong() == 0;
}

inline bool PolicyBindingMatchesWorkload(
    task::WorkloadMode workload,
    const task::BehaviorPolicyBinding& policy,
    const std::string& expected_distribution_schema) {
    if (!IsSha256Digest(policy.model_artifact_digest()) ||
        policy.distribution_schema_id() != expected_distribution_schema ||
        !IsSha256Digest(policy.policy_spec_digest())) {
        return false;
    }

    switch (workload) {
        case task::WORKLOAD_MODE_TRAINING:
            return !policy.model_lineage_id().empty() &&
                   policy.has_model_step() &&
                   IsSha256Digest(policy.model_manifest_digest());
        case task::WORKLOAD_MODE_EVALUATION:
            return policy.model_lineage_id().empty() &&
                   !policy.has_model_step() &&
                   IsAbsentDigest(policy.model_manifest_digest());
        default:
            return false;
    }
}

}  // namespace maze_client
