#include "agenticpp/incident_queue.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace agenticpp {

IncidentQueue::IncidentQueue(std::size_t workers, std::size_t capacity, Handler handler)
    : handler_(std::move(handler)),
      capacity_(capacity == 0 ? 1 : capacity) {
    if (!handler_) {
        throw std::invalid_argument("IncidentQueue requires a handler");
    }
    const std::size_t count = workers == 0 ? 1 : workers;
    threads_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        threads_.emplace_back([this, i] { worker_loop(i); });
    }
}

IncidentQueue::~IncidentQueue() {
    stop();
}

bool IncidentQueue::submit(std::string line) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] { return stopping_ || queue_.size() < capacity_; });
    if (stopping_) {
        return false;
    }
    queue_.push_back(std::move(line));
    not_empty_.notify_one();
    return true;
}

void IncidentQueue::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}

void IncidentQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    threads_.clear();
}

std::size_t IncidentQueue::processed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return processed_;
}

std::size_t IncidentQueue::errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
}

void IncidentQueue::worker_loop(std::size_t worker) {
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            not_empty_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            line = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
            not_full_.notify_one();
        }

        try {
            handler_(worker, line);
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++errors_;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++errors_;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++processed_;
            --active_;
            if (queue_.empty() && active_ == 0) {
                idle_.notify_all();
            }
        }
    }
}

}  // namespace agenticpp
