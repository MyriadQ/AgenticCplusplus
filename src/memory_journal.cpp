#include "agenticpp/memory_journal.hpp"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "agenticpp/unique_fd.hpp"

namespace agenticpp {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kVersionLine = R"({"version":1})";

Result<void> write_all(int fd, std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count =
            ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return Result<void>::err(1, std::string{"journal write: "} + std::strerror(errno));
    }
    return Result<void>::ok();
}

std::int64_t to_ns(Timestamp at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

Timestamp from_ns(std::int64_t ns) {
    return Timestamp{std::chrono::nanoseconds{ns}};
}

Result<std::uint64_t> parse_id(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        return Result<std::uint64_t>::ok(value.get<std::uint64_t>());
    }
    if (value.is_number_integer()) {
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            return Result<std::uint64_t>::err(40, "negative template id");
        }
        return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(signed_value));
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        try {
            std::size_t consumed = 0;
            const unsigned long long parsed = std::stoull(text, &consumed, 0);
            if (consumed != text.size()) {
                return Result<std::uint64_t>::err(41, "invalid template id string");
            }
            return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(parsed));
        } catch (const std::exception&) {
            return Result<std::uint64_t>::err(42, "invalid template id string");
        }
    }
    return Result<std::uint64_t>::err(43, "missing template id");
}

Result<std::vector<std::string>> parse_argv(const nlohmann::json& value) {
    std::vector<std::string> argv;
    if (!value.is_array()) {
        return Result<std::vector<std::string>>::err(50, "argv must be an array");
    }
    for (const auto& item : value) {
        if (!item.is_string()) {
            return Result<std::vector<std::string>>::err(51, "argv entries must be strings");
        }
        argv.push_back(item.get<std::string>());
    }
    return Result<std::vector<std::string>>::ok(std::move(argv));
}

Result<MemoryJournalEvent> parse_event(const nlohmann::json& line) {
    if (!line.is_object()) {
        return Result<MemoryJournalEvent>::err(60, "journal line must be an object");
    }

    MemoryJournalEvent event;
    const std::string op = line.value("op", std::string{});
    if (op == "upsert") {
        event.kind = MemoryJournalEvent::Kind::Upsert;
    } else if (op == "outcome") {
        event.kind = MemoryJournalEvent::Kind::Outcome;
    } else {
        return Result<MemoryJournalEvent>::err(61, "unknown journal op: " + op);
    }

    if (!line.contains("template_id")) {
        return Result<MemoryJournalEvent>::err(62, "journal event has no template_id");
    }
    auto id = parse_id(line["template_id"]);
    if (!id.has_value()) {
        return Result<MemoryJournalEvent>::err(id.error());
    }
    event.template_id = id.value();
    event.successes = line.value("successes", 0u);
    event.failures = line.value("failures", 0u);
    event.last_seen = from_ns(line.value("last_seen_ns", std::int64_t{0}));

    if (event.kind == MemoryJournalEvent::Kind::Upsert) {
        event.template_text = line.value("template_text", std::string{});
        if (event.template_text.empty()) {
            return Result<MemoryJournalEvent>::err(63, "upsert event has no template_text");
        }
        event.purpose = line.value("purpose", std::string{});
        if (line.contains("argv")) {
            auto argv = parse_argv(line["argv"]);
            if (!argv.has_value()) {
                return Result<MemoryJournalEvent>::err(argv.error());
            }
            event.argv = std::move(argv.value());
        }
    }

    return Result<MemoryJournalEvent>::ok(std::move(event));
}

}  // namespace

MemoryJournal::MemoryJournal(fs::path path) : path_(std::move(path)) {}

Result<void> MemoryJournal::append_line(std::string line) {
    if (path_.empty()) {
        return Result<void>::ok();
    }

    std::error_code directory_error;
    if (!path_.parent_path().empty()) {
        fs::create_directories(path_.parent_path(), directory_error);
        if (directory_error) {
            return Result<void>::err(1, "cannot create journal directory: " +
                                            directory_error.message());
        }
    }

    UniqueFd fd{::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd) {
        return Result<void>::err(1, std::string{"cannot open journal: "} + std::strerror(errno));
    }

    std::error_code size_error;
    const bool empty = !fs::exists(path_, size_error) || fs::file_size(path_, size_error) == 0;
    if (size_error) {
        return Result<void>::err(1, "cannot stat journal: " + size_error.message());
    }

    std::string payload;
    if (empty) {
        payload.append(kVersionLine);
        payload.push_back('\n');
    }
    payload.append(std::move(line));
    payload.push_back('\n');

    auto written = write_all(fd.get(), payload);
    if (!written.has_value()) {
        return written;
    }
    if (::fsync(fd.get()) != 0) {
        return Result<void>::err(1, std::string{"journal fsync: "} + std::strerror(errno));
    }
    return Result<void>::ok();
}

