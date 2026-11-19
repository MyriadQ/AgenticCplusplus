#include "agenticpp/executor.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agenticpp/unique_fd.hpp"

#if AGENTICPP_HAVE_SECCOMP
#include <seccomp.h>
#endif

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

namespace fs = std::filesystem;

bool write_proc_text(const char* path, std::string_view text) {
    UniqueFd fd{::open(path, O_WRONLY | O_CLOEXEC)};
    if (!fd) {
        return false;
    }
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(fd.get(), text.data() + written, text.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool setup_namespaces(bool isolate_network) {
    // Capture the host ids before unshare: after entering a fresh user
    // namespace the process is temporarily mapped to the overflow id.
    const uid_t uid = ::getuid();
    const gid_t gid = ::getgid();

    int flags = CLONE_NEWUSER | CLONE_NEWNS;
    if (isolate_network) {
        flags |= CLONE_NEWNET;
    }
    if (::unshare(flags) != 0) {
        std::fprintf(stderr, "sandbox: unshare: %s\n", std::strerror(errno));
        return false;
    }

    // setgroups may not exist on every kernel; failure here is non-fatal.
    (void)write_proc_text("/proc/thread-self/setgroups", "deny");
    if (!write_proc_text("/proc/thread-self/uid_map",
                         "0 " + std::to_string(static_cast<unsigned long>(uid)) + " 1\n")) {
        std::fprintf(stderr, "sandbox: uid_map: %s\n", std::strerror(errno));
        return false;
    }
    if (!write_proc_text("/proc/thread-self/gid_map",
                         "0 " + std::to_string(static_cast<unsigned long>(gid)) + " 1\n")) {
        std::fprintf(stderr, "sandbox: gid_map: %s\n", std::strerror(errno));
        return false;
    }
    if (::setresgid(0, 0, 0) != 0) {
        std::fprintf(stderr, "sandbox: setresgid: %s\n", std::strerror(errno));
        return false;
    }
    if (::setresuid(0, 0, 0) != 0) {
        std::fprintf(stderr, "sandbox: setresuid: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

bool bind_path(const fs::path& source, const fs::path& target, bool read_only) {
    std::error_code error;
    fs::create_directories(target, error);
    if (error) {
        return false;
    }
    if (::mount(source.c_str(), target.c_str(), nullptr, MS_BIND | MS_REC, nullptr) != 0) {
        return false;
    }
    if (read_only &&
        ::mount(nullptr, target.c_str(), nullptr, MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) !=
            0) {
        return false;
    }
    return true;
}

bool bind_if_present(const fs::path& source, const fs::path& target, bool read_only) {
    std::error_code error;
    if (!fs::exists(source, error)) {
        return true;
    }
    return bind_path(source, target, read_only);
}

bool prepare_sandbox(const ExecutorConfig& config, const fs::path& root) {
    if (config.sandbox_root.empty()) {
        return false;
    }
    if (!setup_namespaces(config.sandbox_network)) {
        return false;
    }
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        return false;
    }
    if (::mount("tmpfs", root.c_str(), "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=64m") !=
        0) {
        return false;
    }

    const fs::path sandbox_tmp = root / "tmp";
    std::error_code error;
    fs::create_directories(sandbox_tmp, error);
    if (error ||
        ::mount("tmpfs", sandbox_tmp.c_str(), "tmpfs", MS_NOSUID | MS_NODEV,
                "mode=1777,size=16m") != 0) {
        return false;
    }

    // Host runtime directories are read-only inside the sandbox. /dev stays
    // writable so scripts can use /dev/null; /tmp is a fresh tmpfs above.
    for (const char* name : {"/bin", "/sbin", "/lib", "/lib64", "/usr", "/etc", "/nix",
                             "/opt"}) {
        if (!bind_if_present(name, root / fs::path{name}.relative_path(), true)) {
            return false;
        }
    }
    if (!bind_if_present("/dev", root / "dev", false)) {
        return false;
    }

    const fs::path project = config.sandbox_root;
    for (const fs::path& writable : config.sandbox_writable_paths) {
        const fs::path source = writable.is_absolute() ? writable : project / writable;
        fs::create_directories(source, error);
        if (error) {
            return false;
        }
    }

    const fs::path project_target = root / project.relative_path();
    if (!bind_path(project, project_target, true)) {
        return false;
    }
    for (const fs::path& writable : config.sandbox_writable_paths) {
        const fs::path source = writable.is_absolute() ? writable : project / writable;
        const fs::path target =
            writable.is_absolute() ? root / writable.relative_path() : project_target / writable;
        if (!bind_path(source, target, false)) {
            return false;
        }
    }

    if (::chroot(root.c_str()) != 0 || ::chdir("/") != 0) {
        return false;
    }
    return true;
}

bool install_seccomp() {
#if AGENTICPP_HAVE_SECCOMP
    scmp_filter_ctx context = ::seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (context == nullptr) {
        return false;
    }

    const char* allowed[] = {
        "read", "write", "close", "lseek", "pread64", "pwrite64", "readv", "writev",
        "fstat", "stat", "lstat", "newfstatat", "statx", "access", "faccessat",
        "faccessat2", "openat", "openat2", "readlink", "readlinkat", "getdents64",
        "mmap", "mprotect", "munmap", "brk", "madvise", "ioctl", "fcntl", "dup",
        "dup2", "dup3", "pipe", "pipe2", "poll", "ppoll", "select", "pselect6",
        "epoll_create1", "epoll_ctl", "epoll_wait", "epoll_pwait", "eventfd2",
        "getpid", "getppid", "getuid", "geteuid", "getgid", "getegid", "gettid",
        "getcwd", "chdir", "fchdir", "execve", "execveat", "exit", "exit_group",
        "wait4", "waitid", "clone", "clone3", "fork", "vfork", "nanosleep",
        "clock_nanosleep", "gettimeofday", "clock_gettime", "uname", "sched_yield",
        "sched_getaffinity", "sched_setaffinity", "set_tid_address",
        "set_robust_list", "futex", "prlimit64", "getrandom", "rseq", "arch_prctl",
        "sigaltstack", "rt_sigaction", "rt_sigprocmask", "rt_sigreturn", "tgkill",
        "kill", "umask", "mkdir", "mkdirat", "unlink", "unlinkat", "rename",
        "renameat", "renameat2", "chmod", "fchmod", "fchmodat", "chown", "fchown",
        "fchownat", "truncate", "ftruncate", "fsync", "fdatasync", "utimensat",
        "memfd_create", "eventfd", "timerfd_create", "timerfd_settime", "prctl",
    };

    bool ok = true;
    for (const char* name : allowed) {
        const int number = ::seccomp_syscall_resolve_name(name);
        if (number < 0) {
            continue;
        }
        if (::seccomp_rule_add(context, SCMP_ACT_ALLOW, number, 0) != 0) {
            ok = false;
            break;
        }
    }
    if (ok && ::seccomp_load(context) != 0) {
        ok = false;
    }
    ::seccomp_release(context);
    return ok;
#else
    return false;
#endif
}

fs::path make_sandbox_root() {
    std::string pattern = (fs::temp_directory_path() / "agenticpp-sandbox-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        return {};
    }
    return fs::path{created};
}

void remove_sandbox_root(const fs::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code error;
    fs::remove_all(root, error);
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

    const fs::path sandbox_root = config_.sandbox_tier > 0 ? make_sandbox_root() : fs::path{};
    if (config_.sandbox_tier > 0 && sandbox_root.empty()) {
        result.exit_code = 127;
        result.err = "cannot create sandbox root";
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
        remove_sandbox_root(sandbox_root);
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
        remove_sandbox_root(sandbox_root);
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

        if (config_.sandbox_tier > 0 && !prepare_sandbox(config_, sandbox_root)) {
            std::fprintf(stderr, "sandbox: namespace setup failed\n");
            ::_exit(126);
        }
        if (config_.sandbox_tier >= 2 && !install_seccomp()) {
            std::fprintf(stderr, "sandbox: seccomp setup failed\n");
            ::_exit(126);
        }

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
    remove_sandbox_root(sandbox_root);
    return result;
}

}  // namespace agenticpp
