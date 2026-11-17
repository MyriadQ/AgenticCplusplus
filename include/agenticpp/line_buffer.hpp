#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace agenticpp {

// Turns arbitrary byte chunks into complete lines. Views returned by next_line()
// point into internal storage and are invalidated by the next append() or compact().
class LineBuffer {
public:
    static constexpr std::size_t kMaxLine = 64 * 1024;

    void append(std::span<const char> bytes);
    std::optional<std::string_view> next_line();
    std::size_t pending_bytes() const noexcept;
    std::size_t dropped_lines() const noexcept;
    void compact();
    void flush_eof();

private:
    std::optional<std::string_view> make_line(std::size_t start, std::size_t length);
    std::size_t find_newline() noexcept;

    std::vector<char> buf_;
    std::size_t scan_ = 0;
    std::size_t consume_ = 0;
    std::size_t dropped_ = 0;
    bool discarding_ = false;
    bool eof_ = false;
};

}  // namespace agenticpp
