#pragma once

#include "agenticpp/types.hpp"

namespace agenticpp {

class ConsoleSink final : public EventSink {
public:
    void emit(const Event& event) override;
};

}  // namespace agenticpp
