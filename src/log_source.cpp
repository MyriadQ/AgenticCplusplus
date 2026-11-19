#include "agenticpp/log_source.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenticpp/line_buffer.hpp"
#include "agenticpp/unique_fd.hpp"

namespace agenticpp {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kReadCap = 256 * 1024;

class PollingFileSource final : public LogSource {
public:
    PollingFileSource(fs::path path, std::chrono::milliseconds interval, bool from_start)
        : path_(std::move(path)),
          interval_(interval),
          from_start_(from_start) {
        struct stat status {};
        if (::stat(path_.c_str(), &status) == 0) {
            if (open_file()) {
                offset_ = from_start_ ? 0 : static_cast<std::uint64_t>(status.st_size);
                inode_ = static_cast<std::uint64_t>(status.st_ino);
            }
        }
    }

    std::span<const LogRecord> poll() override {
        records_.clear();
        lines_.clear();

        struct stat status {};
        if (::stat(path_.c_str(), &status) != 0) {
            fd_.reset();
            inode_ = 0;
            offset_ = 0;
            return {};
        }

        if (!fd_ || static_cast<std::uint64_t>(status.st_ino) != inode_) {
            if (!open_file()) {
                return {};
            }
            buffer_ = LineBuffer{};
            offset_ = 0;
            inode_ = static_cast<std::uint64_t>(status.st_ino);
        }

        if (static_cast<std::uint64_t>(status.st_size) < offset_) {
            // Truncate/copytruncate: restart from the beginning of the file.
            buffer_ = LineBuffer{};
            offset_ = 0;
        }

        std::size_t total = 0;
        while (total < kReadCap && offset_ < static_cast<std::uint64_t>(status.st_size)) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
                kReadCap - total, static_cast<std::uint64_t>(status.st_size) - offset_));
            char buffer[4096];
            const std::size_t take = std::min(want, sizeof(buffer));
            const ssize_t count = ::pread(fd_.get(), buffer, take, static_cast<off_t>(offset_));
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (count == 0) {
                break;
            }
            buffer_.append(std::span<const char>(buffer, static_cast<std::size_t>(count)));
            offset_ += static_cast<std::uint64_t>(count);
            total += static_cast<std::size_t>(count);
        }

        drain();
        stats_.bytes += total;
        stats_.dropped = buffer_.dropped_lines();
        stats_.offset = offset_;
        stats_.inode = inode_;
        return records_;
    }

    void wait(std::chrono::milliseconds max) override {
        if (max.count() <= 0) {
            return;
        }
        std::this_thread::sleep_for(std::min(max, interval_));
    }

    void flush_eof() override { buffer_.flush_eof(); }

    [[nodiscard]] LogSourceStats stats() const override { return stats_; }

private:
    bool open_file() {
        UniqueFd fd{::open(path_.c_str(), O_RDONLY | O_CLOEXEC)};
        if (!fd) {
            return false;
        }
        struct stat status {};
        if (::fstat(fd.get(), &status) != 0) {
            return false;
        }
        fd_ = std::move(fd);
        inode_ = static_cast<std::uint64_t>(status.st_ino);
        return true;
    }

    void drain() {
        while (const auto line = buffer_.next_line()) {
            lines_.emplace_back(*line);
            ++stats_.lines;
        }
        buffer_.compact();
        records_.reserve(lines_.size());
        for (const std::string& line : lines_) {
            records_.push_back(LogRecord{line});
        }
    }

    fs::path path_;
    std::chrono::milliseconds interval_;
    bool from_start_ = false;
    UniqueFd fd_;
    std::uint64_t inode_ = 0;
    std::uint64_t offset_ = 0;
    LineBuffer buffer_;
    std::vector<std::string> lines_;
    std::vector<LogRecord> records_;
    LogSourceStats stats_;
};

}  // namespace

std::unique_ptr<LogSource> make_polling_source(const fs::path& path,
                                               std::chrono::milliseconds interval,
                                               bool from_start) {
    return std::make_unique<PollingFileSource>(path, interval, from_start);
}

}  // namespace agenticpp
