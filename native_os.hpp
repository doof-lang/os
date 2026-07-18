#pragma once

#include "doof_runtime.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

#if defined(__linux__)
extern "C" int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t*, const char*);
#endif

namespace doof_os {

inline bool containsNul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

inline std::string errnoMessage(const std::string& action) {
    return action + ": " + std::strerror(errno);
}

inline doof::Result<std::string, std::string> env(const std::string& name) {
    if (name.empty()) {
        return doof::Failure<std::string>{"Environment variable name cannot be empty"};
    }
    if (containsNul(name)) {
        return doof::Failure<std::string>{"Environment variable name contains a NUL byte"};
    }

    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return doof::Failure<std::string>{"Environment variable not found: " + name};
    }

    return doof::Success<std::string>{std::string(value)};
}

inline int32_t pid() {
    return static_cast<int32_t>(::getpid());
}

inline std::string platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#else
    return "unknown";
#endif
}

inline std::string architecture() {
#if defined(__aarch64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x64";
#elif defined(__arm__)
    return "arm";
#elif defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

inline bool readChunkFromFd(int& fd, bool& isOpen, std::shared_ptr<std::vector<uint8_t>>& out) {
    if (!isOpen || fd < 0) {
        out = nullptr;
        return true;
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>(4096);

    while (true) {
        const ssize_t bytesRead = ::read(fd, buffer->data(), buffer->size());
        if (bytesRead > 0) {
            buffer->resize(static_cast<size_t>(bytesRead));
            out = buffer;
            return true;
        }

        if (bytesRead == 0) {
            ::close(fd);
            fd = -1;
            isOpen = false;
            out = nullptr;
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        out = nullptr;
        return false;
    }
}

inline doof::Result<void, std::string> writeAllText(int fd, const std::string& value) {
    const char* data = value.data();
    size_t remaining = value.size();

    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written > 0) {
            data += written;
            remaining -= static_cast<size_t>(written);
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written < 0 && errno == EPIPE) {
            return doof::Failure<std::string>{"Cannot write to stdin: process closed the pipe"};
        }

        return doof::Failure<std::string>{errnoMessage("Failed to write to stdin")};
    }

    return doof::Success<void>{};
}

inline bool setNonBlocking(int fd, std::string& error) {
    if (fd < 0) {
        return true;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        error = errnoMessage("Failed to read file descriptor flags");
        return false;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = errnoMessage("Failed to set file descriptor non-blocking");
        return false;
    }

    return true;
}

inline bool clearEnvironment(std::string& error) {
#if defined(__APPLE__)
    std::vector<std::string> keys;
    if (environ != nullptr) {
        for (char** entry = environ; *entry != nullptr; ++entry) {
            const std::string pair(*entry);
            const size_t separator = pair.find('=');
            if (separator == std::string::npos || separator == 0) {
                continue;
            }
            keys.push_back(pair.substr(0, separator));
        }
    }

    for (const auto& key : keys) {
        if (::unsetenv(key.c_str()) != 0) {
            error = doof_os::errnoMessage("Failed to clear environment");
            return false;
        }
    }
    return true;
#else
    if (::clearenv() != 0) {
        error = doof_os::errnoMessage("Failed to clear environment");
        return false;
    }
    return true;
#endif
}

}  // namespace doof_os

class NativeRunResult {
public:
    NativeRunResult(
        int32_t exitCode,
        std::shared_ptr<std::vector<uint8_t>> stdoutBytes,
        std::shared_ptr<std::vector<uint8_t>> stderrBytes,
        bool stdoutTruncated,
        bool stderrTruncated
    ) : exitCode_(exitCode),
        stdout_(stdoutBytes == nullptr ? std::make_shared<std::vector<uint8_t>>() : stdoutBytes),
        stderr_(stderrBytes == nullptr ? std::make_shared<std::vector<uint8_t>>() : stderrBytes),
        stdoutTruncated_(stdoutTruncated),
        stderrTruncated_(stderrTruncated) {}

    int32_t exitCode() const {
        return exitCode_;
    }

