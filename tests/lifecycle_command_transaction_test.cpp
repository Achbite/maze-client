#include "grpc/lifecycle_command_transaction.h"
#include "grpc/policy_binding_contract.h"
#include "grpc/workload_contract.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace maze = rl::task::maze::v1;
namespace common = rl::common::v1;

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void SetDigest(common::ContentDigest* digest, char fill) {
    digest->set_algorithm(common::DIGEST_ALGORITHM_SHA256);
    digest->set_hex(std::string(64, fill));
}

maze::BehaviorPolicyBinding CommonPolicy() {
    maze::BehaviorPolicyBinding policy;
    SetDigest(policy.mutable_model_artifact_digest(), 'a');
    policy.set_distribution_schema_id("categorical.logits.v1");
    SetDigest(policy.mutable_policy_spec_digest(), 'b');
    return policy;
}

void FillAppliedUpdate(maze::UpdateRsp* response,
                       std::uint64_t sequence,
                       maze::EpisodeState episode_state) {
    response->set_environment_control(
        maze::ENVIRONMENT_CONTROL_ADVANCE);
    auto* lifecycle = response->mutable_lifecycle();
    lifecycle->set_ret_code(0);
    lifecycle->set_result(maze::LIFECYCLE_RESULT_APPLIED);
    lifecycle->set_error_code(
        maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    lifecycle->set_applied_sequence(sequence);
    lifecycle->set_task_state(maze::TASK_STATE_TRAINING);
    lifecycle->set_session_state(
        maze::SESSION_STATE_EPISODE_ACTIVE);
    lifecycle->set_episode_state(episode_state);
}

maze::AgentState* AddAgent(maze::UpdateReq* request,
                           std::uint32_t agent_id,
                           bool done) {
    auto* state = request->add_agents();
    state->set_agent_id(agent_id);
    state->mutable_position()->set_x(0.0f);
    state->mutable_position()->set_y(0.0f);
    state->set_is_done(done);
    state->set_termination_reason(
        done ? maze::MAZE_TERMINATION_REASON_GOAL_REACHED
             : maze::MAZE_TERMINATION_REASON_ACTIVE);
    return state;
}

}  // namespace

