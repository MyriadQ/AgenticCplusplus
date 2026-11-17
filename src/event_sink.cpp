#include "agenticpp/event_sink.hpp"

#include <cctype>
#include <cstdio>
#include <string>

namespace agenticpp {

void ConsoleSink::emit(const Event& event) {
    std::string phase{event.phase};
    for (char& c : phase) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    std::printf("[%s] %s\n", phase.c_str(), event.detail.c_str());
}

}  // namespace agenticpp
