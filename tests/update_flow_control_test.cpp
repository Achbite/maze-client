#include "grpc/update_flow_control.h"

#include <cstdlib>
#include <iostream>

namespace maze = rl::task::maze::v1;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

maze::UpdateRsp MakeWait() {
    maze::UpdateRsp response;
    response.set_environment_control(
        maze::ENVIRONMENT_CONTROL_WAIT_FOR_TRAINING_CAPACITY);
    response.set_retry_after_ms(100);
    auto* lifecycle = response.mutable_lifecycle();
    lifecycle->set_ret_code(0);
    lifecycle->set_result(maze::LIFECYCLE_RESULT_WAIT);
    lifecycle->set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    lifecycle->set_applied_sequence(8);
    lifecycle->set_task_state(maze::TASK_STATE_TRAINING);
    lifecycle->set_session_state(maze::SESSION_STATE_EPISODE_ACTIVE);
    lifecycle->set_episode_state(maze::EPISODE_STATE_RUNNING);
    lifecycle->set_evaluation_state(maze::EVALUATION_STATE_INACTIVE);
    return response;
}

bool Valid(const maze::UpdateRsp& response) {
    return maze_client::IsTrainingCapacityWait(
        response, 9, maze::TASK_STATE_TRAINING,
        maze::SESSION_STATE_EPISODE_ACTIVE, maze::EPISODE_STATE_RUNNING,
        maze::EVALUATION_STATE_INACTIVE);
}

}  // namespace

int main() {
    auto response = MakeWait();
    Require(Valid(response), "valid WAIT must retry the pending command");

    response.add_actions()->set_action_id(1);
    Require(!Valid(response), "WAIT must not carry an action");
    response = MakeWait();
    response.mutable_lifecycle()->set_applied_sequence(9);
    Require(!Valid(response), "WAIT must not consume the pending sequence");
    response = MakeWait();
    response.set_environment_control(maze::ENVIRONMENT_CONTROL_ADVANCE);
    Require(!Valid(response), "ADVANCE and WAIT are mutually exclusive");
    response = MakeWait();
    response.mutable_lifecycle()->set_episode_state(
        maze::EPISODE_STATE_TERMINAL_REPORTED);
    Require(!Valid(response), "WAIT must preserve lifecycle state");
    return 0;
}
