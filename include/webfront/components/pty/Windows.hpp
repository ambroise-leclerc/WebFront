// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-19
//
// WebFront — components/pty
// Role: Windows ConPTY pseudoterminal backend: pseudoconsole, child launch, resize and cleanup.
// Thread: worker-safe | Alloc: unrestricted
// LLM: optional component; handles are owned by PtyHandle, mirror Unix.hpp behavior

#pragma once

#include "../../automation/Credentials.hpp"
#include "../../system/WindowsArguments.hpp"
#include "../../system/WindowsCompat.hpp"
#include "Options.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace webfront::pty::detail {

class PtyHandle {
public:
    ~PtyHandle() {
        reset();
    }
    PtyHandle()                            = default;
    PtyHandle(const PtyHandle&)            = delete;
    PtyHandle& operator=(const PtyHandle&) = delete;

    HANDLE value = nullptr;

    explicit operator bool() const {
        return value && value != INVALID_HANDLE_VALUE;
    }
    void reset() {
        if (*this)
            ::CloseHandle(value);
        value = nullptr;
    }
};

class NativePty {
public:
    explicit NativePty(const PtyOptions& options) {
        // Resolve at runtime so a program that only conditionally offers PTYs can still load on older Windows.
        const auto kernel        = ::GetModuleHandleW(L"kernel32.dll");
        const auto createConsole = load<HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HANDLE*)>(kernel, "CreatePseudoConsole");
        resizeConsole            = load<HRESULT(WINAPI*)(HANDLE, COORD)>(kernel, "ResizePseudoConsole");
        closeConsole             = load<void(WINAPI*)(HANDLE)>(kernel, "ClosePseudoConsole");

        auto [program, command] = commandLine(options);
        auto      environment   = environmentBlock(options);
        PtyHandle consoleInput, consoleOutput;
        createInputPipe(consoleInput);
        if (!::CreatePipe(&output.value, &consoleOutput.value, nullptr, 0))
            fail("create ConPTY output pipe");
        writeEvent.value = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!writeEvent)
            fail("create ConPTY write event");
        pending.hEvent = writeEvent.value;
        job.value      = ::CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            fail("create ConPTY job");

        try {
            check(createConsole(dimensions(options.size), consoleInput.value, consoleOutput.value, 0, &console), "create ConPTY");
            consoleInput.reset();
            consoleOutput.reset();

            SIZE_T size = 0;
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            std::vector<std::max_align_t> storage((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
            // The analyzer follows data() of the vector to a null pointer: test the pointer itself, not the size (C6387).
            const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (attributes == nullptr)
                fail("size ConPTY process attributes");
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            // Null standard handles let ConPTY install its own console handles even if the host is redirected.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.lpAttributeList     = attributes;
            if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size))
                fail("initialize ConPTY process attributes");
            struct DeleteAttributes {
                LPPROC_THREAD_ATTRIBUTE_LIST attributes;
                ~DeleteAttributes() {
                    ::DeleteProcThreadAttributeList(attributes);
                }
            } cleanup{attributes};

            // ProcThreadAttributeValue(22, FALSE, TRUE, FALSE), also with older SDK headers.
            constexpr DWORD_PTR pseudoConsoleAttribute = 0x00020016;
            if (!::UpdateProcThreadAttribute(attributes, 0, pseudoConsoleAttribute, console, sizeof(console), nullptr, nullptr))
                fail("attach ConPTY process attributes");

            PROCESS_INFORMATION info{};
            if (!::CreateProcessW(program.c_str(),
                                  command.data(),
                                  nullptr,
                                  nullptr,
                                  FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                  environment.data(),
                                  options.directory.c_str(),
                                  &startup.StartupInfo,
                                  &info))
                fail("start ConPTY child");
            process.value = info.hProcess;
            PtyHandle thread;
            thread.value = info.hThread;
            if (!::AssignProcessToJobObject(job.value, process.value) || ::ResumeThread(thread.value) == static_cast<DWORD>(-1))
                fail("start ConPTY child in its job");
        } catch (...) {
            consoleInput.reset();
            consoleOutput.reset();
            finish();
            throw;
        }
    }

    ~NativePty() {
        finish();
    }

    std::size_t read(std::span<std::uint8_t> bytes) {
        if (draining && console)
            closeWhenJobIsEmpty();
        DWORD available = 0;
        DWORD count     = 0;
        if (!::PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) {
            if (!outputPipeClosed(::GetLastError()))
                fail("read ConPTY output");
            outputClosed = true;
        } else if (available && !::ReadFile(output.value, bytes.data(), std::min(available, static_cast<DWORD>(bytes.size())), &count, nullptr)) {
            if (!outputPipeClosed(::GetLastError()))
                fail("read ConPTY output");
            outputClosed = true;
        }
        return count;
    }

    bool atEnd() const {
        return outputClosed;
    }

    void beginDrain() {
        draining = true;
    }

    // The overlapped operation borrows the backend's own buffer, including across worker iterations.
    std::size_t write(std::span<const std::uint8_t> bytes) {
        if (writing) {
            DWORD count = 0;
            if (!::GetOverlappedResult(input.value, &pending, &count, FALSE)) {
                if (::GetLastError() == ERROR_IO_INCOMPLETE)
                    return 0;
                writing = false;
                input.reset();
                return 0;
            }
            writing = false;
            return count;
        }
        if (!input)
            return 0;
        const auto size = std::min(bytes.size(), writeBuffer.size());
        std::copy_n(bytes.data(), size, writeBuffer.data());
        DWORD count = 0;
        ::ResetEvent(writeEvent.value);
        if (::WriteFile(input.value, writeBuffer.data(), static_cast<DWORD>(size), &count, &pending))
            return count;
        writing = ::GetLastError() == ERROR_IO_PENDING;
        if (!writing)
            input.reset();
        return 0;
    }

    void resize(PtySize size) {
        check(resizeConsole(console, dimensions(size)), "resize ConPTY");
    }

    bool exited() const {
        return ::WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
    }

    int finish() {
        if (input && writing) {
            ::CancelIoEx(input.value, &pending);
            DWORD count = 0;
            ::GetOverlappedResult(input.value, &pending, &count, TRUE);
        }
        input.reset();
        writeEvent.reset();
        if (job)
            ::TerminateJobObject(job.value, 1);
        if (process) {
            // Covers constructor failure before the suspended process was assigned to the job.
            if (!exited())
                ::TerminateProcess(process.value, 1);
            ::WaitForSingleObject(process.value, INFINITE);
            DWORD status = 0;
            if (::GetExitCodeProcess(process.value, &status))
                exitCode = static_cast<int>(status);
            process.reset();
        }
        // Older versions of ConPTY wait for their final output during ClosePseudoConsole.
        output.reset();
        if (closer.joinable())
            closer.join();
        if (console)
            closeConsole(console);
        console = nullptr;
        job.reset();
        return exitCode;
    }

