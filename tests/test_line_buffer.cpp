#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "agenticpp/line_buffer.hpp"

namespace {

void append(agenticpp::LineBuffer& buffer, std::string_view text) {
    buffer.append(std::span<const char>(text.data(), text.size()));
}

std::vector<std::string> split_expected(const std::string& input) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\n') {
            lines.emplace_back(input.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < input.size()) {
        lines.emplace_back(input.substr(start));
    }
    return lines;
}

}  // namespace

TEST_CASE("LineBuffer: returns a complete line") {
    agenticpp::LineBuffer buffer;
    append(buffer, "hello\n");

    const auto line = buffer.next_line();

    REQUIRE(line.has_value());
    REQUIRE(*line == "hello");
    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.pending_bytes() == 0);
}

TEST_CASE("LineBuffer: splits a line across appends") {
    agenticpp::LineBuffer buffer;
    append(buffer, "hel");

    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.pending_bytes() == 3);

    append(buffer, "lo\n");
    const auto line = buffer.next_line();

    REQUIRE(line.has_value());
    REQUIRE(*line == "hello");
    REQUIRE(buffer.pending_bytes() == 0);
}

TEST_CASE("LineBuffer: strips CRLF") {
    agenticpp::LineBuffer buffer;
    append(buffer, "a\r\nb\r\n");

    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"a"});
    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"b"});
    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.dropped_lines() == 0);
}

TEST_CASE("LineBuffer: emits a blank line") {
    agenticpp::LineBuffer buffer;
    append(buffer, "a\n\nb\n");

    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"a"});
    REQUIRE(buffer.next_line() == std::optional<std::string_view>{""});
    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"b"});
    REQUIRE_FALSE(buffer.next_line().has_value());
}

TEST_CASE("LineBuffer: flush_eof emits a partial line") {
    agenticpp::LineBuffer buffer;
    append(buffer, "tail");

    REQUIRE_FALSE(buffer.next_line().has_value());
    buffer.flush_eof();

    const auto line = buffer.next_line();
    REQUIRE(line.has_value());
    REQUIRE(*line == "tail");
    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.pending_bytes() == 0);
}

TEST_CASE("LineBuffer: truncates an overlong line and counts it") {
    agenticpp::LineBuffer buffer;
    std::string data(100 * 1024, 'x');
    data.push_back('\n');
    append(buffer, data);

    const auto line = buffer.next_line();

    REQUIRE(line.has_value());
    REQUIRE(line->size() == agenticpp::LineBuffer::kMaxLine);
    REQUIRE(buffer.dropped_lines() == 1);
    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.pending_bytes() == 0);
}

TEST_CASE("LineBuffer: discards the remainder of an overlong line") {
    agenticpp::LineBuffer buffer;
    const std::string chunk(70 * 1024, 'x');
    append(buffer, chunk);

    const auto truncated = buffer.next_line();
    REQUIRE(truncated.has_value());
    REQUIRE(truncated->size() == agenticpp::LineBuffer::kMaxLine);
    REQUIRE(buffer.dropped_lines() == 1);

    append(buffer, "\n");
    REQUIRE_FALSE(buffer.next_line().has_value());
    REQUIRE(buffer.pending_bytes() == 0);

    append(buffer, "ok\n");
    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"ok"});
}

TEST_CASE("LineBuffer: preserves embedded NUL") {
    agenticpp::LineBuffer buffer;
    const std::string data{"a\0b\n", 4};
    append(buffer, data);

    const auto line = buffer.next_line();

    REQUIRE(line.has_value());
    REQUIRE(line->size() == 3);
    REQUIRE((*line)[0] == 'a');
    REQUIRE((*line)[1] == '\0');
    REQUIRE((*line)[2] == 'b');
    REQUIRE(*line == std::string_view{"a\0b", 3});
}

TEST_CASE("LineBuffer: handles 10k lines in one append") {
    agenticpp::LineBuffer buffer;
    std::string data;
    for (int i = 0; i < 10000; ++i) {
        data += "line " + std::to_string(i) + "\n";
    }
    append(buffer, data);

    std::size_t count = 0;
    while (const auto line = buffer.next_line()) {
        if (count == 0) {
            REQUIRE(*line == "line 0");
        }
        ++count;
    }

    REQUIRE(count == 10000);
    REQUIRE(buffer.dropped_lines() == 0);
}

TEST_CASE("LineBuffer: random chunks reassemble") {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> printable(32, 126);
    std::uniform_int_distribution<int> newline_chance(0, 9);
    std::uniform_int_distribution<int> chunk_size(1, 23);

    std::string input;
    input.reserve(6000);
    for (int i = 0; i < 6000; ++i) {
        if (newline_chance(rng) == 0) {
            input.push_back('\n');
        } else {
            input.push_back(static_cast<char>(printable(rng)));
        }
    }

    const std::vector<std::string> expected = split_expected(input);
    std::vector<std::string> actual;

    agenticpp::LineBuffer buffer;
    std::size_t offset = 0;
    while (offset < input.size()) {
        const std::size_t n =
            std::min<std::size_t>(input.size() - offset, static_cast<std::size_t>(chunk_size(rng)));
        append(buffer, std::string_view{input.data() + offset, n});
        offset += n;

        while (const auto line = buffer.next_line()) {
            actual.emplace_back(*line);
        }
        buffer.compact();
    }

    buffer.flush_eof();
    while (const auto line = buffer.next_line()) {
        actual.emplace_back(*line);
    }

    REQUIRE(actual == expected);
}

TEST_CASE("LineBuffer: compact releases consumed bytes") {
    agenticpp::LineBuffer buffer;
    append(buffer, "a\nb");

    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"a"});
    REQUIRE(buffer.pending_bytes() == 1);

    buffer.compact();
    REQUIRE(buffer.pending_bytes() == 1);

    append(buffer, "\n");
    REQUIRE(buffer.next_line() == std::optional<std::string_view>{"b"});
    REQUIRE(buffer.pending_bytes() == 0);

    buffer.compact();
    REQUIRE(buffer.pending_bytes() == 0);
}
