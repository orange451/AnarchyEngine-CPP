#include "RunProcess.hpp"

#include "IdeResources.hpp"

#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace ide {
namespace {

namespace fs = std::filesystem;

// arg as CommandLineToArgvW reads it back. force quotes it even when it has no
// space, as cmd.exe needs to keep & and ( inside one argument.
std::wstring QuoteArg(const std::wstring& arg, bool force) {
    if (!force && !arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            // Before the closing quote, each backslash is doubled so none escapes it.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

#if defined(_WIN32)

std::string Describe(DWORD code) {
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring message = length > 0 && text != nullptr ? std::wstring(text, length) : L"error " + std::to_wstring(code);
    if (text != nullptr) {
        LocalFree(text);
    }
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L'.')) {
        message.pop_back();
    }
    return utf8_path(fs::path(message));
}

bool IsBatch(const fs::path& program) {
    std::wstring extension = program.extension().wstring();
    for (wchar_t& c : extension) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return extension == L".cmd" || extension == L".bat";
}

// Closes a handle when it goes out of scope.
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset() {
        if (value != nullptr) {
            CloseHandle(value);
            value = nullptr;
        }
    }
};

ProcessResult Run(const fs::path& program, const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    ProcessResult result;
    std::wstring line;
    std::wstring application;
    if (IsBatch(program)) {
        // cmd.exe expands % and ! even inside quotes, and a quote of an
        // argument's own would end the one around it.
        const std::string program_text = utf8_path(program);
        if (program_text.find_first_of("%!\"^\r\n") != std::string::npos) {
            result.error = "cmd.exe cannot be given " + program_text;
            return result;
        }
        for (const std::string& arg : args) {
            if (arg.find_first_of("%!\"^\r\n") != std::string::npos) {
                result.error = "cmd.exe cannot be given the argument " + arg;
                return result;
            }
        }
        wchar_t system[MAX_PATH];
        const UINT length = GetSystemDirectoryW(system, MAX_PATH);
        application = (length > 0 && length < MAX_PATH ? std::wstring(system, length) : L"C:\\Windows\\System32") +
                      L"\\cmd.exe";
        // /s keeps the quotes inside the outer pair as they are.
        line = L"cmd.exe /d /s /c \"" + QuoteArg(program.wstring(), true);
        for (const std::string& arg : args) {
            line += L" " + QuoteArg(path_from_utf8(arg).wstring(), true);
        }
        line += L"\"";
    } else {
        application = program.wstring();
        line = windows_command_line(program, args);
    }

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &inherit, 0)) {
        result.error = "Could not make a pipe: " + Describe(GetLastError());
        return result;
    }
    Handle read_end(read_raw);
    Handle write_end(write_raw);
    SetHandleInformation(read_end.value, HANDLE_FLAG_INHERIT, 0);
    Handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));

    // Only these two handles go to the child, so a terminal the studio starts
    // meanwhile cannot hold the pipe open, and neither can the child's.
    HANDLE inherited[2] = {write_end.value, nul.value};
    const DWORD inherited_count = nul.value != nullptr ? 2 : 1;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   inherited_count * sizeof(HANDLE), nullptr, nullptr)) {
        result.error = "Could not set up the child's handles: " + Describe(GetLastError());
        return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul.value;
    startup.StartupInfo.hStdOutput = write_end.value;
    startup.StartupInfo.hStdError = write_end.value;
    startup.lpAttributeList = attributes;

    // Everything it starts is in the job, so ending the job ends them all.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job.value != nullptr) {
        SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    const BOOL created = CreateProcessW(
        application.c_str(), mutable_line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        nullptr, &startup.StartupInfo, &process);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    write_end.reset();
    nul.reset();
    if (!created) {
        result.error = "Could not start " + utf8_path(program) + ": " + Describe(create_error);
        return result;
    }
    result.started = true;
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    if (job.value != nullptr) {
        AssignProcessToJobObject(job.value, process_handle.value);
    }
    ResumeThread(thread_handle.value);

    std::string output;
    std::thread reader([&output, pipe = read_end.value] {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
            output.append(buffer, got);
        }
    });
    if (WaitForSingleObject(process_handle.value, static_cast<DWORD>(timeout.count())) == WAIT_TIMEOUT) {
        result.timed_out = true;
        if (job.value != nullptr) {
            TerminateJobObject(job.value, 1);
        } else {
            TerminateProcess(process_handle.value, 1);
        }
        WaitForSingleObject(process_handle.value, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(process_handle.value, &code);
    result.exit_code = static_cast<int>(code);
    // Whatever it left running would keep the pipe open.
    if (job.value != nullptr) {
        TerminateJobObject(job.value, 1);
    }
    reader.join();
    result.output = std::move(output);
    return result;
}

#else

ProcessResult Run(const fs::path& program, const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    ProcessResult result;
    int fds[2];
    if (::pipe(fds) != 0) {
        result.error = std::string("Could not make a pipe: ") + std::strerror(errno);
        return result;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own process group, so a timeout ends what it started too.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    const std::string path = program.string();
    std::vector<std::string> owned;
    owned.push_back(path);
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& arg : owned) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(fds[1]);
    if (spawned != 0) {
        ::close(fds[0]);
        result.error = "Could not start " + path + ": " + std::strerror(spawned);
        return result;
    }
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (!result.timed_out && left.count() <= 0) {
            result.timed_out = true;
            ::kill(-pid, SIGKILL);
        }
        pollfd ready{fds[0], POLLIN, 0};
        const int polled = ::poll(&ready, 1, result.timed_out ? 1000 : static_cast<int>(left.count()));
        if (polled < 0 && errno == EINTR) {
            continue;
        }
        if (polled == 0) {
            if (result.timed_out) {
                break;
            }
            continue;
        }
        const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        result.output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

#endif

}  // namespace

std::wstring windows_command_line(const fs::path& program, const std::vector<std::string>& args) {
    std::wstring line = QuoteArg(program.wstring(), false);
    for (const std::string& arg : args) {
        line += L" " + QuoteArg(path_from_utf8(arg).wstring(), false);
    }
    return line;
}

ProcessResult run_process(const fs::path& program, const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout) {
    std::error_code error;
    if (!fs::is_regular_file(program, error)) {
        ProcessResult result;
        result.error = utf8_path(program) + " was not found";
        return result;
    }
    return Run(program, args, timeout);
}

}  // namespace ide
