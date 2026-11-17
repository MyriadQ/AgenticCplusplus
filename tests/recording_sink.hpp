#pragma once

#include <vector>

#include "agenticpp/types.hpp"

namespace agenticpp {

class RecordingSink final : public EventSink {
public:
    void emit(const Event& event) override { events.push_back(event); }

    std::vector<Event> events;
};

}  // namespace agenticpp
