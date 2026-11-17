#include "agenticpp/line_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace agenticpp {

namespace {
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);
}  // namespace

void LineBuffer::append(std::span<const char> bytes) {
    if (bytes.empty()) {
        return;
    }
    eof_ = false;
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

std::size_t LineBuffer::find_newline() noexcept {
    if (scan_ >= buf_.size()) {
        return kNotFound;
    }

    const auto begin = buf_.begin() + static_cast<std::ptrdiff_t>(scan_);
    const auto it = std::find(begin, buf_.end(), '\n');
    if (it == buf_.end()) {
        scan_ = buf_.size();
        return kNotFound;
    }
    return static_cast<std::size_t>(it - buf_.begin());
}

std::optional<std::string_view> LineBuffer::make_line(std::size_t start, std::size_t length) {
    if (length == 0) {
        return std::string_view{};
    }

    std::string_view line{buf_.data() + start, length};
    if (line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::optional<std::string_view> LineBuffer::next_line() {
    for (;;) {
        if (discarding_) {
            const std::size_t newline = find_newline();
            if (newline == kNotFound) {
                return std::nullopt;
            }
            scan_ = newline + 1;
            consume_ = scan_;
            discarding_ = false;
            continue;
        }

        const std::size_t newline = find_newline();
        if (newline != kNotFound) {
            const std::size_t start = consume_;
            const std::size_t length = newline - start;
            scan_ = newline + 1;
            consume_ = scan_;

            if (length > kMaxLine) {
                ++dropped_;
                return make_line(start, kMaxLine);
            }
            return make_line(start, length);
        }

        if (!eof_) {
            const std::size_t available = buf_.size() - consume_;
            if (available > kMaxLine) {
                const std::size_t start = consume_;
                consume_ = start + kMaxLine;
                scan_ = consume_;
                discarding_ = true;
                ++dropped_;
                return make_line(start, kMaxLine);
            }
            return std::nullopt;
        }

        if (consume_ >= buf_.size()) {
            return std::nullopt;
        }

        const std::size_t start = consume_;
        std::size_t length = buf_.size() - start;
        scan_ = buf_.size();
        consume_ = buf_.size();
        if (length > kMaxLine) {
            length = kMaxLine;
            ++dropped_;
        }
        return make_line(start, length);
    }
}

std::size_t LineBuffer::pending_bytes() const noexcept {
    return buf_.size() - consume_;
}

std::size_t LineBuffer::dropped_lines() const noexcept {
    return dropped_;
}

void LineBuffer::compact() {
    if (consume_ == 0) {
        return;
    }

    const std::size_t remaining = buf_.size() - consume_;
    if (remaining != 0) {
        std::memmove(buf_.data(), buf_.data() + consume_, remaining);
    }
    buf_.resize(remaining);
    scan_ -= consume_;
    consume_ = 0;
}

void LineBuffer::flush_eof() {
    eof_ = true;
    if (discarding_) {
        consume_ = buf_.size();
        scan_ = buf_.size();
        discarding_ = false;
    }
}

}  // namespace agenticpp
