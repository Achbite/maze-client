#include "grpc/lifecycle_command_transaction.h"
#include "grpc/workload_contract.h"

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

maze::LifecycleReply ReplyFor(const maze_client::LifecycleCursor& cursor) {
    maze::LifecycleReply reply;
    reply.set_task_state(cursor.task_state);
    reply.set_session_state(cursor.session_state);
    reply.set_episode_state(cursor.episode_state);
    reply.set_evaluation_state(cursor.evaluation_state);
    return reply;
}

}  // namespace

int main() {
    maze::EpisodeAssignment training_assignment;
    training_assignment.set_mode(maze::EPISODE_MODE_TRAINING);
    training_assignment.set_collect_training_samples(true);
    Require(maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_TRAINING, training_assignment),
            "training workload accepts only sample-emitting training episodes");
    training_assignment.mutable_evaluation()->set_evaluation_id("unexpected");
    Require(!maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_TRAINING, training_assignment),
            "training workload rejects embedded evaluation assignments");

    maze::EpisodeAssignment evaluation_assignment;
    evaluation_assignment.set_mode(
        maze::EPISODE_MODE_EVALUATION_ARGMAX);
    evaluation_assignment.set_collect_training_samples(false);
    evaluation_assignment.mutable_evaluation()->set_evaluation_id("eval-1");
    Require(maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_MODEL_EVALUATION,
                evaluation_assignment),
            "local model evaluation accepts a pinned zero-sample episode");
    Require(!maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_TRAINING, evaluation_assignment),
            "training workload rejects Argmax evaluation episodes");
    Require(!maze_client::AssignmentMatchesWorkload(
                maze::WORKLOAD_MODE_MAP_VALIDATION,
                evaluation_assignment),
            "map validation never enters an episode loop");

    maze_client::LifecycleCursor init_cursor;
    init_cursor.session_id = "session-init";
    init_cursor.lifecycle_epoch = 3;
    init_cursor.next_sequence = 1;
    init_cursor.task_state = maze::TASK_STATE_CREATED;
    init_cursor.session_state = maze::SESSION_STATE_OPENED;
    init_cursor.episode_state = maze::EPISODE_STATE_UNSPECIFIED;
    init_cursor.evaluation_state = maze::EVALUATION_STATE_INACTIVE;
    auto init_applied = ReplyFor(init_cursor);
    init_applied.set_ret_code(0);
    init_applied.set_result(maze::LIFECYCLE_RESULT_APPLIED);
    init_applied.set_error_code(
        maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    init_applied.set_applied_sequence(1);
    init_applied.set_task_state(maze::TASK_STATE_TRAINING);
    init_applied.set_session_state(maze::SESSION_STATE_IDLE);
    Require(maze_client::AcceptCommandReply(init_cursor, init_applied),
            "Init may authoritatively report no active Episode");
    Require(init_cursor.next_sequence == 2 &&
                init_cursor.episode_state ==
                    maze::EPISODE_STATE_UNSPECIFIED,
            "a valid Init reply commits its cursor with Episode absent");

    maze_client::LifecycleCursor malformed_cursor;
    malformed_cursor.session_id = "session-malformed";
    malformed_cursor.lifecycle_epoch = 4;
    malformed_cursor.next_sequence = 2;
    malformed_cursor.task_state = maze::TASK_STATE_TRAINING;
    malformed_cursor.session_state = maze::SESSION_STATE_EPISODE_ACTIVE;
    malformed_cursor.episode_state = maze::EPISODE_STATE_RUNNING;
    malformed_cursor.evaluation_state = maze::EVALUATION_STATE_INACTIVE;
    auto missing_live_episode = ReplyFor(malformed_cursor);
    missing_live_episode.set_ret_code(0);
    missing_live_episode.set_result(maze::LIFECYCLE_RESULT_APPLIED);
    missing_live_episode.set_error_code(
        maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    missing_live_episode.set_applied_sequence(2);
    missing_live_episode.set_episode_state(
        maze::EPISODE_STATE_UNSPECIFIED);
    Require(!maze_client::AcceptCommandReply(
                malformed_cursor, missing_live_episode),
            "an active Session cannot omit its Episode state");
    Require(malformed_cursor.next_sequence == 2,
            "a malformed active reply must not consume the cursor");

    auto live_episode_without_active_session = missing_live_episode;
    live_episode_without_active_session.set_session_state(
        maze::SESSION_STATE_IDLE);
    live_episode_without_active_session.set_episode_state(
        maze::EPISODE_STATE_RUNNING);
    Require(!maze_client::AcceptCommandReply(
                malformed_cursor, live_episode_without_active_session),
            "a non-active Session cannot report a running Episode");
    Require(malformed_cursor.next_sequence == 2,
            "a malformed non-active reply must preserve the cursor");

    maze_client::LifecycleCursor cursor;
    cursor.session_id = "session-1";
    cursor.episode_id = "episode-1";
    cursor.evaluation_id = "evaluation-1";
    cursor.lifecycle_epoch = 7;
    cursor.next_sequence = 9;
    cursor.task_state = maze::TASK_STATE_EVALUATING;
    cursor.session_state = maze::SESSION_STATE_EPISODE_ACTIVE;
    cursor.episode_state = maze::EPISODE_STATE_RUNNING;
    cursor.evaluation_state = maze::EVALUATION_STATE_ARGMAX_ROUND_1;

    maze::LifecycleCommand update;
    maze_client::FillCommand(cursor, "update", &update);
    Require(update.command_sequence() == 9,
            "building a command must use the committed cursor sequence");
    Require(cursor.next_sequence == 9,
            "building a command must not consume the sequence");

    maze::LifecycleCommand exact_retry;
    maze_client::FillCommand(cursor, "update", &exact_retry);
    Require(update.SerializeAsString() == exact_retry.SerializeAsString(),
            "an unresolved operation must be byte-stable for exact retry");

    auto rejected = ReplyFor(cursor);
    rejected.set_ret_code(-1);
    rejected.set_result(maze::LIFECYCLE_RESULT_REJECTED);
    rejected.set_error_code(maze::LIFECYCLE_ERROR_CODE_STATE_CONFLICT);
    rejected.set_applied_sequence(8);
    Require(maze_client::IsConclusiveRejected(cursor, rejected),
            "a coherent rejection must prove the command was not committed");
    Require(!maze_client::AcceptCommandReply(cursor, rejected),
            "a rejected command must not advance the cursor");
    Require(cursor.next_sequence == 9,
            "a rejected command must leave its sequence reusable");

    auto wait = ReplyFor(cursor);
    wait.set_ret_code(0);
    wait.set_result(maze::LIFECYCLE_RESULT_WAIT);
    wait.set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    wait.set_applied_sequence(8);
    Require(maze_client::IsCommandWait(cursor, wait),
            "a coherent WAIT must prove the command was not committed");
    Require(!maze_client::AcceptCommandReply(cursor, wait),
            "WAIT must not advance the cursor");
    Require(cursor.next_sequence == 9,
            "WAIT must preserve the sequence for an identical retry");
    auto malformed_wait = wait;
    malformed_wait.set_episode_state(maze::EPISODE_STATE_ABORTED);
    Require(!maze_client::IsCommandWait(cursor, malformed_wait),
            "WAIT must echo the exact committed lifecycle state");

    auto contradictory_rejection = rejected;
    contradictory_rejection.set_ret_code(0);
    Require(!maze_client::IsConclusiveRejected(
                cursor, contradictory_rejection),
            "a positive ret_code cannot prove a rejection was uncommitted");
    contradictory_rejection = rejected;
    contradictory_rejection.set_error_code(
        maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    Require(!maze_client::IsConclusiveRejected(
                cursor, contradictory_rejection),
            "a rejection without a concrete error cannot release the sequence");

    maze::LifecycleCommand abort;
    maze_client::FillCommand(cursor, "abort-chain-failure", &abort);
    Require(abort.command_sequence() == 9,
            "cleanup after a rejected update must reuse the uncommitted sequence");
    Require(abort.idempotency_key() != update.idempotency_key(),
            "a replacement operation must have a distinct idempotency key");

    auto applied = ReplyFor(cursor);
    applied.set_ret_code(0);
    applied.set_result(maze::LIFECYCLE_RESULT_APPLIED);
    applied.set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    applied.set_applied_sequence(9);
    applied.set_session_state(maze::SESSION_STATE_IDLE);
    applied.set_episode_state(maze::EPISODE_STATE_ABORTED);
    applied.set_evaluation_state(maze::EVALUATION_STATE_INACTIVE);
    Require(maze_client::AcceptCommandReply(cursor, applied),
            "an exact applied reply must commit the cursor");
    Require(cursor.next_sequence == 10,
            "only an applied reply may advance the sequence");

    auto mismatched = ReplyFor(cursor);
    mismatched.set_ret_code(0);
    mismatched.set_result(maze::LIFECYCLE_RESULT_ALREADY_APPLIED);
    mismatched.set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    mismatched.set_applied_sequence(9);
    Require(!maze_client::AcceptCommandReply(cursor, mismatched),
            "a replay for an older sequence must not advance the cursor");
    Require(cursor.next_sequence == 10,
            "an invalid replay must preserve the committed cursor");
    return 0;
}
