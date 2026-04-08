#include <windows.h>

#include "version_info.h"

#include <algorithm>
#include <array>
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
    constexpr auto kDefaultDoubleTapDelay = std::chrono::milliseconds(150);

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

        ConsoleModeGuard() = default;

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
        bool swapEnter = true;
        bool doubleTapEnter = false;
        std::optional<std::chrono::milliseconds> delayWindow;
        std::optional<std::wstring> childCommand;
        std::optional<std::wstring> workingDirectory;
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
            << L"remap [options]\n\n"
            << L"Launches a child command inside an inner ConPTY and remaps Enter keys only for this session.\n\n"
            << L"Options:\n"
            << L"  --swap-enter        Physical Enter -> ESC CR, Shift+Enter -> CR (default)\n"
            << L"  --standard-enter    Physical Enter -> CR, Shift+Enter -> ESC CR\n"
            << L"  --double-tap        Enable plain-Enter double-tap mode (default window: 150ms)\n"
            << L"  --delay <ms>        Set the double-tap window in milliseconds (requires --double-tap)\n"
            << L"  --cwd <path>        Working directory for the child process\n"
            << L"  -- <command ...>    Required. Treat the remaining tokens as the full child command line\n"
            << L"  --version           Show version info\n"
            << L"  --help              Show this help text\n";
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
                PrintVersion(L"remap");
                return ParseResult::Version;
            }

            if (arg == L"--swap-enter")
            {
                config.swapEnter = true;
                continue;
            }

            if (arg == L"--standard-enter")
            {
                config.swapEnter = false;
                continue;
            }

            if (arg == L"--double-tap")
            {
                config.doubleTapEnter = true;
                continue;
            }

            if (arg == L"--delay")
            {
                if (i + 1 >= argc)
                {
                    std::wcerr << L"--delay requires a millisecond value.\n";
                    return ParseResult::Error;
                }

                try
                {
                    const auto value = std::stoi(argv[++i]);
                    if (value < 0)
                    {
                        std::wcerr << L"--delay must be >= 0.\n";
                        return ParseResult::Error;
                    }
                    config.delayWindow = std::chrono::milliseconds(value);
                }
                catch (const std::exception&)
                {
                    std::wcerr << L"Invalid --delay value.\n";
                    return ParseResult::Error;
                }
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
                break;
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

            std::wcerr << L"Unknown argument: " << arg << L"\n";
            return ParseResult::Error;
        }

        if (config.delayWindow.has_value() && !config.doubleTapEnter)
        {
            std::wcerr << L"--delay requires --double-tap.\n";
            return ParseResult::Error;
        }

        if (config.doubleTapEnter && !config.delayWindow.has_value())
        {
            config.delayWindow = kDefaultDoubleTapDelay;
        }

        return ParseResult::Success;
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

    [[nodiscard]] std::chrono::milliseconds PollIntervalUntil(
        const std::optional<std::chrono::steady_clock::time_point>& deadline,
        std::chrono::milliseconds maxInterval = std::chrono::milliseconds(10))
    {
        if (!deadline.has_value())
        {
            return maxInterval;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= *deadline)
        {
            return std::chrono::milliseconds(0);
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
        return std::min(maxInterval, std::max(std::chrono::milliseconds(1), remaining));
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

    [[nodiscard]] bool IsCsiFinalByte(char ch)
    {
        return ch >= 0x40 && ch <= 0x7e;
    }

    [[nodiscard]] bool IsMouseTrackingMode(std::string_view parameter)
    {
        static constexpr std::array<std::string_view, 9> kMouseTrackingModes{
            "9",
            "1000",
            "1001",
            "1002",
            "1003",
            "1005",
            "1006",
            "1015",
            "1016"
        };

        return std::find(kMouseTrackingModes.begin(), kMouseTrackingModes.end(), parameter) != kMouseTrackingModes.end();
    }

    [[nodiscard]] std::string FilterMouseTrackingMode(std::string_view csi)
    {
        if (csi.size() < 5 || csi[0] != '\x1b' || csi[1] != '[' || csi[2] != '?')
        {
            return std::string(csi);
        }

        const char finalByte = csi.back();
        if (finalByte != 'h' && finalByte != 'l')
        {
            return std::string(csi);
        }

        const auto parameters = csi.substr(3, csi.size() - 4);
        if (parameters.empty())
        {
            return std::string(csi);
        }

        std::string filtered;
        bool removedMouseMode = false;
        std::size_t offset = 0;
        while (offset <= parameters.size())
        {
            const auto separator = parameters.find(';', offset);
            const auto length = separator == std::string_view::npos ? parameters.size() - offset : separator - offset;
            const auto parameter = parameters.substr(offset, length);

            if (!parameter.empty() && !IsMouseTrackingMode(parameter))
            {
                if (!filtered.empty())
                {
                    filtered.push_back(';');
                }
                filtered.append(parameter);
            }
            else if (IsMouseTrackingMode(parameter))
            {
                removedMouseMode = true;
            }

            if (separator == std::string_view::npos)
            {
                break;
            }

            offset = separator + 1;
        }

        if (!removedMouseMode)
        {
            return std::string(csi);
        }

        if (filtered.empty())
        {
            return {};
        }

        std::string rebuilt = "\x1b[?";
        rebuilt += filtered;
        rebuilt.push_back(finalByte);
        return rebuilt;
    }

    struct VtOutputFilter
    {
        std::string pendingSequence;
        bool sawEscape{ false };
        bool bufferingCsi{ false };

        [[nodiscard]] std::string Filter(std::string_view chunk)
        {
            std::string output;
            output.reserve(chunk.size());

            for (const char ch : chunk)
            {
                if (bufferingCsi)
                {
                    pendingSequence.push_back(ch);
                    if (IsCsiFinalByte(ch))
                    {
                        output += FilterMouseTrackingMode(pendingSequence);
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

                    output += pendingSequence;
                    pendingSequence.clear();
                    sawEscape = false;
                }

                if (ch == '\x1b')
                {
                    pendingSequence.assign(1, ch);
                    sawEscape = true;
                    continue;
                }

                output.push_back(ch);
            }

            return output;
        }

        [[nodiscard]] std::string Flush()
        {
            std::string output;
            if (!pendingSequence.empty())
            {
                output.swap(pendingSequence);
            }
            bufferingCsi = false;
            sawEscape = false;
            return output;
        }
    };

    [[nodiscard]] std::string ResetMouseTrackingModes()
    {
        return "\x1b[?9l"
               "\x1b[?1000l"
               "\x1b[?1001l"
               "\x1b[?1002l"
               "\x1b[?1003l"
               "\x1b[?1005l"
               "\x1b[?1006l"
               "\x1b[?1015l"
               "\x1b[?1016l";
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

    [[nodiscard]] std::string Repeat(std::string_view payload, WORD count)
    {
        std::string result;
        result.reserve(payload.size() * std::max<WORD>(count, 1));
        for (WORD i = 0; i < std::max<WORD>(count, 1); ++i)
        {
            result.append(payload);
        }
        return result;
    }

    [[nodiscard]] std::string RawEnterPayload()
    {
        return "\r";
    }

    [[nodiscard]] std::string RemappedEnterPayload(const Config& config)
    {
        return config.swapEnter ? "\x1b\r" : "\r";
    }

    [[nodiscard]] bool IsPlainEnterCandidate(const KEY_EVENT_RECORD& key)
    {
        if (!key.bKeyDown || key.wVirtualKeyCode != VK_RETURN || key.wRepeatCount != 1)
        {
            return false;
        }

        const auto modifiers =
            key.dwControlKeyState & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        return modifiers == 0;
    }

    [[nodiscard]] std::optional<std::string> EncodeKey(const KEY_EVENT_RECORD& key, const Config& config)
    {
        if (!key.bKeyDown)
        {
            return std::nullopt;
        }

        const auto controlState = key.dwControlKeyState;
        const bool shift = (controlState & SHIFT_PRESSED) != 0;
        const bool ctrl = (controlState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (controlState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        const std::string enterPayload = RemappedEnterPayload(config);
        const std::string shiftEnterPayload = config.swapEnter ? "\r" : "\x1b\r";

        switch (key.wVirtualKeyCode)
        {
        case VK_RETURN:
            return Repeat(shift ? shiftEnterPayload : enterPayload, key.wRepeatCount);
        case VK_TAB:
            return Repeat(shift ? "\x1b[Z" : "\t", key.wRepeatCount);
        case VK_BACK:
            return Repeat("\x7f", key.wRepeatCount);
        case VK_ESCAPE:
            return Repeat("\x1b", key.wRepeatCount);
        case VK_UP:
            return Repeat("\x1b[A", key.wRepeatCount);
        case VK_DOWN:
            return Repeat("\x1b[B", key.wRepeatCount);
        case VK_RIGHT:
            return Repeat("\x1b[C", key.wRepeatCount);
        case VK_LEFT:
            return Repeat("\x1b[D", key.wRepeatCount);
        case VK_HOME:
            return Repeat("\x1b[H", key.wRepeatCount);
        case VK_END:
            return Repeat("\x1b[F", key.wRepeatCount);
        case VK_DELETE:
            return Repeat("\x1b[3~", key.wRepeatCount);
        case VK_INSERT:
            return Repeat("\x1b[2~", key.wRepeatCount);
        case VK_PRIOR:
            return Repeat("\x1b[5~", key.wRepeatCount);
        case VK_NEXT:
            return Repeat("\x1b[6~", key.wRepeatCount);
        default:
            break;
        }

        if (ctrl && !alt)
        {
            const WCHAR ch = key.uChar.UnicodeChar;
            if (ch != 0)
            {
                std::string payload;
                for (WORD i = 0; i < std::max<WORD>(key.wRepeatCount, 1); ++i)
                {
                    payload.push_back(static_cast<char>(ch & 0xff));
                }
                return payload;
            }

            if (key.wVirtualKeyCode == ' ' || key.wVirtualKeyCode == VK_SPACE)
            {
                return Repeat(std::string(1, '\0'), key.wRepeatCount);
            }
        }

        if (key.uChar.UnicodeChar != 0)
        {
            std::wstring chars(std::max<WORD>(key.wRepeatCount, 1), key.uChar.UnicodeChar);
            auto payload = Utf8FromWide(chars);
            if (alt)
            {
                payload.insert(payload.begin(), '\x1b');
            }
            return payload;
        }

        return std::nullopt;
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

    void PumpOutput(HANDLE outputRead, HANDLE consoleOutput, std::atomic<bool>& running)
    {
        std::vector<std::byte> buffer(8192);
        VtOutputFilter filter;
        while (running.load())
        {
            DWORD bytesRead = 0;
            if (!ReadFile(outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) || bytesRead == 0)
            {
                running.store(false);
                break;
            }

            const auto chunk = std::string_view(reinterpret_cast<const char*>(buffer.data()), bytesRead);
            const auto filtered = filter.Filter(chunk);
            if (!WriteString(consoleOutput, filtered))
            {
                running.store(false);
                break;
            }
        }

        const auto pending = filter.Flush();
        if (!pending.empty() && !WriteString(consoleOutput, pending))
        {
            running.store(false);
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

    if (!config.childCommand.has_value())
    {
        std::wcerr << L"A child command is required after `--`.\n\n";
        PrintUsage();
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

    Session session;
    if (!StartSession(config, consoleOutput.get(), session))
    {
        return 1;
    }

    std::atomic<bool> running{ true };
    std::thread outputThread(PumpOutput, session.outputRead.get(), consoleOutput.get(), std::ref(running));
    std::optional<std::chrono::steady_clock::time_point> pendingEnterDeadline;

    const auto flushPendingEnter = [&]() -> bool
    {
        if (!pendingEnterDeadline.has_value())
        {
            return true;
        }

        const auto payload = RemappedEnterPayload(config);
        pendingEnterDeadline.reset();
        if (!WriteString(session.inputWrite.get(), payload))
        {
            std::wcerr << L"Failed to write delayed Enter to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
            running.store(false);
            return false;
        }

        return true;
    };

    while (running.load())
    {
        if (pendingEnterDeadline.has_value() &&
            std::chrono::steady_clock::now() >= *pendingEnterDeadline)
        {
            if (!flushPendingEnter())
            {
                break;
            }
        }

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
            const auto sleepDuration = PollIntervalUntil(pendingEnterDeadline);
            if (sleepDuration.count() > 0)
            {
                std::this_thread::sleep_for(sleepDuration);
            }
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

            const auto& key = record.Event.KeyEvent;
            if (!key.bKeyDown)
            {
                continue;
            }

            const bool plainEnter = IsPlainEnterCandidate(key);
            const auto now = std::chrono::steady_clock::now();

            if (config.doubleTapEnter && pendingEnterDeadline.has_value())
            {
                if (plainEnter && now < *pendingEnterDeadline)
                {
                    pendingEnterDeadline.reset();
                    if (!WriteString(session.inputWrite.get(), RawEnterPayload()))
                    {
                        std::wcerr << L"Failed to write raw Enter to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
                        running.store(false);
                        break;
                    }
                    continue;
                }

                if (now >= *pendingEnterDeadline || !plainEnter)
                {
                    if (!flushPendingEnter())
                    {
                        break;
                    }
                }
            }

            if (config.doubleTapEnter && plainEnter)
            {
                pendingEnterDeadline = now + *config.delayWindow;
                continue;
            }

            const auto payload = EncodeKey(key, config);
            if (!payload.has_value())
            {
                continue;
            }

            if (!WriteString(session.inputWrite.get(), *payload))
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

    if (!WriteString(consoleOutput.get(), ResetMouseTrackingModes()))
    {
        std::wcerr << L"Failed to reset mouse tracking modes on stdout.\n";
        exitCode = 1;
    }

    return static_cast<int>(exitCode);
}
