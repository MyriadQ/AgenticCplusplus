#include "agenticpp/executor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace agenticpp {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> build_environment(const std::vector<std::string>& allowlist) {
    std::vector<std::string> environment;
    environment.reserve(allowlist.size());
    for (const std::string& name : allowlist) {
        const char* value = std::getenv(name.c_str());
        if (value != nullptr) {
            environment.push_back(name + "=" + value);
        }
    }
    return environment;
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> result;
    result.reserve(strings.size() + 1);
    for (std::string& string : strings) {
        result.push_back(string.data());
    }
    result.push_back(nullptr);
    return result;
}

void set_limit(int resource, rlim_t value) {
    rlimit limit{value, value};
    if (::setrlimit(resource, &limit) != 0) {
        ::_exit(126);
    }
}

void append_output(std::string& destination, const char* data, std::size_t count,
                   std::size_t cap, bool& truncated) {
    if (destination.size() >= cap) {
        truncated = true;
        return;
    }
    const std::size_t remaining = cap - destination.size();
    const std::size_t take = std::min(count, remaining);
    destination.append(data, take);
    if (take < count) {
        truncated = true;
    }
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

int remaining_ms(Clock::time_point deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining.count() <= 0) {
        return 0;
    }
    constexpr auto kMaxPollMs = 1000;
    return static_cast<int>(std::min<std::int64_t>(remaining.count(), kMaxPollMs));
}

}  // namespace

ExecResult DryRunRunner::run(const std::vector<std::string>& argv) {
    ExecResult result;
    result.exit_code = 0;
    result.out = "dry-run:";
    for (const std::string& arg : argv) {
        result.out += " ";
        result.out += arg;
    }
    return result;
}

ChildProcessRunner::ChildProcessRunner(ExecutorConfig config) : config_(std::move(config)) {}

ExecResult ChildProcessRunner::run(const std::vector<std::string>& argv) {
    ExecResult result;
    if (argv.empty()) {
        result.exit_code = 127;
        result.err = "empty argv";
        return result;
    }

    std::vector<std::string> arguments = argv;
    std::vector<std::string> environment = build_environment(config_.env_allowlist);
    std::vector<char*> argv_ptrs = pointers(arguments);
    std::vector<char*> env_ptrs = pointers(environment);

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe2(out_pipe, O_CLOEXEC) != 0 || ::pipe2(err_pipe, O_CLOEXEC) != 0) {
        result.exit_code = 127;
        result.err = std::string{"pipe2: "} + std::strerror(errno);
        if (out_pipe[0] >= 0) {
            ::close(out_pipe[0]);
            ::close(out_pipe[1]);
        }
        return result;
    }

    const Clock::time_point started = Clock::now();
    const pid_t child = ::fork();
    if (child < 0) {
        result.exit_code = 127;
        result.err = std::string{"fork: "} + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return result;
    }

    if (child == 0) {
        ::setpgid(0, 0);
        if (::dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
            ::dup2(err_pipe[1], STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);

        set_limit(RLIMIT_CPU, static_cast<rlim_t>(config_.limits.cpu_seconds));
        set_limit(RLIMIT_AS, static_cast<rlim_t>(config_.limits.address_space));
        set_limit(RLIMIT_NPROC, static_cast<rlim_t>(config_.limits.nproc));
        set_limit(RLIMIT_FSIZE, static_cast<rlim_t>(config_.limits.fsize));

        ::execve(argv_ptrs[0], argv_ptrs.data(), env_ptrs.data());
        ::_exit(127);
    }

    (void)::setpgid(child, child);

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    out_pipe[1] = -1;
    err_pipe[1] = -1;
    set_nonblocking(out_pipe[0]);
    set_nonblocking(err_pipe[0]);

    bool out_open = true;
    bool err_open = true;
    bool timed_out = false;
    bool poll_failed = false;
    const Clock::time_point deadline = started + config_.timeout;

    while (out_open || err_open) {
        const int wait_ms = remaining_ms(deadline);
        if (wait_ms == 0) {
            timed_out = true;
            break;
        }

        pollfd fds[2] = {
            {out_pipe[0], static_cast<short>(out_open ? POLLIN : 0), 0},
            {err_pipe[0], static_cast<short>(err_open ? POLLIN : 0), 0},
        };

        const int ready = ::poll(fds, 2, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            poll_failed = true;
            break;
        }
        if (ready == 0) {
            if (Clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            continue;
        }

        if (out_open && (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buffer[4096];
            for (;;) {
                const ssize_t count = ::read(out_pipe[0], buffer, sizeof(buffer));
                if (count > 0) {
                    append_output(result.out, buffer, static_cast<std::size_t>(count),
                                  config_.output_cap, result.truncated);
                    continue;
                }
                if (count == 0) {
                    ::close(out_pipe[0]);
                    out_pipe[0] = -1;
                    out_open = false;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ::close(out_pipe[0]);
                    out_pipe[0] = -1;
                    out_open = false;
                }
                break;
            }
        }

        if (err_open && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buffer[4096];
            for (;;) {
                const ssize_t count = ::read(err_pipe[0], buffer, sizeof(buffer));
                if (count > 0) {
                    append_output(result.err, buffer, static_cast<std::size_t>(count),
                                  config_.output_cap, result.truncated);
                    continue;
                }
                if (count == 0) {
                    ::close(err_pipe[0]);
                    err_pipe[0] = -1;
                    err_open = false;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ::close(err_pipe[0]);
                    err_pipe[0] = -1;
                    err_open = false;
                }
                break;
            }
        }
    }

    if (out_pipe[0] >= 0) {
        ::close(out_pipe[0]);
    }
    if (err_pipe[0] >= 0) {
        ::close(err_pipe[0]);
    }

    int status = 0;
    if (timed_out || poll_failed) {
        (void)::kill(-child, SIGTERM);
        const Clock::time_point grace_end = Clock::now() + config_.grace;
        bool reaped = false;
        while (Clock::now() < grace_end) {
            const pid_t waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!reaped) {
            (void)::kill(-child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        }
    } else {
        while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
    }

    result.elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started);
    result.timed_out = timed_out;
    if (poll_failed) {
        result.err = "poll failed";
    }

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = -1;
        result.term_signal = WTERMSIG(status);
    }

    if (timed_out) {
        result.exit_code = -1;
    }
    return result;
}

}  // namespace agenticpp
