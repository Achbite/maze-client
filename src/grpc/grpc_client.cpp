#include "grpc/grpc_client.h"
#include "log/logger.h"

#include <chrono>
#include <thread>

namespace {

void SetRpcDeadline(grpc::ClientContext& context) {
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

void SetUpdateDeadline(grpc::ClientContext& context) {
    // A training Update can intentionally wait at a global sample boundary
    // while the Learner publishes the next contiguous model.
    context.set_deadline(
        std::chrono::system_clock::now() + std::chrono::seconds(90));
}

template <typename Response, typename Invoke>
bool InvokeIdempotent(const char* operation,
                      bool update_deadline,
                      Response& response,
                      bool& outcome_unknown,
                      Invoke invoke) {
    outcome_unknown = false;
    grpc::Status last_status;
    bool saw_non_ok_transport_status = false;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        grpc::ClientContext context;
        if (update_deadline) {
            SetUpdateDeadline(context);
        } else {
            SetRpcDeadline(context);
        }
        Response candidate;
        last_status = invoke(context, candidate);
        if (last_status.ok()) {
            response.Swap(&candidate);
            return true;
        }
        // A non-OK unary status never proves that the server did not commit
        // the command. Only the exact protobuf lifecycle reply can provide
        // that evidence, so every transport failure remains outcome-unknown.
        saw_non_ok_transport_status = true;
        const auto code = last_status.error_code();
        const bool retryable_transport_failure =
            code == grpc::StatusCode::ABORTED ||
            code == grpc::StatusCode::CANCELLED ||
            code == grpc::StatusCode::DEADLINE_EXCEEDED ||
            code == grpc::StatusCode::INTERNAL ||
            code == grpc::StatusCode::RESOURCE_EXHAUSTED ||
            code == grpc::StatusCode::UNKNOWN ||
            code == grpc::StatusCode::UNAVAILABLE;
        if (!retryable_transport_failure || attempt == 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    LOG_ERROR("GrpcClient", "%s RPC 失败: %s", operation,
              last_status.error_message().c_str());
    outcome_unknown = saw_non_ok_transport_status;
    return false;
}

}  // namespace

// ---- 创建 gRPC Channel 连接 AIServer ----
bool GrpcClient::Connect(const std::string& host, int port) {
    std::string target = host + ":" + std::to_string(port);

    // 创建不安全通道（内网通信，无需 TLS）
    channel_ = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    stub_ = maze::MazeTaskService::NewStub(channel_);

    // 等待通道就绪（最多 5 秒）
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
    bool ok = channel_->WaitForConnected(deadline);

    if (ok) {
        connected_ = true;
        LOG_INFO("GrpcClient", "已连接 %s", target.c_str());
    } else {
        LOG_ERROR("GrpcClient", "连接 %s 超时", target.c_str());
    }

    return ok;
}

// ---- 连接状态查询 ----
bool GrpcClient::IsConnected() const {
    return connected_;
}

bool GrpcClient::OpenSession(const maze::OpenSessionReq& req,
                             maze::OpenSessionRsp& rsp) {
    return InvokeIdempotent(
        "OpenSession", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context, maze::OpenSessionRsp& candidate) {
            return stub_->OpenSession(&context, req, &candidate);
        });
}

// ---- 0.10.0 canonical map 初始化与校验 RPC ----
bool GrpcClient::Init(const maze::InitReq& req, maze::InitRsp& rsp) {
    return InvokeIdempotent(
        "Init", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context, maze::InitRsp& candidate) {
            return stub_->Init(&context, req, &candidate);
        });
}

bool GrpcClient::BeginEpisode(const maze::BeginEpisodeReq& req,
                              maze::BeginEpisodeRsp& rsp) {
    return InvokeIdempotent(
        "BeginEpisode", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context,
            maze::BeginEpisodeRsp& candidate) {
            return stub_->BeginEpisode(&context, req, &candidate);
        });
}

// ---- 帧同步 RPC ----
bool GrpcClient::Update(const maze::UpdateReq& req, maze::UpdateRsp& rsp) {
    return InvokeIdempotent(
        "Update", true, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context, maze::UpdateRsp& candidate) {
            return stub_->Update(&context, req, &candidate);
        });
}

// ---- Episode 结束 RPC ----
bool GrpcClient::EndEpisode(const maze::EndEpisodeReq& req, maze::EndEpisodeRsp& rsp) {
    return InvokeIdempotent(
        "EndEpisode", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context, maze::EndEpisodeRsp& candidate) {
            return stub_->EndEpisode(&context, req, &candidate);
        });
}

bool GrpcClient::AbortEpisode(const maze::AbortEpisodeReq& req,
                              maze::AbortEpisodeRsp& rsp) {
    return InvokeIdempotent(
        "AbortEpisode", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context,
            maze::AbortEpisodeRsp& candidate) {
            return stub_->AbortEpisode(&context, req, &candidate);
        });
}

bool GrpcClient::CloseSession(const maze::CloseSessionReq& req,
                              maze::CloseSessionRsp& rsp) {
    if (!connected_ || !stub_) return false;
    return InvokeIdempotent(
        "CloseSession", false, rsp, last_rpc_outcome_unknown_,
        [&](grpc::ClientContext& context,
            maze::CloseSessionRsp& candidate) {
            return stub_->CloseSession(&context, req, &candidate);
        });
}

bool GrpcClient::LastRpcOutcomeUnknown() const {
    return last_rpc_outcome_unknown_;
}

// ---- 断开连接，释放 Channel 和 Stub ----
void GrpcClient::Disconnect() {
    if (!connected_) return;

    stub_.reset();
    channel_.reset();
    connected_ = false;
}
