#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agenticpp {

// Bounded incident queue with a fixed pool of blocking workers. Each worker
// runs one incident at a time; the handler receives its worker index so the
// caller can hand it a private Agent/LlmClient.
class IncidentQueue {
public:
    using Handler = std::function<void(std::size_t worker, const std::string& line)>;

    IncidentQueue(std::size_t workers, std::size_t capacity, Handler handler);
    ~IncidentQueue();

    IncidentQueue(const IncidentQueue&) = delete;
    IncidentQueue& operator=(const IncidentQueue&) = delete;

    // Blocks while the queue is full. Returns false after stop().
    bool submit(std::string line);

    // Waits until every submitted line has been handled.
    void wait_idle();

    // Stops accepting work and joins the workers. Call wait_idle() first if
    // pending lines must be processed; otherwise stop drains what is queued.
    void stop();

    [[nodiscard]] std::size_t processed() const;
    [[nodiscard]] std::size_t errors() const;

private:
    void worker_loop(std::size_t worker);

    Handler handler_;
    std::size_t capacity_;

    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::condition_variable idle_;
    std::deque<std::string> queue_;
    std::vector<std::thread> threads_;
    std::size_t active_ = 0;
    std::size_t processed_ = 0;
    std::size_t errors_ = 0;
    bool stopping_ = false;
};

}  // namespace agenticpp
