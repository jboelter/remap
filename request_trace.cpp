#include <windows.h>

#include "version_info.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef HPCON
using HPCON = HANDLE;
#endif

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace
{
    enum class ParseResult
    {
        Success,
        Help,
        Version,
        Error
    };

    struct UniqueHandle
    {
        HANDLE value{ INVALID_HANDLE_VALUE };

        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) noexcept : value(handle) {}

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : value(other.release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        ~UniqueHandle()
        {
            reset();
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return value != nullptr && value != INVALID_HANDLE_VALUE;
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return value;
        }

        [[nodiscard]] HANDLE release() noexcept
        {
            const auto handle = value;
            value = INVALID_HANDLE_VALUE;
            return handle;
        }

        void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        {
            if (valid())
            {
                CloseHandle(value);
            }
            value = handle;
        }
    };

    struct UniquePseudoConsole
    {
        HPCON value{ nullptr };

        UniquePseudoConsole() = default;
        explicit UniquePseudoConsole(HPCON handle) noexcept : value(handle) {}

        UniquePseudoConsole(const UniquePseudoConsole&) = delete;
        UniquePseudoConsole& operator=(const UniquePseudoConsole&) = delete;

        UniquePseudoConsole(UniquePseudoConsole&& other) noexcept : value(other.release()) {}
        UniquePseudoConsole& operator=(UniquePseudoConsole&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        ~UniquePseudoConsole()
        {
            reset();
        }

        [[nodiscard]] HPCON get() const noexcept
        {
            return value;
        }

        [[nodiscard]] HPCON release() noexcept
        {
            const auto handle = value;
            value = nullptr;
            return handle;
        }

        void reset(HPCON handle = nullptr) noexcept
        {
            if (value != nullptr)
            {
                ClosePseudoConsole(value);
            }
            value = handle;
        }
    };

    struct ConsoleModeGuard
    {
        HANDLE handle{ INVALID_HANDLE_VALUE };
        DWORD originalMode{ 0 };
        bool restore{ false };

        explicit ConsoleModeGuard(HANDLE consoleHandle)
            : handle(consoleHandle)
        {
            if (GetConsoleMode(handle, &originalMode))
            {
                restore = true;
            }
        }

        ConsoleModeGuard(const ConsoleModeGuard&) = delete;
        ConsoleModeGuard& operator=(const ConsoleModeGuard&) = delete;

        ~ConsoleModeGuard()
        {
            if (restore)
            {
                SetConsoleMode(handle, originalMode);
            }
        }
    };

    struct CodePageGuard
    {
        UINT inputCodePage{ 0 };
        UINT outputCodePage{ 0 };
        bool restoreInput{ false };
        bool restoreOutput{ false };

        CodePageGuard()
        {
            inputCodePage = GetConsoleCP();
            outputCodePage = GetConsoleOutputCP();
            restoreInput = inputCodePage != 0;
            restoreOutput = outputCodePage != 0;
        }

        CodePageGuard(const CodePageGuard&) = delete;
        CodePageGuard& operator=(const CodePageGuard&) = delete;

        ~CodePageGuard()
        {
            if (restoreInput)
            {
                SetConsoleCP(inputCodePage);
            }
            if (restoreOutput)
            {
                SetConsoleOutputCP(outputCodePage);
            }
        }
    };

    struct ProcessHandles
    {
        UniqueHandle process;
        UniqueHandle thread;
    };

    struct Session
    {
        UniquePseudoConsole pseudoConsole;
        UniqueHandle inputWrite;
        UniqueHandle outputRead;
        ProcessHandles child;
    };

    struct Config
    {
        std::optional<std::wstring> childCommand;
        std::optional<std::wstring> workingDirectory;
        std::wstring logPath{ L"request-trace.log" };
    };

    [[nodiscard]] std::wstring GetLastErrorMessage(DWORD error)
    {
        LPWSTR buffer = nullptr;
        const auto size = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message;
        if (size != 0 && buffer != nullptr)
        {
            message.assign(buffer, size);
            LocalFree(buffer);
        }
        else
        {
            message = L"unknown error";
        }

        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'))
        {
            message.pop_back();
        }
        return message;
    }

    [[nodiscard]] std::wstring HResultMessage(HRESULT hr)
    {
        return GetLastErrorMessage(static_cast<DWORD>(hr));
    }

    void PrintUsage()
    {
        std::wcout
            << L"request-trace [--cwd <path>] [--log <path>] -- <command ...>\n\n"
            << L"Runs a child behind an inner ConPTY and logs private/input mode requests\n"
            << L"seen on the child output stream while forwarding normal output.\n";
    }

    void PrintVersion(std::wstring_view programName)
    {
        std::wcout << programName << L" " << remap::version::kTagWide << L"\n";
    }

    [[nodiscard]] std::wstring QuoteIfNeeded(std::wstring_view value)
    {
        if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring_view::npos)
        {
            return std::wstring(value);
        }

        std::wstring quoted;
        quoted.reserve((value.size() * 2) + 2);
        quoted.push_back(L'"');

        size_t backslashCount = 0;
        for (const auto ch : value)
        {
            if (ch == L'\\')
            {
                ++backslashCount;
                continue;
            }

            if (ch == L'"')
            {
                quoted.append(backslashCount * 2 + 1, L'\\');
                quoted.push_back(L'"');
                backslashCount = 0;
                continue;
            }

            quoted.append(backslashCount, L'\\');
            backslashCount = 0;
            quoted.push_back(ch);
        }

        quoted.append(backslashCount * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    [[nodiscard]] std::wstring JoinCommandLine(int argc, wchar_t** argv, int startIndex)
    {
        std::wstring commandLine;
        for (int i = startIndex; i < argc; ++i)
        {
            if (!commandLine.empty())
            {
                commandLine.push_back(L' ');
            }
            commandLine += QuoteIfNeeded(argv[i]);
        }
        return commandLine;
    }

    [[nodiscard]] ParseResult ParseArgs(int argc, wchar_t** argv, Config& config)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg{ argv[i] };
            if (arg == L"--help" || arg == L"-h" || arg == L"/?")
            {
                PrintUsage();
                return ParseResult::Help;
            }

            if (arg == L"--version")
            {
                PrintVersion(L"request-trace");
                return ParseResult::Version;
            }

            if (arg == L"--cwd")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"--cwd requires a value.\n";
                    return ParseResult::Error;
                }
                config.workingDirectory = argv[++i];
                continue;
            }

            if (arg == L"--log")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"--log requires a value.\n";
                    return ParseResult::Error;
                }
                config.logPath = argv[++i];
                continue;
            }

            if (arg == L"--")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"-- requires a child command.\n";
                    return ParseResult::Error;
                }
                config.childCommand = JoinCommandLine(argc, argv, i + 1);
                return ParseResult::Success;
            }

            std::wcerr << L"Unknown argument: " << arg << L"\n";
            return ParseResult::Error;
        }

        std::wcerr << L"A child command is required after `--`.\n";
        return ParseResult::Error;
    }

    [[nodiscard]] bool EnableVtOutput(HANDLE output)
    {
        DWORD mode = 0;
        if (!GetConsoleMode(output, &mode))
        {
            return false;
        }

        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        mode |= DISABLE_NEWLINE_AUTO_RETURN;
        return SetConsoleMode(output, mode) != FALSE;
    }

    [[nodiscard]] bool ConfigureInputMode(HANDLE input)
    {
        DWORD mode = 0;
        if (!GetConsoleMode(input, &mode))
        {
            return false;
        }

        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        mode |= ENABLE_WINDOW_INPUT;
        return SetConsoleMode(input, mode) != FALSE;
    }

    [[nodiscard]] COORD GetConsoleSize(HANDLE output)
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(output, &info))
        {
            return {
                static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1),
                static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1)
            };
        }

        return { 120, 30 };
    }

    [[nodiscard]] bool WriteAll(HANDLE handle, const void* buffer, DWORD length)
    {
        const auto* bytes = static_cast<const std::byte*>(buffer);
        DWORD totalWritten = 0;

        while (totalWritten < length)
        {
            DWORD written = 0;
            if (!WriteFile(handle, bytes + totalWritten, length - totalWritten, &written, nullptr))
            {
                return false;
            }
            totalWritten += written;
        }

        return true;
    }

    [[nodiscard]] bool WriteString(HANDLE handle, const std::string& data)
    {
        return data.empty() || WriteAll(handle, data.data(), static_cast<DWORD>(data.size()));
    }

    [[nodiscard]] std::string Utf8FromWide(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const auto size = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (size <= 0)
        {
            return {};
        }

        std::string result(size, '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            size,
            nullptr,
            nullptr);
        return result;
    }

    [[nodiscard]] UniqueHandle OpenLogFile(std::wstring_view path)
    {
        return UniqueHandle(CreateFileW(
            std::wstring(path).c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
    }

    void AppendWin32InputModeSequence(std::string& payload, const KEY_EVENT_RECORD& key)
    {
        payload += "\x1b[";
        payload += std::to_string(static_cast<unsigned int>(key.wVirtualKeyCode));
        payload.push_back(';');
        payload += std::to_string(static_cast<unsigned int>(key.wVirtualScanCode));
        payload.push_back(';');
        payload += std::to_string(static_cast<unsigned int>(key.uChar.UnicodeChar));
        payload.push_back(';');
        payload += key.bKeyDown ? '1' : '0';
        payload.push_back(';');
        payload += std::to_string(static_cast<unsigned int>(key.dwControlKeyState));
        payload.push_back(';');
        payload += std::to_string(static_cast<unsigned int>(key.wRepeatCount));
        payload.push_back('_');
    }

    [[nodiscard]] std::string SerializeKeyEvent(const KEY_EVENT_RECORD& key)
    {
        std::string payload;
        AppendWin32InputModeSequence(payload, key);
        return payload;
    }

    [[nodiscard]] bool CreatePipes(UniqueHandle& inputRead, UniqueHandle& inputWrite, UniqueHandle& outputRead, UniqueHandle& outputWrite)
    {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;

        HANDLE rawInputRead = INVALID_HANDLE_VALUE;
        HANDLE rawInputWrite = INVALID_HANDLE_VALUE;
        HANDLE rawOutputRead = INVALID_HANDLE_VALUE;
        HANDLE rawOutputWrite = INVALID_HANDLE_VALUE;

        if (!CreatePipe(&rawInputRead, &rawInputWrite, &sa, 0))
        {
            return false;
        }

        if (!CreatePipe(&rawOutputRead, &rawOutputWrite, &sa, 0))
        {
            CloseHandle(rawInputRead);
            CloseHandle(rawInputWrite);
            return false;
        }

        inputRead.reset(rawInputRead);
        inputWrite.reset(rawInputWrite);
        outputRead.reset(rawOutputRead);
        outputWrite.reset(rawOutputWrite);
        return true;
    }

    [[nodiscard]] bool StartSession(const Config& config, HANDLE consoleOutput, Session& session)
    {
        UniqueHandle inputRead;
        UniqueHandle inputWrite;
        UniqueHandle outputRead;
        UniqueHandle outputWrite;

        if (!CreatePipes(inputRead, inputWrite, outputRead, outputWrite))
        {
            std::wcerr << L"CreatePipe failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
            return false;
        }

        const auto size = GetConsoleSize(consoleOutput);

        HPCON pseudoConsole = nullptr;
        const auto hr = CreatePseudoConsole(size, inputRead.get(), outputWrite.get(), 0, &pseudoConsole);
        if (FAILED(hr))
        {
            std::wcerr << L"CreatePseudoConsole failed: " << HResultMessage(hr) << L"\n";
            return false;
        }

        session.pseudoConsole.reset(pseudoConsole);

        SIZE_T attributeListSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeListSize);
        std::vector<std::byte> attributeListStorage(attributeListSize);
        auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeListStorage.data());

        if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeListSize))
        {
            std::wcerr << L"InitializeProcThreadAttributeList failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
            return false;
        }

        auto cleanupAttributes = [&]() {
            DeleteProcThreadAttributeList(attributeList);
        };

        if (!UpdateProcThreadAttribute(
                attributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                session.pseudoConsole.get(),
                sizeof(HPCON),
                nullptr,
                nullptr))
        {
            std::wcerr << L"UpdateProcThreadAttribute failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
            cleanupAttributes();
            return false;
        }

        STARTUPINFOEXW startupInfo{};
        startupInfo.StartupInfo.cb = sizeof(startupInfo);
        startupInfo.lpAttributeList = attributeList;

        std::wstring childCommand = *config.childCommand;
        std::vector<wchar_t> commandLine(childCommand.begin(), childCommand.end());
        commandLine.push_back(L'\0');

        PROCESS_INFORMATION processInfo{};
        const auto currentDirectory = config.workingDirectory ? config.workingDirectory->c_str() : nullptr;

        const BOOL created = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            currentDirectory,
            &startupInfo.StartupInfo,
            &processInfo);

        cleanupAttributes();

        if (!created)
        {
            std::wcerr << L"CreateProcessW failed for '" << *config.childCommand << L"': " << GetLastErrorMessage(GetLastError()) << L"\n";
            return false;
        }

        session.child.process.reset(processInfo.hProcess);
        session.child.thread.reset(processInfo.hThread);
        session.inputWrite = std::move(inputWrite);
        session.outputRead = std::move(outputRead);

        inputRead.reset();
        outputWrite.reset();
        return true;
    }

    [[nodiscard]] bool IsCsiFinalByte(char ch)
    {
        return ch >= 0x40 && ch <= 0x7e;
    }

    [[nodiscard]] std::wstring Utf16FromUtf8(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }

        const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return {};
        }

        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    [[nodiscard]] std::wstring DescribePrivateMode(std::string_view parameter)
    {
        if (parameter == "1")
        {
            return L"cursor keys application mode";
        }
        if (parameter == "9")
        {
            return L"X10 mouse tracking";
        }
        if (parameter == "1000")
        {
            return L"VT200 mouse tracking";
        }
        if (parameter == "1001")
        {
            return L"highlight mouse tracking";
        }
        if (parameter == "1002")
        {
            return L"button-event mouse tracking";
        }
        if (parameter == "1003")
        {
            return L"any-event mouse tracking";
        }
        if (parameter == "1004")
        {
            return L"focus event mode";
        }
        if (parameter == "1005")
        {
            return L"UTF-8 mouse encoding";
        }
        if (parameter == "1006")
        {
            return L"SGR mouse encoding";
        }
        if (parameter == "1015")
        {
            return L"urxvt mouse encoding";
        }
        if (parameter == "1016")
        {
            return L"pixel mouse encoding";
        }
        if (parameter == "1049")
        {
            return L"alternate screen buffer";
        }
        if (parameter == "2004")
        {
            return L"bracketed paste";
        }
        if (parameter == "9001")
        {
            return L"win32-input-mode";
        }
        return L"unknown private mode";
    }

    void LogTraceLine(HANDLE logFile, std::wstring_view line)
    {
        std::wstring prefixed = L"[request-trace] ";
        prefixed.append(line);
        prefixed.append(L"\r\n");
        const auto utf8 = Utf8FromWide(prefixed);
        if (!WriteString(logFile, utf8))
        {
            std::wcerr << L"Failed to write to trace log.\n";
        }
    }

    void LogPrivateModeSequence(HANDLE logFile, std::string_view sequence)
    {
        if (sequence.size() < 5 || sequence[0] != '\x1b' || sequence[1] != '[' || sequence[2] != '?')
        {
            return;
        }

        const char finalByte = sequence.back();
        const std::wstring action = finalByte == 'h' ? L"enable" : finalByte == 'l' ? L"disable" : L"set";
        const auto parameters = sequence.substr(3, sequence.size() - 4);

        size_t offset = 0;
        while (offset <= parameters.size())
        {
            const auto separator = parameters.find(';', offset);
            const auto length = separator == std::string_view::npos ? parameters.size() - offset : separator - offset;
            const auto parameter = parameters.substr(offset, length);

            if (!parameter.empty())
            {
                const auto paramWide = Utf16FromUtf8(parameter);
                LogTraceLine(logFile, action + L" ?" + paramWide + L" (" + DescribePrivateMode(parameter) + L")");
            }

            if (separator == std::string_view::npos)
            {
                break;
            }

            offset = separator + 1;
        }
    }

    void LogKittyKeyboardSequence(HANDLE logFile, std::string_view sequence)
    {
        if (sequence.size() < 4 || sequence[0] != '\x1b' || sequence[1] != '[' || sequence.back() != 'u')
        {
            return;
        }

        const char selector = sequence[2];
        const auto parameters = Utf16FromUtf8(sequence.substr(3, sequence.size() - 4));

        switch (selector)
        {
        case '>':
            LogTraceLine(logFile, L"kitty keyboard push/set > " + parameters + L"u");
            break;
        case '<':
            LogTraceLine(logFile, L"kitty keyboard pop < " + parameters + L"u");
            break;
        case '=':
            LogTraceLine(logFile, L"kitty keyboard set = " + parameters + L"u");
            break;
        case '?':
            LogTraceLine(logFile, L"kitty keyboard query ?u");
            break;
        default:
            break;
        }
    }

    struct TraceFilter
    {
        HANDLE logFile{ INVALID_HANDLE_VALUE };
        std::string pendingSequence;
        bool sawEscape{ false };
        bool bufferingCsi{ false };

        explicit TraceFilter(HANDLE file) noexcept
            : logFile(file)
        {
        }

        void HandleCompletedCsi(std::string_view csi)
        {
            if (csi.size() >= 4 && csi[0] == '\x1b' && csi[1] == '[')
            {
                if (csi[2] == '?')
                {
                    LogPrivateModeSequence(logFile, csi);
                }
                else if ((csi[2] == '>' || csi[2] == '<' || csi[2] == '=' || csi[2] == '?') && csi.back() == 'u')
                {
                    LogKittyKeyboardSequence(logFile, csi);
                }
            }
        }

        void Inspect(std::string_view chunk)
        {
            for (const char ch : chunk)
            {
                if (bufferingCsi)
                {
                    pendingSequence.push_back(ch);
                    if (IsCsiFinalByte(ch))
                    {
                        HandleCompletedCsi(pendingSequence);
                        pendingSequence.clear();
                        bufferingCsi = false;
                        sawEscape = false;
                    }
                    continue;
                }

                if (sawEscape)
                {
                    if (ch == '[')
                    {
                        pendingSequence.push_back(ch);
                        bufferingCsi = true;
                        continue;
                    }

                    pendingSequence.clear();
                    sawEscape = false;
                }

                if (ch == '\x1b')
                {
                    pendingSequence.assign(1, ch);
                    sawEscape = true;
                }
            }
        }
    };

    void PumpOutput(HANDLE outputRead, HANDLE consoleOutput, HANDLE logFile, std::atomic<bool>& running)
    {
        std::vector<std::byte> buffer(8192);
        TraceFilter tracer(logFile);
        while (running.load())
        {
            DWORD bytesRead = 0;
            if (!ReadFile(outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
            {
                running.store(false);
                break;
            }

            const auto chunk = std::string_view(reinterpret_cast<const char*>(buffer.data()), bytesRead);
            tracer.Inspect(chunk);
            if (!WriteAll(consoleOutput, buffer.data(), bytesRead))
            {
                running.store(false);
                break;
            }
        }
    }

    void HandleResize(const INPUT_RECORD& record, const UniquePseudoConsole& pseudoConsole)
    {
        if (record.EventType != WINDOW_BUFFER_SIZE_EVENT || pseudoConsole.get() == nullptr)
        {
            return;
        }

        ResizePseudoConsole(pseudoConsole.get(), record.Event.WindowBufferSizeEvent.dwSize);
    }
}

int wmain(int argc, wchar_t** argv)
{
    Config config;
    switch (ParseArgs(argc, argv, config))
    {
    case ParseResult::Success:
        break;
    case ParseResult::Help:
        return 0;
    case ParseResult::Version:
        return 0;
    case ParseResult::Error:
        return 1;
    }

    UniqueHandle consoleInput(GetStdHandle(STD_INPUT_HANDLE));
    UniqueHandle consoleOutput(GetStdHandle(STD_OUTPUT_HANDLE));

    if (!consoleInput.valid() || !consoleOutput.valid())
    {
        std::wcerr << L"Failed to acquire standard console handles.\n";
        return 1;
    }

    ConsoleModeGuard inputModeGuard(consoleInput.get());
    ConsoleModeGuard outputModeGuard(consoleOutput.get());
    CodePageGuard codePageGuard;

    if (!SetConsoleCP(CP_UTF8))
    {
        std::wcerr << L"Failed to set console input code page to UTF-8.\n";
        return 1;
    }

    if (!SetConsoleOutputCP(CP_UTF8))
    {
        std::wcerr << L"Failed to set console output code page to UTF-8.\n";
        return 1;
    }

    if (!EnableVtOutput(consoleOutput.get()))
    {
        std::wcerr << L"Failed to enable VT processing on stdout.\n";
        return 1;
    }

    if (!ConfigureInputMode(consoleInput.get()))
    {
        std::wcerr << L"Failed to configure console input mode.\n";
        return 1;
    }

    UniqueHandle logFile = OpenLogFile(config.logPath);
    if (!logFile.valid())
    {
        std::wcerr << L"Failed to open log file '" << config.logPath << L"': " << GetLastErrorMessage(GetLastError()) << L"\n";
        return 1;
    }

    Session session;
    if (!StartSession(config, consoleOutput.get(), session))
    {
        return 1;
    }

    std::atomic<bool> running{ true };
    std::thread outputThread(PumpOutput, session.outputRead.get(), consoleOutput.get(), logFile.get(), std::ref(running));

    while (running.load())
    {
        if (WaitForSingleObject(session.child.process.get(), 0) == WAIT_OBJECT_0)
        {
            running.store(false);
            break;
        }

        DWORD pendingEvents = 0;
        if (!GetNumberOfConsoleInputEvents(consoleInput.get(), &pendingEvents))
        {
            std::wcerr << L"GetNumberOfConsoleInputEvents failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
            running.store(false);
            break;
        }

        if (pendingEvents == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::vector<INPUT_RECORD> records(std::min<DWORD>(pendingEvents, 16));
        DWORD recordsRead = 0;
        if (!ReadConsoleInputW(consoleInput.get(), records.data(), static_cast<DWORD>(records.size()), &recordsRead))
        {
            std::wcerr << L"ReadConsoleInputW failed: " << GetLastErrorMessage(GetLastError()) << L"\n";
            running.store(false);
            break;
        }

        for (DWORD i = 0; i < recordsRead; ++i)
        {
            const auto& record = records[i];
            HandleResize(record, session.pseudoConsole);

            if (record.EventType != KEY_EVENT)
            {
                continue;
            }

            const auto payload = SerializeKeyEvent(record.Event.KeyEvent);
            if (!WriteString(session.inputWrite.get(), payload))
            {
                std::wcerr << L"Failed to write to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
                running.store(false);
                break;
            }
        }
    }

    session.inputWrite.reset();
    session.pseudoConsole.reset();

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(session.child.process.get(), &exitCode))
    {
        exitCode = 1;
    }

    if (outputThread.joinable())
    {
        outputThread.join();
    }

    return static_cast<int>(exitCode);
}
