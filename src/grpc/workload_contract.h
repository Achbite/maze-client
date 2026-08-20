#pragma once

#include "maze_task.pb.h"

namespace maze_client {

namespace task = rl::task::maze::v1;

inline bool AssignmentMatchesWorkload(
    task::WorkloadMode workload,
    const task::EpisodeAssignment& assignment) {
    switch (workload) {
        case task::WORKLOAD_MODE_TRAINING:
            return assignment.mode() == task::EPISODE_MODE_TRAINING &&
                   assignment.collect_training_samples();
        case task::WORKLOAD_MODE_EVALUATION:
            return assignment.mode() == task::EPISODE_MODE_EVALUATION &&
                   !assignment.collect_training_samples();
        default:
            return false;
    }
}

}  // namespace maze_client