Result<void> MemoryJournal::append_upsert(std::uint64_t template_id,
                                          std::string_view template_text,
                                          const std::vector<std::string>& argv,
                                          std::string_view purpose, std::uint32_t successes,
                                          std::uint32_t failures, Timestamp last_seen) {
    nlohmann::json line;
    line["op"] = "upsert";
    line["at_ns"] = to_ns(std::chrono::system_clock::now());
    line["template_id"] = template_id;
    line["template_text"] = template_text;
    line["argv"] = argv;
    line["purpose"] = purpose;
    line["successes"] = successes;
    line["failures"] = failures;
    line["last_seen_ns"] = to_ns(last_seen);
    return append_line(line.dump());
}

Result<void> MemoryJournal::append_outcome(std::uint64_t template_id, std::uint32_t successes,
                                           std::uint32_t failures, Timestamp last_seen) {
    nlohmann::json line;
    line["op"] = "outcome";
    line["at_ns"] = to_ns(std::chrono::system_clock::now());
    line["template_id"] = template_id;
    line["successes"] = successes;
    line["failures"] = failures;
    line["last_seen_ns"] = to_ns(last_seen);
    return append_line(line.dump());
}

Result<std::vector<MemoryJournalEvent>> MemoryJournal::load() const {
    std::vector<MemoryJournalEvent> events;
    if (path_.empty()) {
        return Result<std::vector<MemoryJournalEvent>>::ok(std::move(events));
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return Result<std::vector<MemoryJournalEvent>>::ok(std::move(events));
    }

    std::string content((std::istreambuf_iterator<char>(input)), {});
    if (input.bad()) {
        return Result<std::vector<MemoryJournalEvent>>::err(1, "cannot read journal");
    }
    if (content.empty()) {
        return Result<std::vector<MemoryJournalEvent>>::ok(std::move(events));
    }

    // A torn final line was not fsynced as a complete event. Ignore it.
    if (content.back() != '\n') {
        const std::size_t last_newline = content.rfind('\n');
        if (last_newline == std::string::npos) {
            return Result<std::vector<MemoryJournalEvent>>::err(2, "journal has no version line");
        }
        content.resize(last_newline + 1);
    }

    std::size_t position = 0;
    std::size_t line_number = 0;
    bool version_seen = false;
    while (position < content.size()) {
        const std::size_t end = content.find('\n', position);
        const std::size_t line_end = end == std::string::npos ? content.size() : end;
        const std::string_view text{content.data() + position, line_end - position};
        position = line_end + 1;
        ++line_number;

        if (text.empty()) {
            continue;
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(text);
        } catch (const std::exception&) {
            // Only a damaged trailing line is tolerated; a hole in the middle
            // would silently discard earlier knowledge.
            if (position >= content.size()) {
                break;
            }
            return Result<std::vector<MemoryJournalEvent>>::err(
                3, "journal parse error on line " + std::to_string(line_number));
        }

        if (!version_seen) {
            if (!parsed.is_object() || parsed.value("version", 0) != 1) {
                return Result<std::vector<MemoryJournalEvent>>::err(
                    4, "unsupported memory journal version");
            }
            version_seen = true;
            continue;
        }

        try {
            auto event = parse_event(parsed);
            if (!event.has_value()) {
                return Result<std::vector<MemoryJournalEvent>>::err(event.error());
            }
            events.push_back(std::move(event.value()));
        } catch (const std::exception& error) {
            if (position >= content.size()) {
                break;
            }
            return Result<std::vector<MemoryJournalEvent>>::err(
                6, std::string{"journal event error: "} + error.what());
        }
    }

    if (!version_seen && !content.empty()) {
        return Result<std::vector<MemoryJournalEvent>>::err(5, "journal has no version line");
    }
    return Result<std::vector<MemoryJournalEvent>>::ok(std::move(events));
}

Result<void> MemoryJournal::clear() {
    if (path_.empty()) {
        return Result<void>::ok();
    }

    std::error_code directory_error;
    if (!path_.parent_path().empty()) {
        fs::create_directories(path_.parent_path(), directory_error);
        if (directory_error) {
            return Result<void>::err(1, "cannot create journal directory: " +
                                            directory_error.message());
        }
    }

    UniqueFd fd{::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
    if (!fd) {
        return Result<void>::err(1, std::string{"cannot truncate journal: "} + std::strerror(errno));
    }

    std::string payload{kVersionLine};
    payload.push_back('\n');
    auto written = write_all(fd.get(), payload);
    if (!written.has_value()) {
        return written;
    }
    if (::fsync(fd.get()) != 0) {
        return Result<void>::err(1, std::string{"journal fsync: "} + std::strerror(errno));
    }
    return Result<void>::ok();
}

}  // namespace agenticpp
