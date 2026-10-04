#include "Pty.hpp"

#include "Environment.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

extern char** environ;

namespace ide {
namespace {

winsize Size(int cols, int rows) {
    winsize size{};
    size.ws_col = static_cast<unsigned short>(cols < 1 ? 1 : cols);
    size.ws_row = static_cast<unsigned short>(rows < 1 ? 1 : rows);
    return size;
}

int ExitCode(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

// This process's environment with the additions, as execve takes it.
std::vector<std::string> Environment(const std::vector<std::pair<std::string, std::string>>& additions) {
    std::vector<std::string> entries;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        entries.emplace_back(*entry);
    }
    for (const auto& [name, value] : additions) {
        const std::string prefix = name + "=";
        bool replaced = false;
        for (std::string& entry : entries) {
            if (entry.compare(0, prefix.size(), prefix) == 0) {
                entry = prefix + value;
                replaced = true;
            }
        }
        if (!replaced) {
            entries.push_back(prefix + value);
        }
    }
    return entries;
}

class PosixPty final : public Pty {
public:
    PosixPty(int master, pid_t child, int stop_read, int stop_write, PtyOptions options)
        : master_(master),
          child_(child),
          stop_read_(stop_read),
          stop_write_(stop_write),
          on_output_(std::move(options.on_output)),
          on_exit_(std::move(options.on_exit)) {
        reader_ = std::thread([this] { read(); });
    }

    ~PosixPty() override {
        closing_ = true;
        const char wake = 0;
        (void)::write(stop_write_, &wake, 1);
        reader_.join();
        // Closing the master hangs the terminal up, as when a terminal window
        // closes, and drops its unread output. It comes before the wait: macOS
        // holds an exiting shell until that output drains, and a job that
        // outlives the shell's hangup (claude, for one) keeps adding to it.
        close(master_);
        if (!reaped_) {
            kill(child_, SIGHUP);
            for (int tries = 0; tries < 50; ++tries) {
                if (waitpid(child_, nullptr, WNOHANG) != 0) {
                    reaped_ = true;
                    break;
                }
                usleep(10000);
            }
            if (!reaped_) {
                kill(child_, SIGKILL);
                waitpid(child_, nullptr, 0);
            }
        }
        close(stop_read_);
        close(stop_write_);
    }

    void write(std::string_view bytes) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        while (!bytes.empty()) {
            const ssize_t wrote = ::write(master_, bytes.data(), bytes.size());
            if (wrote < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                return;
            }
            bytes.remove_prefix(static_cast<std::size_t>(wrote));
        }
    }

    void resize(int cols, int rows) override {
        const winsize size = Size(cols, rows);
        ioctl(master_, TIOCSWINSZ, &size);
    }

private:
    void read() {
        char buffer[1 << 16];
        pollfd waits[2] = {{master_, POLLIN, 0}, {stop_read_, POLLIN, 0}};
        for (;;) {
            if (poll(waits, 2, -1) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (waits[1].revents != 0) {
                return;
            }
            // Linux reports EIO, not end of file, once the program and its children are gone.
            const ssize_t got = ::read(master_, buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            if (on_output_) {
                on_output_(std::string_view(buffer, static_cast<std::size_t>(got)));
            }
        }
        if (closing_) {
            return;
        }
        int status = 0;
        while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
        if (on_exit_) {
            on_exit_(ExitCode(status));
        }
    }

    int master_;
    pid_t child_;
    int stop_read_;
    int stop_write_;
    std::mutex write_mutex_;
    std::function<void(std::string_view)> on_output_;
    std::function<void(int)> on_exit_;
    std::atomic<bool> closing_{false};
    bool reaped_ = false;
    std::thread reader_;
};

}  // namespace

std::unique_ptr<Pty> Pty::spawn(PtyOptions options, std::string* error) {
    auto fail = [error](std::string text) -> std::unique_ptr<Pty> {
        if (error != nullptr) {
            *error = std::move(text);
        }
        return nullptr;
    };

    // Everything the child needs is built before the fork: between fork and
    // exec only async-signal-safe calls are allowed, since other threads' locks
    // may be held in the copy.
    std::vector<std::string> args;
    args.push_back(options.program);
    args.insert(args.end(), options.args.begin(), options.args.end());
    std::vector<char*> argv;
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    std::vector<std::string> env = Environment(options.env);
    std::vector<char*> envp;
    for (std::string& entry : env) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    const std::string cwd = options.cwd;
    const std::string program = options.program;

    int stop[2] = {-1, -1};
    if (pipe(stop) != 0) {
        return fail(std::string("Can't make the terminal's wake pipe: ") + std::strerror(errno));
    }
    fcntl(stop[0], F_SETFD, FD_CLOEXEC);
    fcntl(stop[1], F_SETFD, FD_CLOEXEC);

    winsize size = Size(options.cols, options.rows);
    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &size);
    if (child < 0) {
        const int code = errno;
        close(stop[0]);
        close(stop[1]);
        return fail(std::string("Can't make a pseudo-terminal: ") + std::strerror(code));
    }
    if (child == 0) {
        // The parent may ignore or block signals (SIGPIPE, for one) that a shell expects.
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (int signal : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU}) {
            ::signal(signal, SIG_DFL);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            _exit(126);
        }
        environ = envp.data();
        execvp(program.c_str(), argv.data());
        _exit(127);
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    return std::make_unique<PosixPty>(master, child, stop[0], stop[1], std::move(options));
}

PtyOptions DefaultShell() {
    PtyOptions options;
    std::optional<std::string> shell = engine_core::environment_variable("SHELL");
    if (!shell || shell->empty()) {
        if (const passwd* user = getpwuid(getuid()); user != nullptr && user->pw_shell != nullptr) {
            shell = std::string(user->pw_shell);
        }
    }
    options.program = shell && !shell->empty() ? *shell : "/bin/sh";
    // A login shell reads the profile that sets PATH, which an app opened from
    // the Finder or a desktop launcher does not inherit.
    options.args = {"-l"};
    options.env.emplace_back("TERM", "xterm-256color");
    options.env.emplace_back("COLORTERM", "truecolor");
    return options;
}

}  // namespace ide
