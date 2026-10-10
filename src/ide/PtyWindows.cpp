#include "Pty.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>

namespace ide {
namespace {

// ConPTY's three calls, looked up at run time so any SDK builds this and
// Windows 10 before 1809, which has no ConPTY, gets an error instead of a
// missing export.
using PseudoConsole = void*;
using CreateFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole*);
using ResizeFn = HRESULT(WINAPI*)(PseudoConsole, COORD);
using CloseFn = void(WINAPI*)(PseudoConsole);
// PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, which older SDKs do not define.
constexpr DWORD_PTR kPseudoConsoleAttribute = 0x00020016;

struct ConPty {
    CreateFn create = nullptr;
    ResizeFn resize = nullptr;
    CloseFn close = nullptr;
};

const ConPty* LoadConPty() {
    static const ConPty api = [] {
        ConPty out;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            out.create = reinterpret_cast<CreateFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
            out.resize = reinterpret_cast<ResizeFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
            out.close = reinterpret_cast<CloseFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        return out;
    }();
    return api.create != nullptr && api.resize != nullptr && api.close != nullptr ? &api : nullptr;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string ErrorText(const char* what, DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::string text = what;
    if (message != nullptr) {
        std::wstring wide(message);
        LocalFree(message);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.pop_back();
        }
        text += ": " + Narrow(wide);
    }
    return text;
}

// One argument, quoted so CommandLineToArgvW and the C runtime read it back unchanged.
void AppendArgument(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) {
        line.push_back(L' ');
    }
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line.push_back(L'"');
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            line.append(slashes * 2 + 1, L'\\');
        } else {
            line.append(slashes, L'\\');
        }
        line.push_back(arg[i]);
    }
    line.push_back(L'"');
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towupper(x) == std::towupper(y);
           });
}

// This process's environment with the additions, as the block CreateProcessW takes.
std::vector<wchar_t> EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& additions) {
    std::vector<std::wstring> entries;
    if (wchar_t* strings = GetEnvironmentStringsW()) {
        for (const wchar_t* entry = strings; *entry != L'\0'; entry += wcslen(entry) + 1) {
            entries.emplace_back(entry);
        }
        FreeEnvironmentStringsW(strings);
    }
    for (const auto& [name, value] : additions) {
        const std::wstring wide_name = Wide(name);
        const std::wstring entry = wide_name + L"=" + Wide(value);
        // Names that start with '=' are the per-drive folders, so the name ends at the next '='.
        auto same = std::find_if(entries.begin(), entries.end(), [&](const std::wstring& existing) {
            const std::size_t equals = existing.find(L'=', 1);
            return equals != std::wstring::npos && SameName(existing.substr(0, equals), wide_name);
        });
        if (same != entries.end()) {
            *same = entry;
        } else {
            entries.push_back(entry);
        }
    }
    std::vector<wchar_t> block;
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool OnPath(const wchar_t* program) {
    wchar_t found[MAX_PATH];
    return SearchPathW(nullptr, program, nullptr, MAX_PATH, found, nullptr) != 0;
}

COORD Size(int cols, int rows) {
    COORD size;
    size.X = static_cast<SHORT>(std::clamp(cols, 1, 32767));
    size.Y = static_cast<SHORT>(std::clamp(rows, 1, 32767));
    return size;
}

class WindowsPty final : public Pty {
public:
    WindowsPty(const ConPty& api, PseudoConsole console, HANDLE input, HANDLE output, HANDLE process,
               PtyOptions options)
        : api_(api),
          console_(console),
          input_(input),
          output_(output),
          process_(process),
          stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          on_output_(std::move(options.on_output)),
          on_exit_(std::move(options.on_exit)) {
        watcher_ = std::thread([this] { watch(); });
        reader_ = std::thread([this] { read(); });
    }

    ~WindowsPty() override {
        closing_.store(true);
        SetEvent(stop_);
        watcher_.join();
        // Closing the console ends the programs attached to it. The reader keeps
        // draining meanwhile, which the close can wait on.
        close_console();
        reader_.join();
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
        }
        CloseHandle(process_);
        CloseHandle(input_);
        CloseHandle(output_);
        CloseHandle(stop_);
    }

    void write(std::string_view bytes) override {
        std::lock_guard<std::mutex> lock(write_mutex_);
        while (!bytes.empty()) {
            DWORD wrote = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1 << 16));
            if (!WriteFile(input_, bytes.data(), chunk, &wrote, nullptr) || wrote == 0) {
                return;
            }
            bytes.remove_prefix(wrote);
        }
    }

    void resize(int cols, int rows) override {
        std::lock_guard<std::mutex> lock(console_mutex_);
        if (console_ != nullptr) {
            api_.resize(console_, Size(cols, rows));
        }
    }

