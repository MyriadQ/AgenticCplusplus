#pragma once

#include <cstdint>

#include "agenticpp/types.hpp"

namespace agenticpp {

class Verifier {
public:
    virtual ~Verifier() = default;
    virtual Verdict verify(std::uint64_t template_id) = 0;
};

// Stage 1 double: verification is deferred to Stage 2's health probe.
class AlwaysOkVerifier final : public Verifier {
public:
    Verdict verify(std::uint64_t template_id) override;
};

}  // namespace agenticpp
