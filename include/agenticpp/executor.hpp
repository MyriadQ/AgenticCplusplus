#pragma once

#include <string>
#include <vector>

#include "agenticpp/types.hpp"

namespace agenticpp {

class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    virtual ExecResult run(const std::vector<std::string>& argv) = 0;
};

// Stage 1 double: records the intent, never forks.
class DryRunRunner final : public CommandRunner {
public:
    ExecResult run(const std::vector<std::string>& argv) override;
};

}  // namespace agenticpp
