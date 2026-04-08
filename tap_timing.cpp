#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    enum class ParseResult
    {
        Success,
        Help,
        Error
    };

    struct Config
    {
        std::optional<std::chrono::milliseconds> delayWindow;
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

    struct KeyTap
    {
        WORD virtualKey{ 0 };
        WORD scanCode{ 0 };
        DWORD modifiers{ 0 };
        std::chrono::steady_clock::time_point timestamp{};
    };

    struct PendingTap
    {
        WORD virtualKey{ 0 };
        WORD scanCode{ 0 };
        DWORD modifiers{ 0 };
        std::wstring keyName;
        std::chrono::steady_clock::time_point timestamp{};
    };

    void PrintUsage()
    {
        std::wcout
            << L"tap-timer [--delay <ms>]\n\n"
            << L"Without --delay:\n"
            << L"  <key>\n"
            << L"  <key> <key> <ms>\n\n"
            << L"With --delay N:\n"
            << L"  first tap is held until N ms expires\n"
            << L"  if a second tap of the same key arrives in time, prints:\n"
            << L"    <key> <key> <ms>\n"
            << L"  otherwise on timeout, prints:\n"
            << L"    <key>\n";
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

            std::wcerr << L"Unknown argument: " << arg << L"\n";
            return ParseResult::Error;
        }

        return ParseResult::Success;
    }

    [[nodiscard]] std::wstring FormatModifiers(DWORD controlState)
    {
        std::wstring result;
        if ((controlState & SHIFT_PRESSED) != 0)
        {
            result += L"Shift+";
        }
        if ((controlState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0)
        {
            result += L"Ctrl+";
        }
        if ((controlState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0)
        {
            result += L"Alt+";
        }
        return result;
    }

    [[nodiscard]] std::wstring DescribeKey(const KEY_EVENT_RECORD& key)
    {
        const auto modifiers = FormatModifiers(key.dwControlKeyState);

        switch (key.wVirtualKeyCode)
        {
        case VK_RETURN:
            return modifiers + L"Enter";
        case VK_ESCAPE:
            return modifiers + L"Esc";
        case VK_TAB:
            return modifiers + L"Tab";
        case VK_SPACE:
            return modifiers + L"Space";
        case VK_UP:
            return modifiers + L"Up";
        case VK_DOWN:
            return modifiers + L"Down";
        case VK_LEFT:
            return modifiers + L"Left";
        case VK_RIGHT:
            return modifiers + L"Right";
        case VK_BACK:
            return modifiers + L"Backspace";
        default:
            break;
        }

        if (key.uChar.UnicodeChar >= L' ' && key.uChar.UnicodeChar != 0x7f)
        {
            return modifiers + std::wstring(1, key.uChar.UnicodeChar);
        }

        std::wstringstream builder;
        builder << modifiers << L"VK(" << key.wVirtualKeyCode << L")";
        return builder.str();
    }

    [[nodiscard]] DWORD RelevantModifiers(DWORD controlState)
    {
        return controlState & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
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
    case ParseResult::Error:
        return 1;
    }

    HANDLE consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    if (consoleInput == INVALID_HANDLE_VALUE || consoleInput == nullptr)
    {
        std::wcerr << L"Failed to acquire stdin console handle.\n";
        return 1;
    }

    ConsoleModeGuard modeGuard(consoleInput);
    if (!ConfigureInputMode(consoleInput))
    {
        std::wcerr << L"Failed to configure console input mode.\n";
        return 1;
    }

    constexpr auto doubleTapWindow = std::chrono::milliseconds(300);
    std::vector<KeyTap> history;
    std::optional<PendingTap> pendingTap;
    std::wcout
        << L"tap-timer: press keys to measure repeated taps.\n"
        << L"Output format:\n";
    if (config.delayWindow.has_value())
    {
        std::wcout
            << L"  <key>\n"
            << L"  <key> <key> <ms>\n"
            << L"Delay simulation window: " << config.delayWindow->count() << L"ms\n";
    }
    else
    {
        std::wcout
            << L"  <key>\n"
            << L"  <key> <key> <ms>\n";
    }
    std::wcout
        << L"Press Esc to exit.\n\n";

    while (true)
    {
        if (config.delayWindow.has_value() && pendingTap.has_value())
        {
            const auto now = std::chrono::steady_clock::now();
            const auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(now - pendingTap->timestamp);
            if (delta >= *config.delayWindow)
            {
                std::wcout << pendingTap->keyName << L"\n";
                pendingTap.reset();
            }
        }

        INPUT_RECORD record{};
        DWORD recordsRead = 0;
        if (!GetNumberOfConsoleInputEvents(consoleInput, &recordsRead))
        {
            std::wcerr << L"GetNumberOfConsoleInputEvents failed.\n";
            return 1;
        }

        if (recordsRead == 0)
        {
            std::optional<std::chrono::steady_clock::time_point> deadline;
            if (config.delayWindow.has_value() && pendingTap.has_value())
            {
                deadline = pendingTap->timestamp + *config.delayWindow;
            }

            const auto sleepDuration = PollIntervalUntil(deadline);
            if (sleepDuration.count() > 0)
            {
                Sleep(static_cast<DWORD>(sleepDuration.count()));
            }
            continue;
        }

        if (!ReadConsoleInputW(consoleInput, &record, 1, &recordsRead))
        {
            std::wcerr << L"ReadConsoleInputW failed.\n";
            return 1;
        }

        if (record.EventType != KEY_EVENT)
        {
            continue;
        }

        const auto& key = record.Event.KeyEvent;
        if (!key.bKeyDown)
        {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto modifiers = RelevantModifiers(key.dwControlKeyState);

        auto previous = std::find_if(
            history.rbegin(),
            history.rend(),
            [&](const KeyTap& tap)
            {
                return tap.virtualKey == key.wVirtualKeyCode &&
                       tap.scanCode == key.wVirtualScanCode &&
                       tap.modifiers == modifiers;
            });

        std::optional<long long> deltaMs;
        bool doubleTap = false;
        if (previous != history.rend())
        {
            deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous->timestamp).count();
            doubleTap = *deltaMs <= doubleTapWindow.count();
        }

        const auto keyName = DescribeKey(key);

        if (config.delayWindow.has_value())
        {
            if (pendingTap.has_value())
            {
                const bool sameKey =
                    pendingTap->virtualKey == key.wVirtualKeyCode &&
                    pendingTap->scanCode == key.wVirtualScanCode &&
                    pendingTap->modifiers == modifiers;

                const auto pendingDelta = std::chrono::duration_cast<std::chrono::milliseconds>(now - pendingTap->timestamp);
                if (sameKey && pendingDelta <= *config.delayWindow)
                {
                    std::wcout << keyName << L" " << keyName << L" " << pendingDelta.count() << L"ms\n";
                    pendingTap.reset();
                }
                else
                {
                    std::wcout << pendingTap->keyName << L"\n";
                    pendingTap = PendingTap{
                        .virtualKey = key.wVirtualKeyCode,
                        .scanCode = key.wVirtualScanCode,
                        .modifiers = modifiers,
                        .keyName = keyName,
                        .timestamp = now
                    };
                }
            }
            else
            {
                pendingTap = PendingTap{
                    .virtualKey = key.wVirtualKeyCode,
                    .scanCode = key.wVirtualScanCode,
                    .modifiers = modifiers,
                    .keyName = keyName,
                    .timestamp = now
                };
            }
        }
        else if (doubleTap)
        {
            std::wcout << keyName << L" " << keyName << L" " << *deltaMs << L"ms\n";
        }
        else
        {
            std::wcout << keyName << L"\n";
        }

        history.push_back(KeyTap{
            .virtualKey = key.wVirtualKeyCode,
            .scanCode = key.wVirtualScanCode,
            .modifiers = modifiers,
            .timestamp = now
        });

        if (history.size() > 256)
        {
            history.erase(history.begin(), history.begin() + 128);
        }

        if (key.wVirtualKeyCode == VK_ESCAPE)
        {
            if (config.delayWindow.has_value() && pendingTap.has_value())
            {
                std::wcout << pendingTap->keyName << L"\n";
            }
            break;
        }
    }

    return 0;
}
