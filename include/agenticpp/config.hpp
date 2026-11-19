#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "agenticpp/executor.hpp"
#include "agenticpp/llm_client.hpp"
#include "agenticpp/types.hpp"
#include "agenticpp/verifier.hpp"

namespace agenticpp {

struct LogSourceConfig {
    std::string path;
    int poll_interval_ms = 200;
    bool from_start = false;
    std::size_t max_line_bytes = 64 * 1024;
};

struct MemoryConfig {
    std::string path;
    std::string journal_path;
    int dim = 512;
    std::size_t top_k = 3;
    std::size_t compact_after_events = 1024;
    float recall_threshold = 0.86f;
};

struct AppConfig {
    std::string mode = "dry_run";
    bool require_confirmation = true;
    LogSourceConfig log_source;
    MemoryConfig memory;
    LlmConfig llm;
    AgentConfig agent;
    ExecutorConfig executor;
    VerifierConfig verifier;
};

Result<AppConfig> load_config(const std::filesystem::path& path);

}  // namespace agenticpp
