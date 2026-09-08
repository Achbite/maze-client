#include "config/config_loader.h"
#include "grpc/abort_episode_transaction.h"
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
        reply->set_result(rl::session::v1::COMMAND_RESULT_APPLIED);
        reply->set_error_code(rl::session::v1::COMMAND_ERROR_CODE_UNSPECIFIED);
        reply->set_applied_sequence(request->command().sequence());
        reply->set_phase(rl::session::v1::SESSION_PHASE_EPISODE_RUNNING);
        auto* action = response->mutable_action_batch()->add_actions();
        action->set_agent_id(0);
        action->set_action_id(maze::MAZE_ACTION_UP_RIGHT);
        return grpc::Status::OK;
    }

    const maze::UpdateReq& request() const { return request_; }

private:
    maze::UpdateReq request_;
};

class AbortWaitMazeTaskService final : public maze::MazeTaskService::Service {
public:
    grpc::Status AbortEpisode(
        grpc::ServerContext*,
        const maze::AbortEpisodeReq* request,
        maze::AbortEpisodeRsp* response) override {
        requests_.push_back(request->SerializeAsString());
        auto* reply = response->mutable_reply();
        reply->set_error_code(rl::session::v1::COMMAND_ERROR_CODE_UNSPECIFIED);
        if (requests_.size() == 1) {
            reply->set_result(rl::session::v1::COMMAND_RESULT_WAIT);
            reply->set_applied_sequence(request->command().sequence() - 1);
            reply->set_phase(rl::session::v1::SESSION_PHASE_EPISODE_RUNNING);
            response->mutable_wait()->set_retry_after_ms(1);
        } else {
            reply->set_result(rl::session::v1::COMMAND_RESULT_APPLIED);
            reply->set_applied_sequence(request->command().sequence());
            reply->set_phase(rl::session::v1::SESSION_PHASE_ABORTED);
        }
        return grpc::Status::OK;
    }

    const std::vector<std::string>& requests() const { return requests_; }

private:
    std::vector<std::string> requests_;
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

    rl_sdk::Session session;
    rl::session::v1::CommandReply opened;
    opened.set_phase(rl::session::v1::SESSION_PHASE_EPISODE_RUNNING);
    session.Bind("session-test", 1, opened);
    session.SetEpisode("episode-test");
    std::vector<maze_client::AgentExecutionCursor> agents(1);

    maze::UpdateReq request;
    session.Prepare(request);
    request.set_frame_id(0);
    AddCurrentAgent(environment, &request);
    const std::string request_bytes = request.SerializeAsString();

    maze::UpdateRsp response;
    std::vector<maze_client::AgentExecutionCursor> candidate;
    const auto result = session.Exchange(request, response,
        [&](const auto& req, auto& rsp) { return client.Update(req, rsp); },
        [&](const auto& req, const auto& rsp) { return maze_client::PrepareAppliedAgentUpdate(req, rsp, agents, candidate); },
        [](const auto&) { return 0; });
    Require(result == rl_sdk::CommandOutcome::Applied && response.action_batch().actions_size() == 1,
            "SDK accepts the typed action for the active Agent");

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
    session.Prepare(receipt_request);
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

void TestSdkReportsInitialAndFinalFacts() {
    int frame = 0;
    std::vector<int> observations;
    const auto outcome = rl_sdk::RunEpisode(
        [&] { return frame; },
        [&](int fact) { observations.push_back(fact); return rl_sdk::CommandOutcome::Applied; },
        [&] { return frame == 2; },
        [&] { ++frame; return true; },
        [] { return false; });
    Require(outcome == rl_sdk::CommandOutcome::Applied && observations == std::vector<int>({0, 1, 2}),
            "SDK submits initial, action results and final facts without an extra final action");
}

void TestAbortWaitRetriesExactRequest() {
    AbortWaitMazeTaskService service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(
        "127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    Require(server != nullptr && port > 0,
            "start the simulated AbortEpisode endpoint");

    GrpcClient client;
    Require(client.Connect("127.0.0.1", port),
            "Client connects to the simulated AbortEpisode endpoint");
    rl_sdk::Session session;
    rl::session::v1::CommandReply open_reply;
    open_reply.set_phase(rl::session::v1::SESSION_PHASE_EPISODE_RUNNING);
    open_reply.set_applied_sequence(6);
    session.Bind("session-abort-test", 1, open_reply);
    session.SetEpisode("episode-abort-test");
    const auto& lifecycle = session.cursor();
    maze::AbortEpisodeReq request;
    session.Prepare(request);
    request.set_reason(maze::MAZE_TERMINATION_REASON_CLIENT_ABORT);
    request.set_message("test abort");
    const std::string expected_request = request.SerializeAsString();
    maze::AbortEpisodeRsp response;
    bool outcome_unknown = true;

    Require(maze_client::ApplyAbortEpisode(
                client, session, request, response,
                std::chrono::milliseconds(100), outcome_unknown),
            "Client applies AbortEpisode after a valid WAIT");
    Require(!outcome_unknown,
            "valid WAIT retry leaves no unknown lifecycle outcome");
    Require(service.requests().size() == 2,
            "AIServer receives one WAIT attempt and one applied attempt");
    Require(service.requests()[0] == expected_request &&
                service.requests()[1] == expected_request,
            "Client retries the exact AbortEpisode request bytes");
    Require(lifecycle.next_sequence == 8 &&
                lifecycle.phase == rl::session::v1::SESSION_PHASE_ABORTED,
            "Client commits the lifecycle cursor only after APPLIED");

    client.Disconnect();
    server->Shutdown();
}

}  // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "usage: client_command_exchange_development_test MAP");
    TestModelActionExecutionReceipt(argv[1]);
    TestAbortWaitRetriesExactRequest();
    TestSdkReportsInitialAndFinalFacts();
    std::cout << "client_command_exchange_data_path: PASS"
              << std::endl;
    return 0;
}