private:
    // ConPTY keeps its output pipe open after the program exits, until the
    // console is closed. So a thread waits for the exit and closes it, and the
    // reader then reads to the end of what the program wrote.
    void watch() {
        const HANDLE waits[] = {process_, stop_};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
            close_console();
        }
    }

    void close_console() {
        std::lock_guard<std::mutex> lock(console_mutex_);
        if (console_ != nullptr) {
            api_.close(console_);
            console_ = nullptr;
        }
    }

    void read() {
        std::vector<char> buffer(1 << 16);
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(output_, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || got == 0) {
                break;
            }
            if (on_output_) {
                on_output_(std::string_view(buffer.data(), got));
            }
        }
        if (closing_.load()) {
            return;
        }
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        if (on_exit_) {
            on_exit_(static_cast<int>(code));
        }
    }

    const ConPty& api_;
    std::mutex console_mutex_;
    PseudoConsole console_;
    std::mutex write_mutex_;
    HANDLE input_;
    HANDLE output_;
    HANDLE process_;
    HANDLE stop_;
    std::function<void(std::string_view)> on_output_;
    std::function<void(int)> on_exit_;
    std::atomic<bool> closing_{false};
    std::thread watcher_;
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
    const ConPty* api = LoadConPty();
    if (api == nullptr) {
        return fail("This version of Windows has no pseudo-console (ConPTY needs Windows 10 1809 or later)");
    }

    // The program reads in_read and writes out_write; this side keeps the other ends.
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (!CreatePipe(&in_read, &in_write, nullptr, 0)) {
        return fail(ErrorText("Can't make the terminal's input pipe", GetLastError()));
    }
    if (!CreatePipe(&out_read, &out_write, nullptr, 0)) {
        const DWORD code = GetLastError();
        CloseHandle(in_read);
        CloseHandle(in_write);
        return fail(ErrorText("Can't make the terminal's output pipe", code));
    }
    PseudoConsole console = nullptr;
    const HRESULT made = api->create(Size(options.cols, options.rows), in_read, out_write, 0, &console);
    // The console holds its own copies of these.
    CloseHandle(in_read);
    CloseHandle(out_write);
    if (FAILED(made)) {
        CloseHandle(in_write);
        CloseHandle(out_read);
        return fail(ErrorText("Can't make a pseudo-console", static_cast<DWORD>(made)));
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<unsigned char> attribute_storage(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Without this, a parent whose own output is redirected (as under ctest)
    // hands the child those handles instead of the pseudo-console's.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    std::string failure;
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        failure = ErrorText("Can't attach the pseudo-console", GetLastError());
    } else {
        if (!UpdateProcThreadAttribute(attributes, 0, kPseudoConsoleAttribute, console, sizeof(console), nullptr,
                                       nullptr)) {
            failure = ErrorText("Can't attach the pseudo-console", GetLastError());
        } else {
            std::wstring command_line;
            AppendArgument(command_line, Wide(options.program));
            for (const std::string& arg : options.args) {
                AppendArgument(command_line, Wide(arg));
            }
            std::vector<wchar_t> environment = EnvironmentBlock(options.env);
            const std::wstring cwd = Wide(options.cwd);
            if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &process)) {
                failure = ErrorText(("Can't start " + options.program).c_str(), GetLastError());
            }
        }
        DeleteProcThreadAttributeList(attributes);
    }
    if (!failure.empty()) {
        api->close(console);
        CloseHandle(in_write);
        CloseHandle(out_read);
        return fail(std::move(failure));
    }
    CloseHandle(process.hThread);
    return std::make_unique<WindowsPty>(*api, console, in_write, out_read, process.hProcess, std::move(options));
}

PtyOptions DefaultShell() {
    PtyOptions options;
    // PowerShell 7 when it is installed, then the Windows PowerShell every copy of Windows has.
    options.program = OnPath(L"pwsh.exe") ? "pwsh.exe" : "powershell.exe";
    options.args = {"-NoLogo"};
    options.env.emplace_back("COLORTERM", "truecolor");
    return options;
}

}  // namespace ide
