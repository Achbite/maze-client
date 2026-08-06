#include "config/config_loader.h"
#include "env/maze_env.h"

#include <cstdlib>
#include <iostream>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Require(argc == 2, "fixed map path is required");
    ClientConfig config;
    config.run.agent_num = 4;
    config.env.map_file = argv[1];
    config.env.max_steps = 1504;

    MazeEnv environment;
    Require(environment.Init(config), "fixed v4 map failed to initialize");
    Require(environment.GetMapId() == "maze_117436372",
            "fixed map id mismatch");
    Require(environment.GetGridSizeMicrounits() == 495050000U,
            "grid_size_microunits lost decimal precision");
    Require(environment.GetShortestActionSteps() == 188,
            "fixed shortest path mismatch");
    Require(environment.GetMapChecksum() ==
                "861e0bb22a8b9a2ed689527d080c65ec2c822367e985c49753e1be9cf3ca8ae9",
            "fixed map checksum mismatch");
    return 0;
}
