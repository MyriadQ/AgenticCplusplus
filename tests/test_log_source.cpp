#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "agenticpp/log_source.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = fs::temp_directory_path() / ("agenticpp_log_source_" + std::move(name));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_, error);
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void append(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::app | std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<std::string> poll_lines(agenticpp::LogSource& source) {
    std::vector<std::string> lines;
    for (const agenticpp::LogRecord& record : source.poll()) {
        lines.emplace_back(record.line);
    }
    return lines;
}

}  // namespace

TEST_CASE("LogSource: from-start reads existing lines") {
    TempDir temp("from_start");
    const fs::path log = temp.path() / "app.log";
    append(log, "one\ntwo\n");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"one", "two"});
    REQUIRE(poll_lines(*source).empty());
    REQUIRE(source->stats().lines == 2);
}

TEST_CASE("LogSource: default starts at EOF and follows appends") {
    TempDir temp("tail");
    const fs::path log = temp.path() / "app.log";
    append(log, "old\n");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, false);
    REQUIRE(poll_lines(*source).empty());

    append(log, "new\n");
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"new"});
}

TEST_CASE("LogSource: partial line completes across writes") {
    TempDir temp("partial");
    const fs::path log = temp.path() / "app.log";
    append(log, "");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    append(log, "par");
    REQUIRE(poll_lines(*source).empty());
    append(log, "tial\n");
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"partial"});
}

TEST_CASE("LogSource: missing file is followed after creation") {
    TempDir temp("missing");
    const fs::path log = temp.path() / "app.log";
    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, false);
    REQUIRE(poll_lines(*source).empty());

    append(log, "created\n");
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"created"});
}

TEST_CASE("LogSource: truncation restarts from byte zero") {
    TempDir temp("truncate");
    const fs::path log = temp.path() / "app.log";
    append(log, "first\n");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"first"});

    std::error_code error;
    fs::resize_file(log, 0, error);
    REQUIRE_FALSE(error);
    // The source detects a size smaller than its offset and resets. Polling at
    // that point is what makes copytruncate observable before the file refills.
    REQUIRE(poll_lines(*source).empty());
    append(log, "second\n");
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"second"});
}

TEST_CASE("LogSource: replacement file is reopened") {
    TempDir temp("rotate");
    const fs::path log = temp.path() / "app.log";
    const fs::path old = temp.path() / "app.log.1";
    append(log, "old\n");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"old"});

    std::error_code error;
    fs::rename(log, old, error);
    REQUIRE_FALSE(error);
    append(log, "new\n");
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"new"});
}

TEST_CASE("LogSource: flush_eof emits the final partial line") {
    TempDir temp("eof");
    const fs::path log = temp.path() / "app.log";
    append(log, "tail");

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    REQUIRE(poll_lines(*source).empty());
    source->flush_eof();
    REQUIRE(poll_lines(*source) == std::vector<std::string>{"tail"});
    REQUIRE(poll_lines(*source).empty());
}

TEST_CASE("LogSource: many lines are emitted in one poll") {
    TempDir temp("many");
    const fs::path log = temp.path() / "app.log";
    for (int i = 0; i < 10000; ++i) {
        append(log, "line " + std::to_string(i) + "\n");
    }

    auto source = agenticpp::make_polling_source(log, std::chrono::milliseconds{10}, true);
    const auto lines = poll_lines(*source);
    REQUIRE(lines.size() == 10000);
    REQUIRE(lines.front() == "line 0");
    REQUIRE(lines.back() == "line 9999");
}
