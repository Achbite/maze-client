#pragma once

#include "proto/tasks/maze/task.pb.h"

namespace maze_client {

namespace task = rl::task::maze::v1;

inline bool AssignmentMatchesWorkload(
    task::WorkloadMode workload,
    const task::EpisodeAssignment& assignment) {
    switch (workload) {
        case task::WORKLOAD_MODE_TRAINING:
            return assignment.mode() == task::EPISODE_MODE_TRAINING;
        case task::WORKLOAD_MODE_EVALUATION:
            return assignment.mode() == task::EPISODE_MODE_EVALUATION;
        default:
            return false;
    }
}

}  // namespace maze_client
