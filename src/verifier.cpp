#include "agenticpp/verifier.hpp"

namespace agenticpp {

Verdict AlwaysOkVerifier::verify(std::uint64_t) {
    Verdict verdict;
    verdict.ok = true;
    verdict.checks.push_back(Check{"always_ok", true, "stage 1 fake"});
    return verdict;
}

}  // namespace agenticpp