int main() {
    maze::EpisodeAssignment training_assignment;
    training_assignment.set_mode(maze::EPISODE_MODE_TRAINING);
    training_assignment.set_collect_training_samples(true);
    Require(maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_TRAINING, training_assignment),
            "training assignment enters the sample-producing chain");

    maze::EpisodeAssignment evaluation_assignment;
    evaluation_assignment.set_mode(maze::EPISODE_MODE_EVALUATION);
    evaluation_assignment.set_collect_training_samples(false);
    Require(maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_EVALUATION,
                evaluation_assignment),
            "evaluation assignment enters the replay-only chain");

    auto training_policy = CommonPolicy();
    training_policy.set_model_lineage_id("local-lineage");
    training_policy.set_model_step(0);
    SetDigest(training_policy.mutable_model_manifest_digest(), 'c');
    Require(maze_client::PolicyBindingMatchesWorkload(
                maze::WORKLOAD_MODE_TRAINING, training_policy,
                "categorical.logits.v1"),
            "training binds an explicit distributed model");

    const auto evaluation_policy = CommonPolicy();
    Require(maze_client::PolicyBindingMatchesWorkload(
                maze::WORKLOAD_MODE_EVALUATION, evaluation_policy,
                "categorical.logits.v1"),
            "evaluation binds the selected model by digest");

    maze_client::LifecycleCursor cursor;
    cursor.session_id = "session-1";
    cursor.lifecycle_epoch = 7;
    cursor.next_sequence = 1;
    cursor.task_state = maze::TASK_STATE_CREATED;
    cursor.session_state = maze::SESSION_STATE_OPENED;
    cursor.episode_state = maze::EPISODE_STATE_UNSPECIFIED;

    maze::LifecycleCommand init;
    maze_client::FillCommand(cursor, "init", &init);
    Require(init.command_sequence() == 1,
            "Init uses the current session sequence");

    maze::LifecycleReply applied;
    applied.set_ret_code(0);
    applied.set_result(maze::LIFECYCLE_RESULT_APPLIED);
    applied.set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    applied.set_applied_sequence(1);
    applied.set_task_state(maze::TASK_STATE_TRAINING);
    applied.set_session_state(maze::SESSION_STATE_IDLE);
    applied.set_episode_state(maze::EPISODE_STATE_UNSPECIFIED);
    Require(maze_client::AcceptCommandReply(cursor, applied),
            "Init reply advances the session cursor");
    Require(cursor.next_sequence == 2 &&
                cursor.task_state == maze::TASK_STATE_TRAINING &&
                cursor.session_state == maze::SESSION_STATE_IDLE,
            "session cursor preserves the applied lifecycle state");

    cursor.next_sequence = 3;
    cursor.session_state = maze::SESSION_STATE_EPISODE_ACTIVE;
    cursor.episode_state = maze::EPISODE_STATE_RUNNING;
    std::vector<maze_client::AgentExecutionCursor> agents(2);

    maze::UpdateReq initial_request;
    initial_request.set_frame_id(0);
    auto* initial_agent_0 = AddAgent(&initial_request, 0, false);
    auto* initial_agent_1 = AddAgent(&initial_request, 1, false);
    Require(maze_client::AttachExecutedActionReceipt(
                0, agents[0], initial_agent_0) &&
                maze_client::AttachExecutedActionReceipt(
                    0, agents[1], initial_agent_1) &&
                !initial_agent_0->has_executed_action_id() &&
                !initial_agent_1->has_executed_action_id(),
            "frame zero reports every active Agent without an action receipt");

    maze::UpdateRsp initial_response;
    FillAppliedUpdate(&initial_response, 3,
                      maze::EPISODE_STATE_RUNNING);
    initial_response.add_actions()->set_agent_id(0);
    initial_response.mutable_actions(0)->set_action_id(0);
    initial_response.add_actions()->set_agent_id(1);
    initial_response.mutable_actions(1)->set_action_id(8);
    std::vector<maze_client::AgentExecutionCursor> candidate;
    Require(maze_client::PrepareAppliedAgentUpdate(
                initial_request, initial_response, agents, candidate) &&
                maze_client::AcceptCommandReply(
                    cursor, initial_response.lifecycle()),
            "initial applied Update prepares the complete active action set");
    agents = candidate;
    Require(maze_client::RecordExecutedAction(agents[0], 0) &&
                maze_client::RecordExecutedAction(agents[1], 8),
            "action zero and action eight are recorded as real executions");

    maze::UpdateReq terminal_request;
    terminal_request.set_frame_id(1);
    auto* terminal_agent = AddAgent(&terminal_request, 0, true);
    auto* active_agent = AddAgent(&terminal_request, 1, false);
    Require(maze_client::AttachExecutedActionReceipt(
                1, agents[0], terminal_agent) &&
                maze_client::AttachExecutedActionReceipt(
                    1, agents[1], active_agent) &&
                terminal_agent->has_executed_action_id() &&
                terminal_agent->executed_action_id() == 0 &&
                active_agent->executed_action_id() == 8,
            "later active and first-terminal reports carry exact receipts");
    const std::string retry_bytes = terminal_request.SerializeAsString();

    maze::UpdateRsp wait_response;
    wait_response.set_environment_control(
        maze::ENVIRONMENT_CONTROL_WAIT_FOR_TRAINING_CAPACITY);
    wait_response.set_retry_after_ms(1);
    auto* wait = wait_response.mutable_lifecycle();
    wait->set_ret_code(0);
    wait->set_result(maze::LIFECYCLE_RESULT_WAIT);
    wait->set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    wait->set_applied_sequence(3);
    wait->set_task_state(cursor.task_state);
    wait->set_session_state(cursor.session_state);
    wait->set_episode_state(cursor.episode_state);
    Require(!maze_client::PrepareAppliedAgentUpdate(
                terminal_request, wait_response, agents, candidate) &&
                !agents[0].retired && !agents[1].retired &&
                terminal_request.SerializeAsString() == retry_bytes,
            "WAIT keeps the exact request and does not retire terminal Agent");

    maze::UpdateRsp terminal_dummy;
    FillAppliedUpdate(&terminal_dummy, 4,
                      maze::EPISODE_STATE_RUNNING);
    terminal_dummy.add_actions()->set_agent_id(0);
    terminal_dummy.mutable_actions(0)->set_action_id(0);
    Require(!maze_client::PrepareAppliedAgentUpdate(
                terminal_request, terminal_dummy, agents, candidate),
            "terminal Agent cannot receive a dummy action");

    maze::UpdateRsp missing_active;
    FillAppliedUpdate(&missing_active, 4,
                      maze::EPISODE_STATE_RUNNING);
    Require(!maze_client::PrepareAppliedAgentUpdate(
                terminal_request, missing_active, agents, candidate),
            "an applied response cannot omit an active Agent action");

    maze::UpdateRsp duplicate_active;
    FillAppliedUpdate(&duplicate_active, 4,
                      maze::EPISODE_STATE_RUNNING);
    duplicate_active.add_actions()->set_agent_id(1);
    duplicate_active.mutable_actions(0)->set_action_id(3);
    duplicate_active.add_actions()->set_agent_id(1);
    duplicate_active.mutable_actions(1)->set_action_id(4);
    Require(!maze_client::PrepareAppliedAgentUpdate(
                terminal_request, duplicate_active, agents, candidate),
            "an applied response cannot duplicate an active Agent action");

    maze::UpdateRsp unknown_active;
    FillAppliedUpdate(&unknown_active, 4,
                      maze::EPISODE_STATE_RUNNING);
    unknown_active.add_actions()->set_agent_id(9);
    unknown_active.mutable_actions(0)->set_action_id(3);
    Require(!maze_client::PrepareAppliedAgentUpdate(
                terminal_request, unknown_active, agents, candidate),
            "an applied response cannot return an unknown Agent action");

    maze::UpdateRsp terminal_response;
    FillAppliedUpdate(&terminal_response, 4,
                      maze::EPISODE_STATE_RUNNING);
    terminal_response.add_actions()->set_agent_id(1);
    terminal_response.mutable_actions(0)->set_action_id(3);
    Require(maze_client::PrepareAppliedAgentUpdate(
                terminal_request, terminal_response, agents, candidate) &&
                !agents[0].retired &&
                maze_client::AcceptCommandReply(
                    cursor, terminal_response.lifecycle()),
            "terminal retirement is prepared before lifecycle commit");
    agents = candidate;
    Require(agents[0].retired && !agents[1].retired &&
                maze_client::ActiveAgentCount(agents) == 1 &&
                maze_client::RecordExecutedAction(agents[1], 3),
            "only a conclusively applied terminal Agent leaves the active set");

    maze::UpdateReq next_request;
    next_request.set_frame_id(2);
    auto* remaining = AddAgent(&next_request, 1, false);
    Require(maze_client::AttachExecutedActionReceipt(
                2, agents[1], remaining) &&
                next_request.agents_size() == 1 &&
                next_request.agents(0).agent_id() == 1 &&
                next_request.agents(0).executed_action_id() == 3,
            "the next request contains only the remaining active Agent");
    Require(terminal_request.SerializeAsString() == retry_bytes,
            "validation never mutates the request used for exact retry");
    return 0;
}
