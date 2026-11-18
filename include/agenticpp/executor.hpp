#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "agenticpp/types.hpp"

namespace agenticpp {

struct ExecutorConfig {
    std::chrono::milliseconds timeout{30000};
    std::chrono::milliseconds grace{200};
    std::size_t output_cap = 64 * 1024;
    std::vector<std::string> env_allowlist{"PATH", "HOME", "LANG", "LC_ALL"};
    ExecLimits limits;
};

class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    virtual ExecResult run(const std::vector<std::string>& argv) = 0;
};

// Stage 1/2 test double: records the intent, never forks.
class DryRunRunner final : public CommandRunner {
public:
    ExecResult run(const std::vector<std::string>& argv) override;
};

// Owns a child process for one authorized argv vector. Blocking.
class ChildProcessRunner final : public CommandRunner {
public:
    explicit ChildProcessRunner(ExecutorConfig config);

    ExecResult run(const std::vector<std::string>& argv) override;

private:
    ExecutorConfig config_;
};

}  // namespace agenticpp
