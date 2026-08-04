#pragma once

#include "doof_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#define NOMINMAX
#include <windows.h>

// The UCRT exposes these stream names as function-like macros, but they are
// also part of std/os's public native ABI.
#undef stdin
#undef stdout
#undef stderr

namespace doof_os {

inline bool containsNul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

inline std::string windowsError(const std::string& action, DWORD error = ::GetLastError()) {
    return action + " (Windows error " + std::to_string(error) + ")";
}

inline doof::Result<std::wstring, std::string> utf8ToWide(const std::string& value) {
    if (value.empty()) return doof::Success<std::wstring>{std::wstring()};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size == 0) return doof::Failure<std::string>{windowsError("Failed to decode UTF-8")};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size) == 0) {
        return doof::Failure<std::string>{windowsError("Failed to decode UTF-8")};
    }
    return doof::Success<std::wstring>{result};
}

inline doof::Result<std::string, std::string> wideToUtf8(const std::wstring& value) {
    if (value.empty()) return doof::Success<std::string>{std::string()};
    const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) return doof::Failure<std::string>{windowsError("Failed to encode UTF-8")};
    std::string result(static_cast<size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) == 0) {
        return doof::Failure<std::string>{windowsError("Failed to encode UTF-8")};
    }
    return doof::Success<std::string>{result};
}

inline doof::Result<std::string, std::string> env(const std::string& name) {
    if (name.empty()) return doof::Failure<std::string>{"Environment variable name cannot be empty"};
    if (containsNul(name)) return doof::Failure<std::string>{"Environment variable name contains a NUL byte"};
    auto wideName = utf8ToWide(name);
    if (doof::is_failure(wideName)) return doof::Failure<std::string>{doof::failure_error(wideName)};
    const DWORD required = ::GetEnvironmentVariableW(doof::success_value(wideName).c_str(), nullptr, 0);
    if (required == 0) return doof::Failure<std::string>{"Environment variable not found: " + name};
    std::vector<wchar_t> buffer(required);
    const DWORD length = ::GetEnvironmentVariableW(doof::success_value(wideName).c_str(), buffer.data(), required);
    if (length == 0 || length >= required) return doof::Failure<std::string>{windowsError("Failed to read environment variable")};
    return wideToUtf8(std::wstring(buffer.data(), length));
}

inline int32_t pid() { return static_cast<int32_t>(::GetCurrentProcessId()); }
inline std::string platform() { return "windows"; }

inline std::string architecture() {
#if defined(_M_ARM64)
    return "arm64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_ARM)
    return "arm";
#elif defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

inline std::wstring quoteArgument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct CaseInsensitiveWideLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const {
        return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
            [](wchar_t a, wchar_t b) { return std::towlower(a) < std::towlower(b); });
    }
};

inline doof::Result<std::vector<wchar_t>, std::string> environmentBlock(
    const std::shared_ptr<std::vector<std::string>>& keys,
    const std::shared_ptr<std::vector<std::string>>& values,
    bool inherit
) {
    std::map<std::wstring, std::wstring, CaseInsensitiveWideLess> environment;
    if (inherit) {
        wchar_t* block = ::GetEnvironmentStringsW();
        if (block == nullptr) return doof::Failure<std::string>{windowsError("Failed to read process environment")};
        for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            const wchar_t* separator = std::wcschr(entry + (entry[0] == L'=' ? 1 : 0), L'=');
            if (separator != nullptr) environment[std::wstring(entry, separator)] = separator + 1;
        }
        ::FreeEnvironmentStringsW(block);
    }
    const size_t count = keys == nullptr ? 0 : keys->size();
    for (size_t index = 0; index < count; ++index) {
        auto key = utf8ToWide((*keys)[index]);
        auto value = utf8ToWide((*values)[index]);
        if (doof::is_failure(key)) return doof::Failure<std::string>{doof::failure_error(key)};
        if (doof::is_failure(value)) return doof::Failure<std::string>{doof::failure_error(value)};
        environment[doof::success_value(key)] = doof::success_value(value);
    }
    std::vector<wchar_t> result;
    for (const auto& pair : environment) {
        result.insert(result.end(), pair.first.begin(), pair.first.end());
        result.push_back(L'=');
        result.insert(result.end(), pair.second.begin(), pair.second.end());
        result.push_back(L'\0');
    }
    if (result.empty()) result.push_back(L'\0');
    result.push_back(L'\0');
    return doof::Success<std::vector<wchar_t>>{std::move(result)};
}

inline void closeHandle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
    handle = nullptr;
}

