#include "config/config_loader.h"
#include "grpc/grpc_client.h"
#include "grpc/lifecycle_command_transaction.h"

#include <cstdlib>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace maze = rl::task::maze::v1;

namespace {

void Require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

class ActionMazeTaskService final : public maze::MazeTaskService::Service {
public:
    grpc::Status Update(grpc::ServerContext*,
                        const maze::UpdateReq* request,
                        maze::UpdateRsp* response) override {
        request_ = *request;
        auto* reply = response->mutable_reply();
        reply->set_result(maze::COMMAND_RESULT_APPLIED);
        reply->set_error_code(maze::COMMAND_ERROR_CODE_UNSPECIFIED);
        reply->set_applied_sequence(request->command().sequence());
        reply->set_phase(maze::SESSION_PHASE_EPISODE_RUNNING);
        auto* action = response->mutable_action_batch()->add_actions();
        action->set_agent_id(0);
        action->set_action_id(maze::MAZE_ACTION_UP_RIGHT);
        return grpc::Status::OK;
    }

    const maze::UpdateReq& request() const { return request_; }

private:
    maze::UpdateReq request_;
};

maze::AgentState* AddCurrentAgent(
    const MazeEnv& environment,
    maze::UpdateReq* request) {
    const AgentInfo& agent = environment.GetAgent(0);
    auto* state = request->add_agents();
    state->set_agent_id(0);
    state->mutable_position()->set_x(static_cast<float>(agent.grid_x));
    state->mutable_position()->set_y(static_cast<float>(agent.grid_y));
    state->set_is_done(false);
    state->set_termination_reason(
        maze::MAZE_TERMINATION_REASON_ACTIVE);
    return state;
}

void TestModelActionExecutionReceipt(const std::string& map_path) {
    ActionMazeTaskService service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(
        "127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    Require(server != nullptr && port > 0,
            "start the simulated AIServer action endpoint");

    GrpcClient client;
    Require(client.Connect("127.0.0.1", port),
            "Client connects to the simulated AIServer");

    ClientConfig environment_config;
    environment_config.run.agent_num = 1;
    environment_config.env.map_file = map_path;
    environment_config.env.max_steps = 1504;
    MazeEnv environment;
    Require(environment.Init(environment_config),
            "initialize the production MazeEnv");

    maze_client::LifecycleCursor lifecycle;
    lifecycle.session_id = "session-test";
    lifecycle.episode_id = "episode-test";
    lifecycle.session_epoch = 1;
    lifecycle.next_sequence = 1;
    lifecycle.phase = maze::SESSION_PHASE_EPISODE_RUNNING;
    std::vector<maze_client::AgentExecutionCursor> agents(1);

    maze::UpdateReq request;
    maze_client::FillCommand(lifecycle, request.mutable_command());
    request.set_frame_id(0);
    AddCurrentAgent(environment, &request);
    const std::string request_bytes = request.SerializeAsString();

    maze::UpdateRsp response;
    Require(client.Update(request, response),
            "Client receives the AIServer action response");
    std::vector<maze_client::AgentExecutionCursor> candidate;
    Require(maze_client::PrepareAppliedAgentUpdate(
                request, response, agents, candidate) &&
                maze_client::AcceptCommandReply(
                    lifecycle, response.reply()) &&
                response.action_batch().actions_size() == 1,
            "Client accepts the action for the active Agent");

    const AgentInfo before = environment.GetAgent(0);
    std::string error;
    Require(maze_client::ExecuteAssignedAction(
                environment, candidate[0],
                response.action_batch().actions(0), error),
            "execute the AIServer action in MazeEnv: " + error);
    const AgentInfo& after = environment.GetAgent(0);
    Require(before.grid_x != after.grid_x || before.grid_y != after.grid_y ||
                after.last_move_blocked,
            "MazeEnv records the action result");
    agents = candidate;

    maze::UpdateReq receipt_request;
    maze_client::FillCommand(lifecycle, receipt_request.mutable_command());
    receipt_request.set_frame_id(1);
    auto* receipt = AddCurrentAgent(environment, &receipt_request);
    Require(maze_client::AttachExecutedActionReceipt(
                1, agents[0], receipt) &&
                receipt->executed_action_id() ==
                    response.action_batch().actions(0).action_id(),
            "the next Client Update carries the executed action receipt");
    Require(service.request().SerializeAsString() == request_bytes,
            "the simulated AIServer receives the Client Update unchanged");

    client.Disconnect();
    server->Shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "usage: client_command_exchange_development_test MAP");
    TestModelActionExecutionReceipt(argv[1]);
    std::cout << "client_command_exchange_development_contract: PASS"
              << std::endl;
    return 0;
}
