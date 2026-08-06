#include "config/config_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream output;
    output << input.rdbuf();
    Require(static_cast<bool>(input) || input.eof(), "cannot read config");
    return output.str();
}

std::string ReplaceOnce(std::string value,
                        const std::string& from,
                        const std::string& to) {
    const auto position = value.find(from);
    Require(position != std::string::npos, "fixture token missing");
    value.replace(position, from.size(), to);
    return value;
}

bool Load(const std::filesystem::path& root,
          const std::string& name,
          const std::string& content) {
    const auto path = root / name;
    std::ofstream output(path, std::ios::trunc);
    output << content;
    output.close();
    ClientConfig config;
    return LoadClientConfig(path.string(), config);
}

}  // namespace

int main(int argc, char* argv[]) {
    Require(argc == 2, "config path is required");
    const std::string valid = Read(argv[1]);
    const auto root = std::filesystem::temp_directory_path() /
                      ("maze-client-config-test-" +
                       std::to_string(::getpid()));
    std::filesystem::create_directories(root);
    Require(Load(root, "valid.yaml", valid), "valid config was rejected");
    Require(!Load(root, "bad-port.yaml",
                  ReplaceOnce(valid, "  server_port: 9002",
                              "  server_port: 9002suffix")),
            "malformed port did not fail closed");
    ::setenv("RL_VIZ_INTERVAL", "1suffix", 1);
    Require(!Load(root, "bad-env.yaml", valid),
            "malformed environment override did not fail closed");
    ::unsetenv("RL_VIZ_INTERVAL");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return 0;
}