inline std::shared_ptr<std::vector<uint8_t>> readChunk(HANDLE& handle, bool& open) {
    if (!open || handle == nullptr) return nullptr;
    auto bytes = std::make_shared<std::vector<uint8_t>>(4096);
    DWORD count = 0;
    if (::ReadFile(handle, bytes->data(), static_cast<DWORD>(bytes->size()), &count, nullptr) != 0 && count > 0) {
        bytes->resize(count);
        return bytes;
    }
    const DWORD error = ::GetLastError();
    closeHandle(handle);
    open = false;
    if (count == 0 && (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF || error == ERROR_SUCCESS)) return nullptr;
    doof::panic(windowsError("Failed to read process output", error));
}

}  // namespace doof_os

class NativeRunResult {
public:
    NativeRunResult(int32_t exitCode, std::shared_ptr<std::vector<uint8_t>> stdoutBytes,
        std::shared_ptr<std::vector<uint8_t>> stderrBytes, bool stdoutTruncated, bool stderrTruncated)
        : exitCode_(exitCode), stdout_(std::move(stdoutBytes)), stderr_(std::move(stderrBytes)),
          stdoutTruncated_(stdoutTruncated), stderrTruncated_(stderrTruncated) {}
    int32_t exitCode() const { return exitCode_; }
    std::shared_ptr<std::vector<uint8_t>> stdout() const { return stdout_; }
    std::shared_ptr<std::vector<uint8_t>> stderr() const { return stderr_; }
    bool stdoutTruncated() const { return stdoutTruncated_; }
    bool stderrTruncated() const { return stderrTruncated_; }
private:
    int32_t exitCode_;
    std::shared_ptr<std::vector<uint8_t>> stdout_;
    std::shared_ptr<std::vector<uint8_t>> stderr_;
    bool stdoutTruncated_;
    bool stderrTruncated_;
};