private:
    PtyHandle    input, output, job, process, writeEvent;
    std::jthread closer;
    bool         draining                         = false;
    bool         outputClosed                     = false;
    HANDLE       console                          = nullptr;
    HRESULT(WINAPI* resizeConsole)(HANDLE, COORD) = nullptr;
    void(WINAPI* closeConsole)(HANDLE)            = nullptr;
    OVERLAPPED                     pending{};
    std::array<std::uint8_t, 4096> writeBuffer{};
    bool                           writing  = false;
    int                            exitCode = 255;

    [[noreturn]] static void fail(const char* action) {
        throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(), action);
    }

    static void check(HRESULT result, const char* action) {
        if (FAILED(result))
            throw std::system_error(static_cast<int>(result), std::system_category(), action);
    }

    template <typename Function>
    static Function load(HMODULE module, const char* name) {
        const auto address = ::GetProcAddress(module, name);
        if (!address)
            fail(name);
        Function function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }

    void closeWhenJobIsEmpty() {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!::QueryInformationJobObject(job.value, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
            fail("query ConPTY job");
        if (accounting.ActiveProcesses)
            return;
        // Older versions can block in ClosePseudoConsole while the output pipe still has bytes.
        closer  = std::jthread([close = closeConsole, handle = console] {
            close(handle);
        });
        console = nullptr;
    }

    static bool outputPipeClosed(DWORD error) {
        return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
    }

    static COORD dimensions(PtySize size) {
        return {static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows)};
    }

    void createInputPipe(PtyHandle& reader) {
        const auto name = L"\\\\.\\pipe\\webfront-pty-" + system::detail::toWide(automation::channel::nonce());
        input.value     = ::CreateNamedPipeW(name.c_str(),
                                         PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                         1,
                                         4096,
                                         0,
                                         0,
                                         nullptr);
        if (!input)
            fail("create ConPTY input pipe");
        reader.value = ::CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (!reader)
            fail("open ConPTY input pipe");
        ULONG peer = 0;
        if (!::GetNamedPipeClientProcessId(input.value, &peer))
            fail("identify ConPTY input peer");
        if (peer != ::GetCurrentProcessId())
            throw std::runtime_error("Unexpected ConPTY input pipe peer");
    }

    static std::filesystem::path systemDirectory() {
        std::array<wchar_t, MAX_PATH> directory{};
        const auto                    count = ::GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (!count || count >= directory.size())
            fail("resolve Windows system directory");
        return directory.data();
    }

    using WindowsEnvironment = std::map<std::wstring, std::wstring, decltype([](const std::wstring& a, const std::wstring& b) {
                                            return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
                                        })>;

    static WindowsEnvironment variables(const PtyOptions& options) {
        WindowsEnvironment result;
        for (const auto& [key, value] : options.environment)
            result[system::detail::toWide(key)] = system::detail::toWide(value);
        return result;
    }

    static std::wstring environmentBlock(const PtyOptions& options) {
        auto entries = variables(options);
        entries.try_emplace(L"SystemRoot", systemDirectory().parent_path().native());
        std::wstring block;
        for (const auto& [key, value] : entries)
            block += key + L'=' + value + L'\0';
        return block + L'\0';
    }

    static std::filesystem::path resolve(const PtyOptions& options) {
        const std::filesystem::path        name = system::detail::toWide(options.argv.front());
        std::vector<std::filesystem::path> directories;
        if (name.has_parent_path() || name.has_root_path()) {
            directories.emplace_back();
        } else {
            const auto entries = variables(options);
            const auto path    = entries.find(L"PATH");
            if (path == entries.end()) {
                directories.push_back(systemDirectory());
            } else {
                std::wstring_view remaining = path->second;
                for (;;) {
                    const auto separator = remaining.find(L';');
                    directories.emplace_back(remaining.substr(0, separator));
                    if (separator == std::wstring_view::npos)
                        break;
                    remaining.remove_prefix(separator + 1);
                }
            }
        }
        for (const auto& directory : directories) {
            auto candidate = options.directory / directory / name;
            if (!candidate.is_absolute())
                candidate = std::filesystem::absolute(candidate);
            std::error_code error;
            if (name.has_extension()) {
                if (std::filesystem::is_regular_file(candidate, error))
                    return candidate;
            } else {
                for (const auto* extension : {L".exe", L".com", L".cmd", L".bat"})
                    if (std::filesystem::is_regular_file(candidate.native() + extension, error))
                        return candidate.native() + extension;
            }
        }
        throw std::system_error(ERROR_FILE_NOT_FOUND, std::system_category(), "resolve ConPTY executable: " + options.argv.front());
    }

    static std::pair<std::filesystem::path, std::wstring> commandLine(const PtyOptions& options) {
        auto       program   = resolve(options);
        const auto extension = program.extension().native();
        const bool batch     = ::CompareStringOrdinal(extension.c_str(), -1, L".cmd", -1, TRUE) == CSTR_EQUAL
                           || ::CompareStringOrdinal(extension.c_str(), -1, L".bat", -1, TRUE) == CSTR_EQUAL;
        if (batch) {
            // cmd.exe expands percent references even inside quotes. Keep its arguments in a literal subset.
            const auto literal = [](const std::wstring& argument) {
                if (argument.find_first_of(L"\"%\r\n") != std::wstring::npos)
                    throw std::invalid_argument("Unsupported character in ConPTY batch argument");
                return L'"' + argument + L'"';
            };
            auto interpreter = systemDirectory() / L"cmd.exe";
            auto command     = system::detail::quoteWindowsArgument(interpreter.native()) + L" /d /v:off /s /c \"" + literal(program.native());
            for (std::size_t i = 1; i < options.argv.size(); ++i)
                command += L' ' + literal(system::detail::toWide(options.argv[i]));
            return {interpreter, command + L'"'};
        }
        auto command = system::detail::quoteWindowsArgument(program.native());
        for (std::size_t i = 1; i < options.argv.size(); ++i)
            command += L' ' + system::detail::quoteWindowsArgument(system::detail::toWide(options.argv[i]));
        return {program, command};
    }
};
}  // namespace webfront::pty::detail
