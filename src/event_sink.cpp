#include "agenticpp/event_sink.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include <nlohmann/json.hpp>

namespace agenticpp {

void ConsoleSink::emit(const Event& event) {
    std::string phase{event.phase};
    for (char& c : phase) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::printf("[%s] %s\n", phase.c_str(), event.detail.c_str());
}

JsonlSink::JsonlSink(std::filesystem::path path) {
    if (!path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
    }
    output_.open(path, std::ios::app);
}

void JsonlSink::emit(const Event& event) {
    if (!output_) {
        return;
    }
    nlohmann::json line;
    line["ts_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                        event.at.time_since_epoch())
                        .count();
    line["phase"] = event.phase;
    line["detail"] = event.detail;
    output_ << line.dump() << '\n';
    output_.flush();
}

}  // namespace agenticpp