class NativeExecProcess {
public:
    static doof::Result<std::shared_ptr<NativeExecProcess>, std::string> spawn(
        const std::string& command, const std::shared_ptr<std::vector<std::string>>& args,
        const std::optional<std::string>& cwd, const std::shared_ptr<std::vector<std::string>>& envKeys,
        const std::shared_ptr<std::vector<std::string>>& envValues, bool inheritEnv, bool withStdin,
        bool mergeStderrIntoStdout, bool inheritOutput, const std::optional<int64_t>& maxOutputBytes,
        const std::optional<int64_t>& timeoutNanos
    ) {
        if (command.empty()) return doof::Failure<std::string>{"Command cannot be empty"};
        if (doof_os::containsNul(command)) return doof::Failure<std::string>{"Command contains a NUL byte"};
        if (timeoutNanos.has_value() && timeoutNanos.value() < 0) return doof::Failure<std::string>{"Process timeout cannot be negative"};
        if (maxOutputBytes.has_value() && maxOutputBytes.value() < 0) return doof::Failure<std::string>{"Maximum output bytes cannot be negative"};
        const size_t keyCount = envKeys == nullptr ? 0 : envKeys->size();
        const size_t valueCount = envValues == nullptr ? 0 : envValues->size();
        if (keyCount != valueCount) return doof::Failure<std::string>{"Environment key/value arrays must be the same length"};
        if (cwd.has_value() && doof_os::containsNul(cwd.value())) return doof::Failure<std::string>{"Working directory contains a NUL byte"};
        for (size_t index = 0; index < keyCount; ++index) {
            if ((*envKeys)[index].empty()) return doof::Failure<std::string>{"Environment variable name cannot be empty"};
            if ((*envKeys)[index].find('=') != std::string::npos) return doof::Failure<std::string>{"Environment variable name cannot contain '='"};
            if (doof_os::containsNul((*envKeys)[index]) || doof_os::containsNul((*envValues)[index])) return doof::Failure<std::string>{"Environment variable key/value contains a NUL byte"};
        }

        auto wideCommand = doof_os::utf8ToWide(command);
        if (doof::is_failure(wideCommand)) return doof::Failure<std::string>{doof::failure_error(wideCommand)};
        std::wstring commandLine = doof_os::quoteArgument(doof::success_value(wideCommand));
        if (args != nullptr) {
            for (const auto& arg : *args) {
                if (doof_os::containsNul(arg)) return doof::Failure<std::string>{"Argument contains a NUL byte"};
                auto wideArg = doof_os::utf8ToWide(arg);
                if (doof::is_failure(wideArg)) return doof::Failure<std::string>{doof::failure_error(wideArg)};
                commandLine += L" " + doof_os::quoteArgument(doof::success_value(wideArg));
            }
        }
        std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
        mutableCommand.push_back(L'\0');
        auto environment = doof_os::environmentBlock(envKeys, envValues, inheritEnv);
        if (doof::is_failure(environment)) return doof::Failure<std::string>{doof::failure_error(environment)};
        std::optional<std::wstring> wideCwd;
        if (cwd.has_value()) {
            auto converted = doof_os::utf8ToWide(cwd.value());
            if (doof::is_failure(converted)) return doof::Failure<std::string>{doof::failure_error(converted)};
            wideCwd = doof::success_value(converted);
        }

        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdoutRead = nullptr, stdoutWrite = nullptr, stderrRead = nullptr, stderrWrite = nullptr;
        HANDLE stdinRead = nullptr, stdinWrite = nullptr;
        auto fail = [&](const std::string& message) {
            doof_os::closeHandle(stdoutRead); doof_os::closeHandle(stdoutWrite);
            doof_os::closeHandle(stderrRead); doof_os::closeHandle(stderrWrite);
            doof_os::closeHandle(stdinRead); doof_os::closeHandle(stdinWrite);
            return doof::Failure<std::string>{message};
        };
        if (!inheritOutput && (!::CreatePipe(&stdoutRead, &stdoutWrite, &security, 0) || !::SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0))) return fail(doof_os::windowsError("Failed to create stdout pipe"));
        if (!inheritOutput && !mergeStderrIntoStdout && (!::CreatePipe(&stderrRead, &stderrWrite, &security, 0) || !::SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0))) return fail(doof_os::windowsError("Failed to create stderr pipe"));
        if (withStdin && (!::CreatePipe(&stdinRead, &stdinWrite, &security, 0) || !::SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0))) return fail(doof_os::windowsError("Failed to create stdin pipe"));

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = withStdin ? stdinRead : ::GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = inheritOutput ? ::GetStdHandle(STD_OUTPUT_HANDLE) : stdoutWrite;
        startup.hStdError = inheritOutput ? ::GetStdHandle(STD_ERROR_HANDLE) : (mergeStderrIntoStdout ? stdoutWrite : stderrWrite);
        PROCESS_INFORMATION info{};
        const BOOL created = ::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP, doof::success_value(environment).data(),
            wideCwd.has_value() ? wideCwd->c_str() : nullptr, &startup, &info);
        doof_os::closeHandle(stdoutWrite); doof_os::closeHandle(stderrWrite); doof_os::closeHandle(stdinRead);
        if (!created) return fail(doof_os::windowsError("Failed to spawn process"));

        auto process = std::shared_ptr<NativeExecProcess>(new NativeExecProcess(info.hProcess, info.hThread,
            info.dwProcessId, maxOutputBytes, timeoutNanos));
        process->stdoutRead_ = stdoutRead;
        process->stderrRead_ = mergeStderrIntoStdout ? nullptr : stderrRead;
        process->stdinWrite_ = withStdin ? stdinWrite : nullptr;
        process->stdoutOpen_ = stdoutRead != nullptr;
        process->stderrOpen_ = stderrRead != nullptr && !mergeStderrIntoStdout;
        process->stdinOpen_ = stdinWrite != nullptr && withStdin;
        return doof::Success<std::shared_ptr<NativeExecProcess>>{process};
    }

    ~NativeExecProcess() {
        doof_os::closeHandle(stdoutRead_); doof_os::closeHandle(stderrRead_); doof_os::closeHandle(stdinWrite_);
        doof_os::closeHandle(thread_); doof_os::closeHandle(process_);
    }

    std::shared_ptr<std::vector<uint8_t>> nextStdoutChunk() { return doof_os::readChunk(stdoutRead_, stdoutOpen_); }
    std::shared_ptr<std::vector<uint8_t>> nextStderrChunk() { return doof_os::readChunk(stderrRead_, stderrOpen_); }

    doof::Result<void, std::string> writeStdinText(const std::string& value) {
        if (!stdinOpen_ || stdinWrite_ == nullptr) return doof::Failure<std::string>{"Stdin is not open for this process"};
        if (doof_os::containsNul(value)) return doof::Failure<std::string>{"Stdin payload contains a NUL byte"};
        size_t written = 0;
        while (written < value.size()) {
            DWORD count = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(value.size() - written, 0xffffffffu));
            if (!::WriteFile(stdinWrite_, value.data() + written, chunk, &count, nullptr)) return doof::Failure<std::string>{doof_os::windowsError("Failed to write to stdin")};
            written += count;
        }
        return doof::Success<void>{};
    }

    doof::Result<void, std::string> closeStdin() {
        doof_os::closeHandle(stdinWrite_);
        stdinOpen_ = false;
        return doof::Success<void>{};
    }

    bool isRunning() {
        if (exited_ || process_ == nullptr) return false;
        const DWORD status = ::WaitForSingleObject(process_, 0);
        if (status == WAIT_TIMEOUT) return true;
        if (status == WAIT_OBJECT_0) { recordExitCode(); return false; }
        return false;
    }

    doof::Result<int32_t, std::string> wait() {
        if (process_ == nullptr) return doof::Failure<std::string>{"Process handle is invalid"};
        if (exited_) return doof::Success<int32_t>{exitCode_};
        const DWORD timeout = timeoutMilliseconds();
        const DWORD status = ::WaitForSingleObject(process_, timeout);
        if (status == WAIT_TIMEOUT) {
            ::TerminateProcess(process_, 1);
            ::WaitForSingleObject(process_, INFINITE);
            recordExitCode();
            return doof::Failure<std::string>{timeoutMessage()};
        }
        if (status != WAIT_OBJECT_0) return doof::Failure<std::string>{doof_os::windowsError("Failed waiting for process")};
        recordExitCode();
        return doof::Success<int32_t>{exitCode_};
    }

    doof::Result<std::shared_ptr<NativeRunResult>, std::string> runToCompletion() {
        (void)closeStdin();
        auto stdoutBytes = std::make_shared<std::vector<uint8_t>>();
        auto stderrBytes = std::make_shared<std::vector<uint8_t>>();
        bool stdoutTruncated = false, stderrTruncated = false;
        std::thread stdoutReader([&]() { drain(stdoutRead_, stdoutOpen_, *stdoutBytes, stdoutTruncated); });
        std::thread stderrReader([&]() { drain(stderrRead_, stderrOpen_, *stderrBytes, stderrTruncated); });
        auto waited = wait();
        stdoutReader.join();
        stderrReader.join();
        if (doof::is_failure(waited)) return doof::Failure<std::string>{doof::failure_error(waited)};
        return doof::Success<std::shared_ptr<NativeRunResult>>{std::make_shared<NativeRunResult>(
            doof::success_value(waited), stdoutBytes, stderrBytes, stdoutTruncated, stderrTruncated)};
    }

    doof::Result<void, std::string> terminate(int32_t signal) {
        if (!isRunning()) return doof::Failure<std::string>{"Process is not running"};
        if (!::TerminateProcess(process_, static_cast<UINT>(128 + signal))) return doof::Failure<std::string>{doof_os::windowsError("Failed to terminate process")};
        return doof::Success<void>{};
    }

    bool stdoutOpen() const { return stdoutOpen_; }
    bool stderrOpen() const { return stderrOpen_; }

