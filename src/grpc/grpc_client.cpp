#include "grpc/grpc_client.h"
#include "log/logger.h"

bool GrpcClient::Connect(const std::string& host, int port) {
    const auto target = host + ":" + std::to_string(port);
    if (!transport_.Connect(target)) {
        LOG_ERROR("GrpcClient", "connection timed out: %s", target.c_str());
        return false;
    }
    stub_ = maze::MazeTaskService::NewStub(transport_.channel());
    return true;
}
bool GrpcClient::IsConnected() const { return transport_.connected(); }
void GrpcClient::Disconnect() { stub_.reset(); transport_.Disconnect(); }
bool GrpcClient::LastRpcOutcomeUnknown() const { return transport_.outcome_unknown(); }

bool GrpcClient::OpenSession(const maze::OpenSessionReq& request, maze::OpenSessionRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::OpenSessionRsp& candidate) {
            return stub_->OpenSession(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "OpenSession: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::Init(const maze::InitReq& request, maze::InitRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::InitRsp& candidate) {
            return stub_->Init(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "Init: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::BeginEpisode(const maze::BeginEpisodeReq& request, maze::BeginEpisodeRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::BeginEpisodeRsp& candidate) {
            return stub_->BeginEpisode(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "BeginEpisode: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::Update(const maze::UpdateReq& request, maze::UpdateRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(90),
        [&](grpc::ClientContext& context, maze::UpdateRsp& candidate) {
            return stub_->Update(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "Update: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::EndEpisode(const maze::EndEpisodeReq& request, maze::EndEpisodeRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::EndEpisodeRsp& candidate) {
            return stub_->EndEpisode(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "EndEpisode: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::AbortEpisode(const maze::AbortEpisodeReq& request, maze::AbortEpisodeRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::AbortEpisodeRsp& candidate) {
            return stub_->AbortEpisode(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "AbortEpisode: %s", transport_.error().c_str());
    return ok;
}

bool GrpcClient::CloseSession(const maze::CloseSessionReq& request, maze::CloseSessionRsp& response) {
    const bool ok = transport_.InvokeUnary(response, std::chrono::seconds(5),
        [&](grpc::ClientContext& context, maze::CloseSessionRsp& candidate) {
            return stub_->CloseSession(&context, request, &candidate);
        });
    if (!ok) LOG_ERROR("GrpcClient", "CloseSession: %s", transport_.error().c_str());
    return ok;
}
