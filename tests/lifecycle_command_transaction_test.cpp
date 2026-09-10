#include "maze/config/config_loader.h"
#include "rl_sdk/task_client.h"
#include "proto/maze/maze.sdk.pb.h"
#include "maze/action/action_receipt.h"

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

class SessionMazeTaskService : public maze::MazeTaskService::Service {
public:
    grpc::Status OpenSession(grpc::ServerContext*, const maze::OpenSessionReq*, maze::OpenSessionRsp* response) override {
        response->set_session_id("session-test");
        response->set_session_epoch(1);
        response->mutable_aiserver()->set_component("rl-aiserver");
        response->mutable_aiserver()->set_instance_id("test-server");
        response->mutable_aiserver()->set_lifecycle_epoch(1);
        Reply(0, rl::session::v1::SESSION_PHASE_OPEN, response->mutable_reply());
        return grpc::Status::OK;
    }
    grpc::Status Init(grpc::ServerContext*, const maze::InitReq* req, maze::InitRsp* rsp) override {
        Reply(req->command().sequence(), rl::session::v1::SESSION_PHASE_READY, rsp->mutable_reply());
        return grpc::Status::OK;
    }
    grpc::Status BeginEpisode(grpc::ServerContext*, const maze::BeginEpisodeReq* req, maze::BeginEpisodeRsp* rsp) override {
        rsp->mutable_assignment()->set_episode_id("episode-test");
        Reply(req->command().sequence(), rl::session::v1::SESSION_PHASE_EPISODE_RUNNING, rsp->mutable_reply());
        return grpc::Status::OK;
    }
    grpc::Status EndEpisode(grpc::ServerContext*, const maze::EndEpisodeReq* req, maze::EndEpisodeRsp* rsp) override {
        Require(req->command().episode_id() == "episode-test", "SDK retains episode identity through EndEpisode");
        ended = true;
        Reply(req->command().sequence(), rl::session::v1::SESSION_PHASE_READY, rsp->mutable_reply());
        return grpc::Status::OK;
    }
    grpc::Status CloseSession(grpc::ServerContext*, const maze::CloseSessionReq* req, maze::CloseSessionRsp* rsp) override {
        Require(req->command().episode_id().empty(), "SDK clears episode identity before CloseSession");
        closed = true;
        Reply(req->command().sequence(), rl::session::v1::SESSION_PHASE_CLOSED, rsp->mutable_reply());
        return grpc::Status::OK;
    }
    bool ended = false, closed = false;
protected:
    static void Reply(uint64_t sequence, rl::session::v1::SessionPhase phase, rl::session::v1::CommandReply* reply) {
        reply->set_result(rl::session::v1::COMMAND_RESULT_APPLIED);
        reply->set_applied_sequence(sequence);
        reply->set_phase(phase);
    }
};

using Client = rl_sdk::TaskClient<rl::task::maze::v1::MazeTaskServiceProtocol>;
void Begin(Client& client, int port) {
    Require(client.Connect("127.0.0.1:" + std::to_string(port)), "SDK connects to the simulated AIServer");
    maze::OpenSessionRsp open;
    maze::InitRsp init;
    maze::BeginEpisodeRsp begin;
    Require(client.OpenSession(open) == rl_sdk::CommandOutcome::Applied &&
            client.Init(maze::InitReq{}, init) == rl_sdk::CommandOutcome::Applied &&
            client.BeginEpisode(begin) == rl_sdk::CommandOutcome::Applied && client.Active(),
            "SDK opens, initializes and begins the typed session");
}

class ActionMazeTaskService final : public SessionMazeTaskService {
public:
    grpc::Status Update(grpc::ServerContext*,
                        const maze::UpdateReq* request,
                        maze::UpdateRsp* response) override {
        request_ = *request;
        frames.push_back(request->frame_id());
        auto* reply = response->mutable_reply();
        reply->set_result(rl::session::v1::COMMAND_RESULT_APPLIED);
        reply->set_error_code(rl::session::v1::COMMAND_ERROR_CODE_UNSPECIFIED);
        reply->set_applied_sequence(request->command().sequence());
        reply->set_phase(request->agents_size() > 0 && request->agents(0).is_done()
            ? rl::session::v1::SESSION_PHASE_EPISODE_TERMINAL : rl::session::v1::SESSION_PHASE_EPISODE_RUNNING);
        if (reply->phase() == rl::session::v1::SESSION_PHASE_EPISODE_TERMINAL) {
            response->mutable_action_batch();
            return grpc::Status::OK;
        }
        auto* action = response->mutable_action_batch()->add_actions();
        action->set_agent_id(0);
        action->set_action_id(maze::MAZE_ACTION_UP_RIGHT);
        return grpc::Status::OK;
    }