private:
    NativeExecProcess(HANDLE process, HANDLE thread, DWORD processId, std::optional<int64_t> maxOutputBytes,
        std::optional<int64_t> timeoutNanos)
        : process_(process), thread_(thread), processId_(processId), startedAt_(std::chrono::steady_clock::now()),
          maxOutputBytes_(maxOutputBytes), timeoutNanos_(timeoutNanos) {}

    void recordExitCode() {
        DWORD code = 0;
        if (::GetExitCodeProcess(process_, &code)) { exitCode_ = static_cast<int32_t>(code); exited_ = true; }
    }

    DWORD timeoutMilliseconds() const {
        if (!timeoutNanos_.has_value()) return INFINITE;
        const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
        const auto remaining = std::chrono::nanoseconds(timeoutNanos_.value()) - elapsed;
        if (remaining <= std::chrono::nanoseconds::zero()) return 0;
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
        return static_cast<DWORD>(std::min<int64_t>(millis <= 0 ? 1 : millis, INFINITE - 1));
    }

    std::string timeoutMessage() const {
        return timeoutNanos_.has_value() ? "Process timed out after " + std::to_string(timeoutNanos_.value()) + " ns" : "Process timed out";
    }

    void drain(HANDLE& handle, bool& open, std::vector<uint8_t>& output, bool& truncated) {
        while (open) {
            auto chunk = doof_os::readChunk(handle, open);
            if (chunk == nullptr) break;
            size_t retained = chunk->size();
            if (maxOutputBytes_.has_value()) {
                const size_t limit = static_cast<size_t>(maxOutputBytes_.value());
                retained = output.size() >= limit ? 0 : std::min(retained, limit - output.size());
                if (retained < chunk->size()) truncated = true;
            }
            output.insert(output.end(), chunk->begin(), chunk->begin() + retained);
        }
    }

    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    DWORD processId_ = 0;
    std::chrono::steady_clock::time_point startedAt_;
    std::optional<int64_t> maxOutputBytes_;
    std::optional<int64_t> timeoutNanos_;
    HANDLE stdoutRead_ = nullptr;
    HANDLE stderrRead_ = nullptr;
    HANDLE stdinWrite_ = nullptr;
    bool stdoutOpen_ = false;
    bool stderrOpen_ = false;
    bool stdinOpen_ = false;
    bool exited_ = false;
    int32_t exitCode_ = -1;
};
