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

    [[nodiscard]] bool ConfigureInputMode(HANDLE input, DWORD originalMode, bool mouseTrackingActive)
    {
        DWORD mode = originalMode;
        mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        mode |= ENABLE_WINDOW_INPUT;

        if (mouseTrackingActive)
        {
            mode |= ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
            mode &= ~ENABLE_QUICK_EDIT_MODE;
        }
        else
        {
            if (originalMode & ENABLE_EXTENDED_FLAGS)
            {
                mode |= ENABLE_EXTENDED_FLAGS;
            }

            if (originalMode & ENABLE_QUICK_EDIT_MODE)
            {
                mode |= ENABLE_QUICK_EDIT_MODE;
            }
            else
            {
                mode &= ~ENABLE_QUICK_EDIT_MODE;
            }
        }

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

    enum MouseTrackingBits : unsigned int
    {
        kMouseX10 = 1u << 0,
        kMouseClick = 1u << 1,
        kMouseHighlight = 1u << 2,
        kMouseDrag = 1u << 3,
        kMouseAny = 1u << 4,
        kMouseUtf8 = 1u << 5,
        kMouseSgr = 1u << 6,
        kMouseUrxvt = 1u << 7,
        kMousePixels = 1u << 8
    };

    constexpr unsigned int kMouseEventMask = kMouseX10 | kMouseClick | kMouseHighlight | kMouseDrag | kMouseAny;
    constexpr DWORD kTrackedMouseButtons =
        FROM_LEFT_1ST_BUTTON_PRESSED |
        RIGHTMOST_BUTTON_PRESSED |
        FROM_LEFT_2ND_BUTTON_PRESSED |
        FROM_LEFT_3RD_BUTTON_PRESSED |
        FROM_LEFT_4TH_BUTTON_PRESSED;

    struct MouseTrackingState
    {
        std::atomic<unsigned int> bits{ 0 };

        [[nodiscard]] unsigned int Load() const noexcept
        {
            return bits.load(std::memory_order_relaxed);
        }

        [[nodiscard]] bool ReportingEnabled() const noexcept
        {
            return (Load() & kMouseEventMask) != 0;
        }
    };

    [[nodiscard]] std::optional<unsigned int> MouseTrackingBitForParameter(std::string_view parameter)
    {
        if (parameter == "9")
        {
            return kMouseX10;
        }
        if (parameter == "1000")
        {
            return kMouseClick;
        }
        if (parameter == "1001")
        {
            return kMouseHighlight;
        }
        if (parameter == "1002")
        {
            return kMouseDrag;
        }
        if (parameter == "1003")
        {
            return kMouseAny;
        }
        if (parameter == "1005")
        {
            return kMouseUtf8;
        }
        if (parameter == "1006")
        {
            return kMouseSgr;
        }
        if (parameter == "1015")
        {
            return kMouseUrxvt;
        }
        if (parameter == "1016")
        {
            return kMousePixels;
        }
        return std::nullopt;
    }

    void UpdateMouseTrackingState(MouseTrackingState& state, unsigned int bit, bool enabled)
    {
        if (enabled)
        {
            state.bits.fetch_or(bit, std::memory_order_relaxed);
        }
        else
        {
            state.bits.fetch_and(~bit, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::string FilterPrivateModes(std::string_view csi, MouseTrackingState& mouseTracking)
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
        bool removedFilteredMode = false;
        const bool enable = finalByte == 'h';
        std::size_t offset = 0;
        while (offset <= parameters.size())
        {
            const auto separator = parameters.find(';', offset);
            const auto length = separator == std::string_view::npos ? parameters.size() - offset : separator - offset;
            const auto parameter = parameters.substr(offset, length);

            const auto mouseBit = MouseTrackingBitForParameter(parameter);
            if (mouseBit.has_value())
            {
                UpdateMouseTrackingState(mouseTracking, *mouseBit, enable);
                removedFilteredMode = true;
            }
            else if (parameter == "9001")
            {
                removedFilteredMode = true;
            }
            else if (!parameter.empty())
            {
                if (!filtered.empty())
                {
                    filtered.push_back(';');
                }
                filtered.append(parameter);
            }

            if (separator == std::string_view::npos)
            {
                break;
            }

            offset = separator + 1;
        }

        if (!removedFilteredMode)
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
        MouseTrackingState& mouseTracking;
        std::string pendingSequence;
        bool sawEscape{ false };
        bool bufferingCsi{ false };

        explicit VtOutputFilter(MouseTrackingState& trackingState)
            : mouseTracking(trackingState)
        {
        }

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
                        output += FilterPrivateModes(pendingSequence, mouseTracking);
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

    enum class EnterKind
    {
        None,
        Plain,
        ShiftOnly
    };

    struct PendingEnter
    {
        KEY_EVENT_RECORD key{};
        std::chrono::steady_clock::time_point deadline{};
    };

    [[nodiscard]] EnterKind ClassifyEnter(const KEY_EVENT_RECORD& key)
    {
        if (key.wVirtualKeyCode != VK_RETURN || key.wRepeatCount != 1)
        {
            return EnterKind::None;
        }

        const auto modifiers =
            key.dwControlKeyState & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        if (modifiers == 0)
        {
            return EnterKind::Plain;
        }
        if (modifiers == SHIFT_PRESSED)
        {
            return EnterKind::ShiftOnly;
        }
        return EnterKind::None;
    }

    [[nodiscard]] bool UseRemappedEnterPayload(const Config& config, EnterKind kind)
    {
        switch (kind)
        {
        case EnterKind::Plain:
            return config.swapEnter;
        case EnterKind::ShiftOnly:
            return !config.swapEnter;
        case EnterKind::None:
        default:
            return false;
        }
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

    [[nodiscard]] int MouseModifierBits(DWORD controlState)
    {
        int modifiers = 0;
        if ((controlState & SHIFT_PRESSED) != 0)
        {
            modifiers += 4;
        }
        if ((controlState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0)
        {
            modifiers += 8;
        }
        if ((controlState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0)
        {
            modifiers += 16;
        }
        return modifiers;
    }

    [[nodiscard]] std::optional<int> MouseButtonCode(DWORD buttonState)
    {
        if ((buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0)
        {
            return 0;
        }
        if ((buttonState & FROM_LEFT_2ND_BUTTON_PRESSED) != 0)
        {
            return 1;
        }
        if ((buttonState & RIGHTMOST_BUTTON_PRESSED) != 0)
        {
            return 2;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> BuildMouseSequence(int buttonCode, COORD position, bool sgrEncoding, char terminator)
    {
        const int column = static_cast<int>(position.X) + 1;
        const int row = static_cast<int>(position.Y) + 1;
        if (column <= 0 || row <= 0)
        {
            return std::nullopt;
        }

        if (sgrEncoding)
        {
            std::string sequence = "\x1b[<";
            sequence += std::to_string(buttonCode);
            sequence.push_back(';');
            sequence += std::to_string(column);
            sequence.push_back(';');
            sequence += std::to_string(row);
            sequence.push_back(terminator);
            return sequence;
        }

        if (buttonCode > 223 || column > 223 || row > 223)
        {
            return std::nullopt;
        }

        std::string sequence = "\x1b[M";
        sequence.push_back(static_cast<char>(32 + buttonCode));
        sequence.push_back(static_cast<char>(32 + column));
        sequence.push_back(static_cast<char>(32 + row));
        return sequence;
    }

    [[nodiscard]] std::optional<std::string> EncodeMouse(const MOUSE_EVENT_RECORD& mouse, const MouseTrackingState& mouseTracking, DWORD& lastButtonState)
    {
        const auto trackingBits = mouseTracking.Load();
        if ((trackingBits & kMouseEventMask) == 0)
        {
            lastButtonState = mouse.dwButtonState & kTrackedMouseButtons;
            return std::nullopt;
        }

        const DWORD buttons = mouse.dwButtonState & kTrackedMouseButtons;
        const int modifiers = MouseModifierBits(mouse.dwControlKeyState);
        const bool sgrEncoding = (trackingBits & kMouseSgr) != 0;

        std::optional<int> buttonCode;
        char terminator = 'M';

        switch (mouse.dwEventFlags)
        {
        case 0:
        case DOUBLE_CLICK:
            if (buttons != 0)
            {
                const auto pressedButton = MouseButtonCode(buttons);
                if (!pressedButton.has_value())
                {
                    lastButtonState = buttons;
                    return std::nullopt;
                }
                buttonCode = *pressedButton + modifiers;
            }
            else if (lastButtonState != 0)
            {
                if (sgrEncoding)
                {
                    const auto releasedButton = MouseButtonCode(lastButtonState);
                    if (!releasedButton.has_value())
                    {
                        lastButtonState = buttons;
                        return std::nullopt;
                    }
                    buttonCode = *releasedButton + modifiers;
                    terminator = 'm';
                }
                else
                {
                    buttonCode = 3 + modifiers;
                }
            }
            break;
        case MOUSE_MOVED:
            if ((trackingBits & (kMouseDrag | kMouseAny)) == 0)
            {
                lastButtonState = buttons;
                return std::nullopt;
            }

            if (buttons == 0 && (trackingBits & kMouseAny) == 0)
            {
                lastButtonState = buttons;
                return std::nullopt;
            }

            if (buttons == 0)
            {
                buttonCode = 35 + modifiers;
            }
            else
            {
                const auto movingButton = MouseButtonCode(buttons);
                if (!movingButton.has_value())
                {
                    lastButtonState = buttons;
                    return std::nullopt;
                }
                buttonCode = 32 + *movingButton + modifiers;
            }
            break;
        case MOUSE_WHEELED:
        {
            const SHORT delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
            if (delta == 0)
            {
                lastButtonState = buttons;
                return std::nullopt;
            }
            buttonCode = (delta > 0 ? 64 : 65) + modifiers;
            break;
        }
        case MOUSE_HWHEELED:
        {
            const SHORT delta = static_cast<SHORT>(HIWORD(mouse.dwButtonState));
            if (delta == 0)
            {
                lastButtonState = buttons;
                return std::nullopt;
            }
            buttonCode = (delta > 0 ? 67 : 66) + modifiers;
            break;
        }
        default:
            lastButtonState = buttons;
            return std::nullopt;
        }

        lastButtonState = buttons;
        if (!buttonCode.has_value())
        {
            return std::nullopt;
        }

        return BuildMouseSequence(*buttonCode, mouse.dwMousePosition, sgrEncoding, terminator);
    }

    [[nodiscard]] KEY_EVENT_RECORD MakeSyntheticEscapeKey(bool keyDown)
    {
        KEY_EVENT_RECORD key{};
        key.bKeyDown = keyDown ? TRUE : FALSE;
        key.wRepeatCount = 1;
        key.wVirtualKeyCode = VK_ESCAPE;
        key.wVirtualScanCode = 1;
        key.uChar.UnicodeChar = keyDown ? L'\x1b' : 0;
        key.dwControlKeyState = 0;
        return key;
    }

    [[nodiscard]] KEY_EVENT_RECORD MakeSyntheticEnterKey(const KEY_EVENT_RECORD& source, bool keyDown)
    {
        KEY_EVENT_RECORD key = source;
        key.bKeyDown = keyDown ? TRUE : FALSE;
        key.wRepeatCount = 1;
        key.dwControlKeyState &= ~(SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        key.uChar.UnicodeChar = keyDown ? L'\r' : 0;
        return key;
    }

    [[nodiscard]] std::string BuildEnterPressPayload(const KEY_EVENT_RECORD& source, bool remapped)
    {
        std::string payload;
        if (remapped)
        {
            AppendWin32InputModeSequence(payload, MakeSyntheticEscapeKey(true));
            AppendWin32InputModeSequence(payload, MakeSyntheticEscapeKey(false));
        }

        AppendWin32InputModeSequence(payload, MakeSyntheticEnterKey(source, true));
        AppendWin32InputModeSequence(payload, MakeSyntheticEnterKey(source, false));
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

    void PumpOutput(HANDLE outputRead, HANDLE consoleOutput, std::atomic<bool>& running, MouseTrackingState& mouseTracking)
    {
        std::vector<std::byte> buffer(8192);
        VtOutputFilter filter(mouseTracking);
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

    DWORD originalInputMode = 0;
    if (!GetConsoleMode(consoleInput.get(), &originalInputMode))
    {
        std::wcerr << L"Failed to read the current console input mode.\n";
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

    if (!ConfigureInputMode(consoleInput.get(), originalInputMode, false))
    {
        std::wcerr << L"Failed to configure console input mode.\n";
        return 1;
    }

    Session session;
    if (!StartSession(config, consoleOutput.get(), session))
    {
        return 1;
    }

    MouseTrackingState mouseTracking;
    std::atomic<bool> running{ true };
    std::thread outputThread(PumpOutput, session.outputRead.get(), consoleOutput.get(), std::ref(running), std::ref(mouseTracking));
    std::optional<PendingEnter> pendingEnter;
    bool mouseTrackingActive = false;
    DWORD lastMouseButtonState = 0;

    const auto flushPendingEnter = [&]() -> bool
    {
        if (!pendingEnter.has_value())
        {
            return true;
        }

        const auto payload = BuildEnterPressPayload(pendingEnter->key, UseRemappedEnterPayload(config, EnterKind::Plain));
        pendingEnter.reset();
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
        if (pendingEnter.has_value() &&
            std::chrono::steady_clock::now() >= pendingEnter->deadline)
        {
            if (!flushPendingEnter())
            {
                break;
            }
        }

        const bool nextMouseTrackingState = mouseTracking.ReportingEnabled();
        if (nextMouseTrackingState != mouseTrackingActive)
        {
            if (!ConfigureInputMode(consoleInput.get(), originalInputMode, nextMouseTrackingState))
            {
                std::wcerr << L"Failed to update console mouse input mode.\n";
                running.store(false);
                break;
            }
            mouseTrackingActive = nextMouseTrackingState;
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
            const auto sleepDuration = pendingEnter.has_value() ?
                PollIntervalUntil(std::optional<std::chrono::steady_clock::time_point>{ pendingEnter->deadline }) :
                PollIntervalUntil(std::nullopt);
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

            if (record.EventType == MOUSE_EVENT)
            {
                if (pendingEnter.has_value())
                {
                    if (!flushPendingEnter())
                    {
                        break;
                    }
                }

                const auto payload = EncodeMouse(record.Event.MouseEvent, mouseTracking, lastMouseButtonState);
                if (!payload.has_value())
                {
                    continue;
                }

                if (!WriteString(session.inputWrite.get(), *payload))
                {
                    std::wcerr << L"Failed to write mouse input to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
                    running.store(false);
                    break;
                }
                continue;
            }

            if (record.EventType != KEY_EVENT)
            {
                continue;
            }

            const auto& key = record.Event.KeyEvent;
            const auto enterKind = ClassifyEnter(key);
            const auto now = std::chrono::steady_clock::now();

            if (config.doubleTapEnter && pendingEnter.has_value())
            {
                if (key.bKeyDown && enterKind == EnterKind::Plain && now < pendingEnter->deadline)
                {
                    const auto payload = BuildEnterPressPayload(pendingEnter->key, false);
                    pendingEnter.reset();
                    if (!WriteString(session.inputWrite.get(), payload))
                    {
                        std::wcerr << L"Failed to write raw Enter to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
                        running.store(false);
                        break;
                    }
                    continue;
                }

                if (!key.bKeyDown && enterKind == EnterKind::Plain)
                {
                    continue;
                }

                if (now >= pendingEnter->deadline || !(key.bKeyDown && enterKind == EnterKind::Plain))
                {
                    if (!flushPendingEnter())
                    {
                        break;
                    }
                }
            }

            if (enterKind != EnterKind::None)
            {
                if (key.bKeyDown)
                {
                    if (config.doubleTapEnter && enterKind == EnterKind::Plain)
                    {
                        pendingEnter = PendingEnter{ key, now + *config.delayWindow };
                        continue;
                    }

                    const auto payload = BuildEnterPressPayload(key, UseRemappedEnterPayload(config, enterKind));
                    if (!WriteString(session.inputWrite.get(), payload))
                    {
                        std::wcerr << L"Failed to write Enter remap input to child input: " << GetLastErrorMessage(GetLastError()) << L"\n";
                        running.store(false);
                        break;
                    }
                }
                continue;
            }

            const auto payload = SerializeKeyEvent(key);
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

    if (!WriteString(consoleOutput.get(), ResetMouseTrackingModes()))
    {
        std::wcerr << L"Failed to reset mouse tracking modes on stdout.\n";
        exitCode = 1;
    }

    return static_cast<int>(exitCode);
}
