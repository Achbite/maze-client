#pragma once

#include "rl_sdk/task_client.h"
#include "proto/maze/maze.sdk.pb.h"
#include <functional>
#include <memory>

class MazeEnv;
struct ClientConfig;

// Maze environment fields and effects; RPC and Session state belong to the SDK.
class MazeClientAdapter {
public:
    using Client = rl_sdk::TaskClient<rl::task::maze::v1::MazeTaskServiceProtocol>;
    MazeClientAdapter(const ClientConfig& config, MazeEnv& environment,
                      Client& client, std::function<bool()> stopped);
    ~MazeClientAdapter();
    rl_sdk::CommandOutcome Run();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
