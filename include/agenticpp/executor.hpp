#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
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

    // Tier 0: policy + no shell + rlimits (the default).
    // Tier 1: user/mount namespaces plus a chrooted tmpfs root.
    // Tier 2: Tier 1 plus a seccomp syscall allowlist.
    int sandbox_tier = 0;
    bool sandbox_network = false;
    std::filesystem::path sandbox_root;
    std::vector<std::filesystem::path> sandbox_writable_paths{"data", "run"};
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
