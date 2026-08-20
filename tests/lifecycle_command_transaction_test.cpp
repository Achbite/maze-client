#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"

#include <cstdlib>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace maze = rl::task::maze::v1;
namespace common = rl::common::v1;

namespace {

void Require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void FillAppliedLifecycle(const maze::LifecycleCommand& command,
                          maze::LifecycleReply* reply) {
    reply->set_ret_code(0);
    reply->set_result(maze::LIFECYCLE_RESULT_APPLIED);
    reply->set_error_code(maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
    reply->set_applied_sequence(command.command_sequence());
    reply->set_task_state(maze::TASK_STATE_TRAINING);
    reply->set_session_state(maze::SESSION_STATE_EPISODE_ACTIVE);
    reply->set_episode_state(maze::EPISODE_STATE_RUNNING);
}

maze::AgentState* AddAgent(maze::UpdateReq* request,
                           uint32_t agent_id,
                           bool terminal) {
    auto* state = request->add_agents();
    state->set_agent_id(agent_id);
    state->mutable_position()->set_x(static_cast<float>(agent_id));
    state->mutable_position()->set_y(1.0f);
    state->set_is_done(terminal);
    state->set_termination_reason(
        terminal ? maze::MAZE_TERMINATION_REASON_GOAL_REACHED
                 : maze::MAZE_TERMINATION_REASON_ACTIVE);
    return state;
}

class FixedMazeTaskService final : public maze::MazeTaskService::Service {
public:
    grpc::Status OpenSession(grpc::ServerContext*,
                             const maze::OpenSessionReq* request,
                             maze::OpenSessionRsp* response) override {
        open_session_request_ = *request;
        auto* lifecycle = response->mutable_lifecycle();
        lifecycle->set_ret_code(0);
        lifecycle->set_result(maze::LIFECYCLE_RESULT_APPLIED);
        lifecycle->set_error_code(
            maze::LIFECYCLE_ERROR_CODE_UNSPECIFIED);
        lifecycle->set_applied_sequence(0);
        lifecycle->set_task_state(maze::TASK_STATE_CREATED);
        lifecycle->set_session_state(maze::SESSION_STATE_OPENED);
        lifecycle->set_episode_state(maze::EPISODE_STATE_UNSPECIFIED);
        response->set_session_protocol_version(3);
        response->set_session_id("session-fixed");
        response->set_lifecycle_epoch(1);
        response->mutable_environment_runtime()->set_agent_count(2);
        return grpc::Status::OK;
    }

    grpc::Status Update(grpc::ServerContext*,
                        const maze::UpdateReq* request,
                        maze::UpdateRsp* response) override {
        update_requests_.push_back(*request);
        FillAppliedLifecycle(request->command(), response->mutable_lifecycle());
        response->set_environment_control(
            maze::ENVIRONMENT_CONTROL_ADVANCE);
        if (request->frame_id() == 0) {
            auto* action0 = response->add_actions();
            action0->set_agent_id(0);
            action0->set_action_id(2);
            auto* action1 = response->add_actions();
            action1->set_agent_id(1);
            action1->set_action_id(8);
        } else {
            auto* action1 = response->add_actions();
            action1->set_agent_id(1);
            action1->set_action_id(3);
        }
        return grpc::Status::OK;
    }

    const maze::OpenSessionReq& open_session_request() const {
        return open_session_request_;
    }

    const std::vector<maze::UpdateReq>& update_requests() const {
        return update_requests_;
    }

private:
    maze::OpenSessionReq open_session_request_;
    std::vector<maze::UpdateReq> update_requests_;
};

void ApplyActions(const maze::UpdateRsp& response,
                  std::vector<maze_client::AgentExecutionCursor>* cursors) {
    for (const auto& action : response.actions()) {
        Require(action.agent_id() < cursors->size(),
                "action maps to a known Agent");
        Require(maze_client::RecordExecutedAction(
                    (*cursors)[action.agent_id()], action.action_id()),
                "record the action returned for its Agent");
    }
}

void TestCommandExchange() {
    FixedMazeTaskService service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(
        "127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    Require(server != nullptr && port > 0, "start local Client test stub");

    GrpcClient client;
    Require(client.Connect("127.0.0.1", port),
            "Client connects to the local test stub");

    maze::OpenSessionReq open_request;
    open_request.mutable_client()->set_component("maze-client");
    open_request.mutable_client()->set_instance_id("client-fixed");
    open_request.mutable_client()->set_lifecycle_epoch(1);
    open_request.set_environment_instance_id("environment-fixed");
    open_request.add_supported_session_protocol_versions(3);
    open_request.set_idempotency_key("open-fixed");
    const std::string expected_open_bytes = open_request.SerializeAsString();
    maze::OpenSessionRsp open_response;
    Require(client.OpenSession(open_request, open_response) &&
                open_response.session_id() == "session-fixed" &&
                open_response.environment_runtime().agent_count() == 2 &&
                service.open_session_request().SerializeAsString() ==
                    expected_open_bytes,
            "OpenSession sends the fixed request and receives the fixed spec");

    maze_client::LifecycleCursor lifecycle;
    lifecycle.session_id = open_response.session_id();
    lifecycle.episode_id = "episode-fixed";
    lifecycle.lifecycle_epoch = open_response.lifecycle_epoch();
    lifecycle.next_sequence = 1;
    lifecycle.task_state = maze::TASK_STATE_TRAINING;
    lifecycle.session_state = maze::SESSION_STATE_EPISODE_ACTIVE;
    lifecycle.episode_state = maze::EPISODE_STATE_RUNNING;
    std::vector<maze_client::AgentExecutionCursor> agents(2);

    maze::UpdateReq first_request;
    maze_client::FillCommand(lifecycle, "update", first_request.mutable_command());
    first_request.set_frame_id(0);
    AddAgent(&first_request, 0, false);
    AddAgent(&first_request, 1, false);
    const std::string expected_first_bytes = first_request.SerializeAsString();
    maze::UpdateRsp first_response;
    Require(client.Update(first_request, first_response),
            "Client sends the first fixed Update");
    std::vector<maze_client::AgentExecutionCursor> candidate;
    Require(maze_client::PrepareAppliedAgentUpdate(
                first_request, first_response, agents, candidate) &&
                maze_client::AcceptCommandReply(
                    lifecycle, first_response.lifecycle()),
            "Client maps the fixed action response to the active Agents");
    ApplyActions(first_response, &candidate);
    agents = candidate;

    maze::UpdateReq terminal_request;
    maze_client::FillCommand(
        lifecycle, "update", terminal_request.mutable_command());
    terminal_request.set_frame_id(1);
    auto* terminal_agent = AddAgent(&terminal_request, 0, true);
    auto* active_agent = AddAgent(&terminal_request, 1, false);
    Require(maze_client::AttachExecutedActionReceipt(
                1, agents[0], terminal_agent) &&
                maze_client::AttachExecutedActionReceipt(
                    1, agents[1], active_agent) &&
                terminal_agent->executed_action_id() == 2 &&
                active_agent->executed_action_id() == 8,
            "the next fixed AgentState carries the executed-action receipts");
    const std::string expected_terminal_bytes =
        terminal_request.SerializeAsString();
    maze::UpdateRsp terminal_response;
    Require(client.Update(terminal_request, terminal_response) &&
                terminal_response.actions_size() == 1 &&
                terminal_response.actions(0).agent_id() == 1 &&
                terminal_response.actions(0).action_id() == 3,
            "terminal Agent receives no action while the active Agent does");
    Require(maze_client::PrepareAppliedAgentUpdate(
                terminal_request, terminal_response, agents, candidate) &&
                maze_client::AcceptCommandReply(
                    lifecycle, terminal_response.lifecycle()),
            "the terminal Update applies through the production boundary");
    ApplyActions(terminal_response, &candidate);
    agents = candidate;

    maze::UpdateReq next_request;
    maze_client::FillCommand(lifecycle, "update", next_request.mutable_command());
    next_request.set_frame_id(2);
    auto* remaining = AddAgent(&next_request, 1, false);
    Require(maze_client::AttachExecutedActionReceipt(2, agents[1], remaining) &&
                agents[0].retired && !agents[1].retired &&
                next_request.agents_size() == 1 &&
                next_request.agents(0).executed_action_id() == 3,
            "the next request contains only the active Agent and its receipt");

    Require(service.update_requests().size() == 2 &&
                service.update_requests()[0].SerializeAsString() ==
                    expected_first_bytes &&
                service.update_requests()[1].SerializeAsString() ==
                    expected_terminal_bytes,
            "the local stub receives both fixed Update requests unchanged");

    client.Disconnect();
    server->Shutdown();
}

}  // namespace

int main() {
    TestCommandExchange();
    std::cout << "client_command_exchange_development_contract: PASS"
              << std::endl;
    return 0;
}
