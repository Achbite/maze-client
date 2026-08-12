#pragma once

#include "maze_task.pb.h"

namespace maze_client {

namespace task = rl::task::maze::v1;

inline bool AssignmentMatchesWorkload(
    task::WorkloadMode workload,
    const task::EpisodeAssignment& assignment) {
    const bool has_evaluation = assignment.has_evaluation();
    switch (workload) {
        case task::WORKLOAD_MODE_TRAINING:
            return assignment.mode() == task::EPISODE_MODE_TRAINING &&
                   assignment.collect_training_samples() &&
                   !has_evaluation;
        case task::WORKLOAD_MODE_INFERENCE_SMOKE:
        case task::WORKLOAD_MODE_MODEL_EVALUATION:
            return (assignment.mode() ==
                        task::EPISODE_MODE_EVALUATION_ARGMAX ||
                    assignment.mode() ==
                        task::EPISODE_MODE_EVALUATION_STOCHASTIC) &&
                   !assignment.collect_training_samples() &&
                   has_evaluation &&
                   !assignment.evaluation()
                        .training_sample_emission_allowed();
        case task::WORKLOAD_MODE_MAP_VALIDATION:
        default:
            return false;
    }
}

}  // namespace maze_client
