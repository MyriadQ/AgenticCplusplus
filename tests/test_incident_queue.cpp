#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agenticpp/incident_queue.hpp"

TEST_CASE("IncidentQueue: processes every submitted line") {
    std::mutex mutex;
    std::size_t handled = 0;
    std::set<std::size_t> workers;

    {
        agenticpp::IncidentQueue queue(3, 4, [&](std::size_t worker, const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            std::lock_guard<std::mutex> lock(mutex);
            ++handled;
            workers.insert(worker);
        });

        for (int i = 0; i < 100; ++i) {
            REQUIRE(queue.submit("line " + std::to_string(i)));
        }
        queue.wait_idle();
        REQUIRE(queue.processed() == 100);
        REQUIRE(queue.errors() == 0);
        queue.stop();
    }

    REQUIRE(handled == 100);
    REQUIRE(workers.size() == 3);
}

TEST_CASE("IncidentQueue: bounded capacity applies backpressure without loss") {
    std::atomic<std::size_t> handled{0};
    agenticpp::IncidentQueue queue(1, 2, [&](std::size_t, const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        handled.fetch_add(1);
    });

    for (int i = 0; i < 40; ++i) {
        REQUIRE(queue.submit("line " + std::to_string(i)));
    }
    queue.wait_idle();
    queue.stop();

    REQUIRE(handled.load() == 40);
    REQUIRE(queue.processed() == 40);
}

TEST_CASE("IncidentQueue: multiple workers run concurrently") {
    std::mutex mutex;
    std::condition_variable rendezvous;
    std::size_t active = 0;
    bool overlapped = false;

    agenticpp::IncidentQueue queue(2, 2, [&](std::size_t, const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        ++active;
        if (active == 2) {
            overlapped = true;
            rendezvous.notify_all();
        }
        rendezvous.wait_for(lock, std::chrono::seconds{1}, [&] { return overlapped; });
        --active;
    });

    REQUIRE(queue.submit("first"));
    REQUIRE(queue.submit("second"));
    queue.wait_idle();
    queue.stop();

    REQUIRE(overlapped);
    REQUIRE(queue.processed() == 2);
}

TEST_CASE("IncidentQueue: handler exceptions are counted") {
    agenticpp::IncidentQueue queue(1, 2, [](std::size_t, const std::string& line) {
        if (line == "bad") {
            throw std::runtime_error("handler failed");
        }
    });

    REQUIRE(queue.submit("good"));
    REQUIRE(queue.submit("bad"));
    queue.wait_idle();
    queue.stop();

    REQUIRE(queue.processed() == 2);
    REQUIRE(queue.errors() == 1);
}

TEST_CASE("IncidentQueue: stop rejects new submissions") {
    agenticpp::IncidentQueue queue(1, 2, [](std::size_t, const std::string&) {});
    queue.stop();

    REQUIRE_FALSE(queue.submit("after stop"));
    REQUIRE(queue.processed() == 0);
}
