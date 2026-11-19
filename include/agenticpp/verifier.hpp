#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "agenticpp/types.hpp"

namespace agenticpp {

struct VerifierConfig {
    std::string health_url = "http://127.0.0.1:8080/health";
    std::chrono::milliseconds health_timeout{5000};
    std::filesystem::path log_path;
    std::chrono::milliseconds log_quiet_window{3000};
};

class Verifier {
public:
    virtual ~Verifier() = default;

    // Called immediately before the authorized command runs. Verifiers that
    // inspect the log use this to record the pre-repair file offset.
    virtual void before_repair(std::uint64_t template_id) { (void)template_id; }

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

// Wraps another verifier and additionally checks that the incident template
// does not reappear in the log during the quiet window after the repair.
class LogQuietVerifier final : public Verifier {
public:
    LogQuietVerifier(std::unique_ptr<Verifier> inner, std::filesystem::path log_path,
                     std::chrono::milliseconds quiet_window);

    void before_repair(std::uint64_t template_id) override;
    Verdict verify(std::uint64_t template_id) override;

private:
    std::unique_ptr<Verifier> inner_;
    std::filesystem::path log_path_;
    std::chrono::milliseconds quiet_window_;

    std::uint64_t offset_ = 0;
    std::uint64_t inode_ = 0;
    Timestamp armed_at_{};
    bool armed_ = false;
};

}  // namespace agenticpp
