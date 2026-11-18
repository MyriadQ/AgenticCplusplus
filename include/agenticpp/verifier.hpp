#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "agenticpp/types.hpp"

namespace agenticpp {

struct VerifierConfig {
    std::string health_url = "http://127.0.0.1:8080/health";
    std::chrono::milliseconds health_timeout{5000};
};

class Verifier {
public:
    virtual ~Verifier() = default;
    virtual Verdict verify(std::uint64_t template_id) = 0;
};

// Test double. The real verifier below is used for execution demos.
class AlwaysOkVerifier final : public Verifier {
public:
    Verdict verify(std::uint64_t template_id) override;
};

class HealthProbeVerifier final : public Verifier {
public:
    explicit HealthProbeVerifier(VerifierConfig config);

    Verdict verify(std::uint64_t template_id) override;

private:
    VerifierConfig config_;
};

}  // namespace agenticpp
