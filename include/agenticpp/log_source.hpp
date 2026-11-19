#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace agenticpp {

struct LogRecord {
    std::string_view line; // valid until the next poll()
};

struct LogSourceStats {
    std::size_t lines = 0;
    std::size_t dropped = 0;
    std::uint64_t bytes = 0;
    std::uint64_t offset = 0;
    std::uint64_t inode = 0;
};

// A non-blocking line source. poll() returns the complete lines available now;
// wait() sleeps for at most the requested duration.
class LogSource {
public:
    virtual ~LogSource() = default;

    virtual std::span<const LogRecord> poll() = 0;
    virtual void wait(std::chrono::milliseconds max) = 0;
    virtual void flush_eof() = 0;
    [[nodiscard]] virtual LogSourceStats stats() const = 0;
};

// Polling tail of one file. It starts at EOF unless from_start is true, handles
// append, truncation, deletion and replacement, and owns a LineBuffer so a line
// split across reads is emitted only when its newline arrives.
std::unique_ptr<LogSource> make_polling_source(const std::filesystem::path& path,
                                               std::chrono::milliseconds interval,
                                               bool from_start);

}  // namespace agenticpp
