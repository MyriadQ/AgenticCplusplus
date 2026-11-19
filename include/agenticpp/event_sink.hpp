#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>

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

// Serializes access to a sink shared by worker threads.
class SynchronizedSink final : public EventSink {
public:
    explicit SynchronizedSink(EventSink& inner) : inner_(inner) {}

    void emit(const Event& event) override {
        std::lock_guard<std::mutex> lock(mutex_);
        inner_.emit(event);
    }

private:
    EventSink& inner_;
    std::mutex mutex_;
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
