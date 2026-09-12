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

    Require(client.Update(receipt_request, response,
        [&](const auto& req, const auto& rsp) {
            return maze_client::PrepareAppliedAgentUpdate(req, rsp, agents, candidate);
        }) == rl_sdk::CommandOutcome::Applied,
        "the next Update actually delivers and confirms the action execution receipt");
    Require(service.request().frame_id() == 1 && service.request().agents(0).has_executed_action_id() &&
            service.request().agents(0).executed_action_id() == receipt->executed_action_id(),
            "service received the real executed action, not just a local request object");

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

class UpdateWaitService final : public SessionMazeTaskService {
public:
    std::vector<std::string> requests;
    grpc::Status Update(grpc::ServerContext*, const maze::UpdateReq* req, maze::UpdateRsp* rsp) override {
        requests.push_back(req->SerializeAsString());
        if (requests.size() == 1) {
            Reply(req->command().sequence() - 1, rl::session::v1::SESSION_PHASE_EPISODE_RUNNING, rsp->mutable_reply());
            rsp->mutable_reply()->set_result(rl::session::v1::COMMAND_RESULT_WAIT);
            rsp->mutable_wait()->set_retry_after_ms(1);
        } else {
            Reply(req->command().sequence(), rl::session::v1::SESSION_PHASE_EPISODE_RUNNING, rsp->mutable_reply());
            auto* action = rsp->mutable_action_batch()->add_actions();
            action->set_agent_id(0);
            action->set_action_id(maze::MAZE_ACTION_RIGHT);
        }
        return grpc::Status::OK;
    }
};

void TestUpdateWaitPreservesFacts() {
    UpdateWaitService service;
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    Require(server && port, "start Update WAIT service");
    Client client;
    Begin(client, port);
    const auto sequence = client.cursor().next_sequence;
    maze::UpdateReq req;
    req.set_frame_id(7);
    req.add_agents()->set_executed_action_id(maze::MAZE_ACTION_LEFT);
    maze::UpdateRsp rsp;
    int applied_payloads = 0;
    Require(client.Update(req, rsp, [&](const auto&, const auto&) { ++applied_payloads; return true; }) ==
                rl_sdk::CommandOutcome::Applied && applied_payloads == 1,
            "WAIT produces no applied environment update");
    Require(service.requests.size() == 2 && service.requests.front() == service.requests.back() &&
                client.cursor().next_sequence == sequence + 1,
            "Update retries identical frame and receipt, commits sequence once");
    server->Shutdown();
    server->Wait();
}

class FailureService final : public SessionMazeTaskService {
public:
    bool transport_failure = false, abort_failure = false;
    int aborts = 0;
    grpc::Status Update(grpc::ServerContext*, const maze::UpdateReq* req, maze::UpdateRsp* rsp) override {
        if (transport_failure) return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "original-wire-cause");
        Reply(req->command().sequence() - 1, rl::session::v1::SESSION_PHASE_EPISODE_RUNNING, rsp->mutable_reply());
        rsp->mutable_reply()->set_result(rl::session::v1::COMMAND_RESULT_REJECTED);
        rsp->mutable_reply()->set_message("original-task-cause");
        rsp->mutable_reply()->set_error_code(rl::session::v1::COMMAND_ERROR_CODE_STATE_CONFLICT);
        return grpc::Status::OK;
    }
    grpc::Status AbortEpisode(grpc::ServerContext*, const maze::AbortEpisodeReq* req, maze::AbortEpisodeRsp* rsp) override {
        ++aborts;
        Reply(req->command().sequence() - (abort_failure ? 1 : 0),
            abort_failure ? rl::session::v1::SESSION_PHASE_EPISODE_RUNNING : rl::session::v1::SESSION_PHASE_ABORTED,
            rsp->mutable_reply());
        if (abort_failure) {
            rsp->mutable_reply()->set_result(rl::session::v1::COMMAND_RESULT_REJECTED);
            rsp->mutable_reply()->set_message("cleanup-cause");
            rsp->mutable_reply()->set_error_code(rl::session::v1::COMMAND_ERROR_CODE_STATE_CONFLICT);
        }
        return grpc::Status::OK;
    }
};

struct FailureBinding {
    Client& client;
    auto Open() { maze::OpenSessionRsp rsp; return client.OpenSession(rsp); }
    auto Initialize() { maze::InitRsp rsp; return client.Init({}, rsp); }
    auto Begin() { maze::BeginEpisodeRsp rsp; return client.BeginEpisode(rsp); }
    auto RunEpisode() { maze::UpdateRsp rsp; return client.Update({}, rsp); }
    auto End() { maze::EndEpisodeRsp rsp; return client.EndEpisode(rsp); }
    auto Abort() { maze::AbortEpisodeRsp rsp; return client.AbortEpisode({}, rsp); }
    auto Close() { maze::CloseSessionRsp rsp; return client.CloseSession(rsp); }
    bool Active() const { return client.Active(); }
    bool CanClose() const { return client.CanClose(); }
    bool Complete() const { return client.Complete(); }
    bool Stopped() const { return false; }
};

void TestErrorSurvivesSessionCleanup() {
    for (int scenario = 0; scenario < 3; ++scenario) {
        FailureService service;
        service.abort_failure = scenario == 1;
        service.transport_failure = scenario == 2;
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        auto server = builder.BuildAndStart();
        Require(server && port, "start error propagation service");
        Client client;
        Require(client.Connect("127.0.0.1:" + std::to_string(port)), "connect error propagation SDK");
        FailureBinding binding{client};
        const auto result = rl_sdk::RunSession(binding);
        if (service.transport_failure) {
            Require(result == rl_sdk::CommandOutcome::Unknown && service.aborts == 0 && !service.closed &&
                    client.error().find("original-wire-cause") != std::string::npos &&
                    client.error().find("3") != std::string::npos,
                    "Unknown preserves gRPC code and cause without substitute cleanup");
        } else {
            Require(result == rl_sdk::CommandOutcome::Rejected && service.aborts == 1 &&
                    client.error().find("original-task-cause") != std::string::npos,
                    "RunSession cleanup preserves Rejected, scenario=" + std::to_string(scenario) + ": " + client.error());
            Require(service.abort_failure ? client.error().find("cleanup-cause") != std::string::npos : service.closed,
                    "cleanup failure is appended; successful cleanup closes the session");
        }
        server->Shutdown();
        server->Wait();
    }
}

}  // namespace

int main(int argc, char** argv) {
    Require(argc == 2, "usage: client_command_exchange_development_test MAP");
    TestModelActionExecutionReceipt(argv[1]);
    TestAbortWaitRetriesExactRequest();
    TestSdkReportsInitialAndFinalFacts();
    TestUpdateWaitPreservesFacts();
    TestErrorSurvivesSessionCleanup();
    std::cout << "client_command_exchange_data_path: PASS"
              << std::endl;
    return 0;
}