    const maze::UpdateReq& request() const { return request_; }
    std::vector<uint64_t> frames;

private:
    maze::UpdateReq request_;
};

class AbortWaitMazeTaskService final : public SessionMazeTaskService {
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

    Client client;
    Begin(client, port);

    ClientConfig environment_config;
    environment_config.run.agent_num = 1;
    environment_config.env.map_file = map_path;
    environment_config.env.max_steps = 1504;
    MazeEnv environment;
    Require(environment.Init(environment_config),
            "initialize the production MazeEnv");

    std::vector<maze_client::AgentExecutionCursor> agents(1);

    maze::UpdateReq request;
    request.set_frame_id(0);
    AddCurrentAgent(environment, &request);
    const std::string request_bytes = request.SerializeAsString();

    maze::UpdateRsp response;
    std::vector<maze_client::AgentExecutionCursor> candidate;
    const auto result = client.Update(request, response,
        [&](const auto& req, const auto& rsp) { return maze_client::PrepareAppliedAgentUpdate(req, rsp, agents, candidate); });
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
    receipt_request.set_frame_id(1);
    auto* receipt = AddCurrentAgent(environment, &receipt_request);
    Require(maze_client::AttachExecutedActionReceipt(
                1, agents[0], receipt) &&
                receipt->executed_action_id() ==
                    response.action_batch().actions(0).action_id(),
            "the next Client Update carries the executed action receipt");
    auto received = service.request();
    received.clear_command();
    Require(received.SerializeAsString() == request_bytes,
            "the simulated AIServer receives the Client Update unchanged");

    client.Disconnect();
    server->Shutdown();
}

void TestSdkReportsInitialAndFinalFacts() {
    ActionMazeTaskService service;
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    Require(server != nullptr, "start initial/final facts endpoint");
    Client client;
    Begin(client, port);
    int frame = 0;
    maze::UpdateRsp response;
    const auto outcome = rl_sdk::RunEpisode(
        [&] {
            maze::UpdateReq request;
            request.set_frame_id(frame);
            request.add_agents()->set_is_done(frame == 2);
            return request;
        },
        [&](auto request) { return client.Update(std::move(request), response); },
        [&] { return frame == 2; },
        [&] { ++frame; return true; },
        [] { return false; });
    Require(outcome == rl_sdk::CommandOutcome::Applied && service.frames == std::vector<uint64_t>({0, 1, 2}) && frame == 2,
            "SDK submits initial, action results and final facts without an extra final action");
    maze::EndEpisodeRsp end;
    maze::CloseSessionRsp close;
    Require(client.EndEpisode(end) == rl_sdk::CommandOutcome::Applied && client.CanClose() &&
            client.CloseSession(close) == rl_sdk::CommandOutcome::Applied && service.ended && service.closed,
            "SDK commits normal EndEpisode and CloseSession after the final fact");
    client.Disconnect();
    server->Shutdown();
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

    rl_sdk::ClientOptions options;
    options.abort_wait_budget = std::chrono::milliseconds(100);
    Client client(options);
    Begin(client, port);
    const auto initial_sequence = client.cursor().next_sequence;
    maze::AbortEpisodeReq request;
    request.set_reason(maze::MAZE_TERMINATION_REASON_CLIENT_ABORT);
    request.set_message("test abort");
    maze::AbortEpisodeRsp response;

    Require(client.AbortEpisode(request, response) == rl_sdk::CommandOutcome::Applied,
            "Client applies AbortEpisode after a valid WAIT with a confirmed outcome");
    Require(service.requests().size() == 2,
            "AIServer receives one WAIT attempt and one applied attempt");
    Require(service.requests()[0] == service.requests()[1],
            "Client retries the exact AbortEpisode request bytes");
    Require(client.cursor().next_sequence == initial_sequence + 1 &&
                client.cursor().phase == rl::session::v1::SESSION_PHASE_ABORTED,
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