    std::shared_ptr<std::vector<uint8_t>> stdout() const {
        return stdout_;
    }

    std::shared_ptr<std::vector<uint8_t>> stderr() const {
        return stderr_;
    }

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
        const std::string& command,
        const std::shared_ptr<std::vector<std::string>>& args,
        const std::optional<std::string>& cwd,
        const std::shared_ptr<std::vector<std::string>>& envKeys,
        const std::shared_ptr<std::vector<std::string>>& envValues,
        bool inheritEnv,
        bool withStdin,
        bool mergeStderrIntoStdout,
        bool inheritOutput,
        const std::optional<int64_t>& maxOutputBytes,
        const std::optional<int64_t>& timeoutNanos
    ) {
        if (command.empty()) {
            return doof::Failure<std::string>{"Command cannot be empty"};
        }
        if (timeoutNanos.has_value() && timeoutNanos.value() < 0) {
            return doof::Failure<std::string>{"Process timeout cannot be negative"};
        }
        if (maxOutputBytes.has_value() && maxOutputBytes.value() < 0) {
            return doof::Failure<std::string>{"Maximum output bytes cannot be negative"};
        }
        if (doof_os::containsNul(command)) {
            return doof::Failure<std::string>{"Command contains a NUL byte"};
        }

        if (args != nullptr) {
            for (const auto& arg : *args) {
                if (doof_os::containsNul(arg)) {
                    return doof::Failure<std::string>{"Argument contains a NUL byte"};
                }
            }
        }

        if (cwd.has_value() && doof_os::containsNul(cwd.value())) {
            return doof::Failure<std::string>{"Working directory contains a NUL byte"};
        }

        const size_t keyCount = envKeys == nullptr ? 0 : envKeys->size();
        const size_t valueCount = envValues == nullptr ? 0 : envValues->size();
        if (keyCount != valueCount) {
            return doof::Failure<std::string>{"Environment key/value arrays must be the same length"};
        }

        for (size_t i = 0; i < keyCount; ++i) {
            const std::string& key = (*envKeys)[i];
            const std::string& value = (*envValues)[i];
            if (key.empty()) {
                return doof::Failure<std::string>{"Environment variable name cannot be empty"};
            }
            if (key.find('=') != std::string::npos) {
                return doof::Failure<std::string>{"Environment variable name cannot contain '='"};
            }
            if (doof_os::containsNul(key) || doof_os::containsNul(value)) {
                return doof::Failure<std::string>{"Environment variable key/value contains a NUL byte"};
            }
        }

        int stdoutPipe[2] = {-1, -1};
        int stderrPipe[2] = {-1, -1};
        int stdinPipe[2] = {-1, -1};

        if (!inheritOutput && ::pipe(stdoutPipe) != 0) {
            return doof::Failure<std::string>{doof_os::errnoMessage("Failed to create stdout pipe")};
        }

        if (!inheritOutput && !mergeStderrIntoStdout && ::pipe(stderrPipe) != 0) {
            if (stdoutPipe[0] >= 0) ::close(stdoutPipe[0]);
            if (stdoutPipe[1] >= 0) ::close(stdoutPipe[1]);
            return doof::Failure<std::string>{doof_os::errnoMessage("Failed to create stderr pipe")};
        }

        if (withStdin && ::pipe(stdinPipe) != 0) {
            if (stdoutPipe[0] >= 0) ::close(stdoutPipe[0]);
            if (stdoutPipe[1] >= 0) ::close(stdoutPipe[1]);
            if (stderrPipe[0] >= 0) {
                ::close(stderrPipe[0]);
                ::close(stderrPipe[1]);
            }
            return doof::Failure<std::string>{doof_os::errnoMessage("Failed to create stdin pipe")};
        }

        std::vector<char*> argv;
        argv.reserve((args == nullptr ? 0 : args->size()) + 2);
        argv.push_back(const_cast<char*>(command.c_str()));
        if (args != nullptr) {
            for (const auto& arg : *args) argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        std::vector<std::string> environment;
        if (inheritEnv && environ != nullptr) {
            for (char** entry = environ; *entry != nullptr; ++entry) {
                const std::string pair(*entry);
                const size_t separator = pair.find('=');
                bool overridden = false;
                for (size_t i = 0; i < keyCount; ++i) {
                    if (separator == (*envKeys)[i].size() && pair.compare(0, separator, (*envKeys)[i]) == 0) {
                        overridden = true;
                        break;
                    }
                }
                if (!overridden) environment.push_back(pair);
            }
        }
        for (size_t i = 0; i < keyCount; ++i) environment.push_back((*envKeys)[i] + "=" + (*envValues)[i]);
        std::vector<char*> envp;
        envp.reserve(environment.size() + 1);
        for (auto& entry : environment) envp.push_back(entry.data());
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        int setupResult = ::posix_spawn_file_actions_init(&actions);
        const bool actionsInitialized = setupResult == 0;
        if (setupResult == 0 && !inheritOutput) {
            setupResult = ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
            if (mergeStderrIntoStdout) {
                if (setupResult == 0) setupResult = ::posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDERR_FILENO);
            } else {
                if (setupResult == 0) setupResult = ::posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
            }
        }
        if (setupResult == 0 && withStdin) setupResult = ::posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
        if (setupResult == 0 && stdoutPipe[0] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
        if (setupResult == 0 && stdoutPipe[1] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
        if (setupResult == 0 && stderrPipe[0] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
        if (setupResult == 0 && stderrPipe[1] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);
        if (setupResult == 0 && stdinPipe[0] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stdinPipe[0]);
        if (setupResult == 0 && stdinPipe[1] >= 0) setupResult = ::posix_spawn_file_actions_addclose(&actions, stdinPipe[1]);
        if (setupResult == 0 && cwd.has_value()) {
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            setupResult = ::posix_spawn_file_actions_addchdir_np(&actions, cwd->c_str());
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
        }

        posix_spawnattr_t attributes;
        if (setupResult == 0) setupResult = ::posix_spawnattr_init(&attributes);
        const bool attributesInitialized = setupResult == 0;
        if (setupResult == 0) setupResult = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
        if (setupResult == 0) setupResult = ::posix_spawnattr_setpgroup(&attributes, 0);

        pid_t childPid = 0;
        const int spawnResult = setupResult == 0
            ? ::posix_spawnp(&childPid, command.c_str(), &actions, &attributes, argv.data(), envp.data())
            : setupResult;
        if (attributesInitialized) ::posix_spawnattr_destroy(&attributes);
        if (actionsInitialized) ::posix_spawn_file_actions_destroy(&actions);

        if (stdoutPipe[1] >= 0) ::close(stdoutPipe[1]);
        if (stderrPipe[1] >= 0) ::close(stderrPipe[1]);
        if (stdinPipe[0] >= 0) ::close(stdinPipe[0]);

        if (spawnResult != 0) {
            if (stdoutPipe[0] >= 0) ::close(stdoutPipe[0]);
            if (stderrPipe[0] >= 0) ::close(stderrPipe[0]);
            if (stdinPipe[1] >= 0) ::close(stdinPipe[1]);
            return doof::Failure<std::string>{"Failed to spawn process: " + std::string(std::strerror(spawnResult))};
        }

        auto process = std::shared_ptr<NativeExecProcess>(new NativeExecProcess(childPid, maxOutputBytes, timeoutNanos));
        process->stdoutFd_ = stdoutPipe[0];
        process->stdoutOpen_ = stdoutPipe[0] >= 0;

        if (mergeStderrIntoStdout) {
            process->stderrFd_ = -1;
            process->stderrOpen_ = false;
        } else if (!inheritOutput) {
            process->stderrFd_ = stderrPipe[0];
            process->stderrOpen_ = true;
        }

        process->stdinFd_ = withStdin ? stdinPipe[1] : -1;
        process->stdinOpen_ = withStdin;

        return doof::Success<std::shared_ptr<NativeExecProcess>>{process};
    }

    ~NativeExecProcess() {
        if (stdoutFd_ >= 0) {
            ::close(stdoutFd_);
        }
        if (stderrFd_ >= 0) {
            ::close(stderrFd_);
        }
        if (stdinFd_ >= 0) {
            ::close(stdinFd_);
        }

        if (!exited_ && childPid_ > 0) {
            int status = 0;
            const pid_t waited = ::waitpid(childPid_, &status, WNOHANG);
            if (waited == childPid_) {
                exited_ = true;
                exitCode_ = decodeExitCode(status);
            }
        }
    }

    std::shared_ptr<std::vector<uint8_t>> nextStdoutChunk() {
        std::shared_ptr<std::vector<uint8_t>> out;
        if (!doof_os::readChunkFromFd(stdoutFd_, stdoutOpen_, out)) {
            doof::panic(doof_os::errnoMessage("Failed to read stdout"));
        }
        return out;
    }

    std::shared_ptr<std::vector<uint8_t>> nextStderrChunk() {
        std::shared_ptr<std::vector<uint8_t>> out;
        if (!doof_os::readChunkFromFd(stderrFd_, stderrOpen_, out)) {
            doof::panic(doof_os::errnoMessage("Failed to read stderr"));
        }
        return out;
    }

    doof::Result<void, std::string> writeStdinText(const std::string& value) {
        if (!stdinOpen_ || stdinFd_ < 0) {
            return doof::Failure<std::string>{"Stdin is not open for this process"};
        }
        if (doof_os::containsNul(value)) {
            return doof::Failure<std::string>{"Stdin payload contains a NUL byte"};
        }

        return doof_os::writeAllText(stdinFd_, value);
    }

    doof::Result<void, std::string> closeStdin() {
        if (!stdinOpen_ || stdinFd_ < 0) {
            return doof::Success<void>{};
        }

        const int fd = stdinFd_;
        stdinFd_ = -1;
        stdinOpen_ = false;
        if (::close(fd) != 0) {
            return doof::Failure<std::string>{doof_os::errnoMessage("Failed to close stdin")};
        }

        return doof::Success<void>{};
    }

    bool isRunning() {
        if (exited_ || childPid_ <= 0) {
            return false;
        }

        int status = 0;
        const pid_t waited = ::waitpid(childPid_, &status, WNOHANG);
        if (waited == 0) {
            return true;
        }

        if (waited == childPid_) {
            exited_ = true;
            exitCode_ = decodeExitCode(status);
            return false;
        }

        return false;
    }

    doof::Result<int32_t, std::string> wait() {
        if (childPid_ <= 0) {
            return doof::Failure<std::string>{"Process handle is invalid"};
        }

        if (exited_) {
            return doof::Success<int32_t>{exitCode_};
        }

        if (!timeoutNanos_.has_value()) {
            int status = 0;
            while (true) {
                const pid_t waited = ::waitpid(childPid_, &status, 0);
                if (waited == childPid_) {
                    exited_ = true;
                    exitCode_ = decodeExitCode(status);
                    return doof::Success<int32_t>{exitCode_};
                }
                if (waited < 0 && errno == EINTR) {
                    continue;
                }
                return doof::Failure<std::string>{doof_os::errnoMessage("Failed waiting for process")};
            }
        }

        while (!exited_) {
            reapChildNoHang();
            if (exited_) {
                return doof::Success<int32_t>{exitCode_};
            }

            if (hasTimedOut()) {
                killTimedOutProcess();
                return doof::Failure<std::string>{timeoutMessage()};
            }

            usleep(1000);
        }

        return doof::Success<int32_t>{exitCode_};
    }

    doof::Result<std::shared_ptr<NativeRunResult>, std::string> runToCompletion() {
        if (childPid_ <= 0) {
            return doof::Failure<std::string>{"Process handle is invalid"};
        }

        std::string nonBlockingError;
        if (!doof_os::setNonBlocking(stdoutFd_, nonBlockingError) || !doof_os::setNonBlocking(stderrFd_, nonBlockingError)) {
            return doof::Failure<std::string>{nonBlockingError};
        }

        const auto stdoutBytes = std::make_shared<std::vector<uint8_t>>();
        const auto stderrBytes = std::make_shared<std::vector<uint8_t>>();
        bool stdoutTruncated = false;
        bool stderrTruncated = false;

        if (stdinOpen_ && stdinFd_ >= 0) {
            (void)closeStdin();
        }

        while (stdoutOpen_ || stderrOpen_ || !exited_) {
            std::string readError;
            if (!readAvailable(stdoutFd_, stdoutOpen_, *stdoutBytes, stdoutTruncated, maxOutputBytes_, "stdout", readError)) {
                return doof::Failure<std::string>{readError};
            }
            if (!readAvailable(stderrFd_, stderrOpen_, *stderrBytes, stderrTruncated, maxOutputBytes_, "stderr", readError)) {
                return doof::Failure<std::string>{readError};
            }

            reapChildNoHang();
            if (!stdoutOpen_ && !stderrOpen_ && exited_) {
                break;
            }

            int pollTimeoutMs = -1;
            if (timeoutNanos_.has_value()) {
                if (hasTimedOut()) {
                    killTimedOutProcess();
                    return doof::Failure<std::string>{timeoutMessage()};
                }
                pollTimeoutMs = remainingTimeoutMillis();
            }

            struct pollfd fds[2];
            nfds_t fdCount = 0;
            if (stdoutOpen_ && stdoutFd_ >= 0) {
                fds[fdCount].fd = stdoutFd_;
                fds[fdCount].events = POLLIN | POLLHUP | POLLERR;
                fds[fdCount].revents = 0;
                ++fdCount;
            }
            if (stderrOpen_ && stderrFd_ >= 0) {
                fds[fdCount].fd = stderrFd_;
                fds[fdCount].events = POLLIN | POLLHUP | POLLERR;
                fds[fdCount].revents = 0;
                ++fdCount;
            }

            if (fdCount == 0) {
                const int status = waitForExitSlice(pollTimeoutMs);
                if (status < 0) {
                    return doof::Failure<std::string>{doof_os::errnoMessage("Failed waiting for process")};
                }
                continue;
            }

            while (true) {
                const int ready = ::poll(fds, fdCount, pollTimeoutMs);
                if (ready >= 0) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                return doof::Failure<std::string>{doof_os::errnoMessage("Failed polling process output")};
            }
        }

        const auto result = std::make_shared<NativeRunResult>(
            exitCode_, stdoutBytes, stderrBytes, stdoutTruncated, stderrTruncated
        );
        return doof::Success<std::shared_ptr<NativeRunResult>>{result};
    }

    doof::Result<void, std::string> terminate(int32_t signal) {
        if (childPid_ <= 0 || exited_) {
            return doof::Failure<std::string>{"Process is not running"};
        }

        if (::kill(childPid_, signal) != 0) {
            return doof::Failure<std::string>{doof_os::errnoMessage("Failed to signal process")};
        }

        return doof::Success<void>{};
    }

    bool stdoutOpen() const {
        return stdoutOpen_;
    }

    bool stderrOpen() const {
        return stderrOpen_;
    }

private:
    explicit NativeExecProcess(
        pid_t childPid,
        std::optional<int64_t> maxOutputBytes,
        std::optional<int64_t> timeoutNanos
    )
        : childPid_(childPid),
          startedAt_(std::chrono::steady_clock::now()),
          maxOutputBytes_(maxOutputBytes),
          timeoutNanos_(timeoutNanos),
          stdoutFd_(-1),
          stderrFd_(-1),
          stdinFd_(-1),
          stdoutOpen_(false),
          stderrOpen_(false),
          stdinOpen_(false),
          exited_(false),
          exitCode_(-1) {}

    static int32_t decodeExitCode(int status) {
        if (WIFEXITED(status)) {
            return static_cast<int32_t>(WEXITSTATUS(status));
        }
        if (WIFSIGNALED(status)) {
            return 128 + static_cast<int32_t>(WTERMSIG(status));
        }
        return -1;
    }

    static bool readAvailable(
        int& fd,
        bool& isOpen,
        std::vector<uint8_t>& output,
        bool& truncated,
        const std::optional<int64_t>& maxOutputBytes,
        const std::string& name,
        std::string& error
    ) {
        if (!isOpen || fd < 0) {
            return true;
        }

        uint8_t buffer[4096];
        while (true) {
            const ssize_t bytesRead = ::read(fd, buffer, sizeof(buffer));
            if (bytesRead > 0) {
                size_t retained = static_cast<size_t>(bytesRead);
                if (maxOutputBytes.has_value()) {
                    const size_t limit = static_cast<size_t>(maxOutputBytes.value());
                    retained = output.size() >= limit ? 0 : std::min(retained, limit - output.size());
                    if (retained < static_cast<size_t>(bytesRead)) truncated = true;
                }
                output.insert(output.end(), buffer, buffer + retained);
                continue;
            }
            if (bytesRead == 0) {
                ::close(fd);
                fd = -1;
                isOpen = false;
                return true;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            error = doof_os::errnoMessage("Failed to read " + name);
            return false;
        }
    }

    void reapChildNoHang() {
        if (exited_ || childPid_ <= 0) {
            return;
        }

        int status = 0;
        const pid_t waited = ::waitpid(childPid_, &status, WNOHANG);
        if (waited == childPid_) {
            exited_ = true;
            exitCode_ = decodeExitCode(status);
        }
    }

    bool hasTimedOut() const {
        if (!timeoutNanos_.has_value()) {
            return false;
        }
        return std::chrono::steady_clock::now() - startedAt_ >= std::chrono::nanoseconds(timeoutNanos_.value());
    }

    int remainingTimeoutMillis() const {
        if (!timeoutNanos_.has_value()) {
            return -1;
        }

        const auto timeout = std::chrono::nanoseconds(timeoutNanos_.value());
        const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
        if (elapsed >= timeout) {
            return 0;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(timeout - elapsed).count();
        return remaining <= 0 ? 0 : static_cast<int>(remaining > 2147483647LL ? 2147483647LL : remaining);
    }

    std::string timeoutMessage() const {
        if (!timeoutNanos_.has_value()) {
            return "Process timed out";
        }
        return "Process timed out after " + std::to_string(timeoutNanos_.value()) + " ns";
    }

    int waitForExitSlice(int timeoutMs) {
        const auto start = std::chrono::steady_clock::now();
        while (!exited_) {
            reapChildNoHang();
            if (exited_) {
                return 0;
            }

            if (timeoutMs >= 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start
                ).count();
                if (elapsed >= timeoutMs) {
                    return 0;
                }
            }

            usleep(1000);
        }
        return 0;
    }

    void killTimedOutProcess() {
        if (!exited_ && childPid_ > 0) {
            (void)::kill(-childPid_, SIGTERM);
            for (int i = 0; i < 100; ++i) {
                reapChildNoHang();
                if (exited_) {
                    break;
                }
                usleep(1000);
            }
            if (!exited_) {
                (void)::kill(-childPid_, SIGKILL);
                int status = 0;
                while (true) {
                    const pid_t waited = ::waitpid(childPid_, &status, 0);
                    if (waited == childPid_) {
                        exited_ = true;
                        exitCode_ = decodeExitCode(status);
                        break;
                    }
                    if (waited < 0 && errno == EINTR) {
                        continue;
                    }
                    break;
                }
            }
        }

        closeOutputFd(stdoutFd_, stdoutOpen_);
        closeOutputFd(stderrFd_, stderrOpen_);
    }

    static void closeOutputFd(int& fd, bool& isOpen) {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        isOpen = false;
    }

    pid_t childPid_;
    std::chrono::steady_clock::time_point startedAt_;
    std::optional<int64_t> maxOutputBytes_;
    std::optional<int64_t> timeoutNanos_;
    int stdoutFd_;
    int stderrFd_;
    int stdinFd_;
    bool stdoutOpen_;
    bool stderrOpen_;
    bool stdinOpen_;
    bool exited_;
    int32_t exitCode_;
};
