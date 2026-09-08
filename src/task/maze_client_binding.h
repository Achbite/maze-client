#pragma once
#include "rl_sdk/session.h"
#include <memory>
class MazeEnv;
struct ClientConfig;

// Training-team binding. Environment teams implement MazeEnv and its proto facts;
// this class owns communication and uses the common SDK lifecycle.
class MazeClientBinding {
public:
    MazeClientBinding(const ClientConfig& config, MazeEnv& environment);
    ~MazeClientBinding();
    rl_sdk::CommandOutcome Run();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int RunMazeClient(int argc, char* argv[]);
