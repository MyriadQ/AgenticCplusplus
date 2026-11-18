#pragma once

#include <filesystem>
#include <fstream>

#include "agenticpp/types.hpp"

namespace agenticpp {

class ConsoleSink final : public EventSink {
public:
    void emit(const Event& event) override;
};

class JsonlSink final : public EventSink {
public:
    explicit JsonlSink(std::filesystem::path path);

    void emit(const Event& event) override;

private:
    std::ofstream output_;
};

class TeeSink final : public EventSink {
public:
    TeeSink(EventSink& first, EventSink& second) : first_(first), second_(second) {}

    void emit(const Event& event) override {
        first_.emit(event);
        second_.emit(event);
    }

private:
    EventSink& first_;
    EventSink& second_;
};

}  // namespace agenticpp
