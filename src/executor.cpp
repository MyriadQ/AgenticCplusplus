#include "agenticpp/executor.hpp"

namespace agenticpp {

ExecResult DryRunRunner::run(const std::vector<std::string>& argv) {
    ExecResult result;
    result.exit_code = 0;
    result.out = "dry-run:";
    for (const std::string& arg : argv) {
        result.out += " ";
        result.out += arg;
    }
    return result;
}

}  // namespace agenticpp
