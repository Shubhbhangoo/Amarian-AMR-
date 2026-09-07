/// Standalone native Windows wallet interface for the Amarian testnet.
///
/// The wallet file is handled by amarian-wallet.exe for creation, while online
/// actions use authenticated amarian-cli RPC so the node remains the sole owner
/// of the open wallet state.
#include <windows.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr int ID_CREATE = 2001;
constexpr int ID_REFRESH = 2002;
constexpr int ID_RECEIVE = 2003;
constexpr int ID_SEND = 2004;
constexpr int ID_OPEN_DATA = 2005;
constexpr int ID_RECIPIENT = 2006;
constexpr int ID_AMOUNT = 2007;
constexpr int ID_OUTPUT = 2008;
constexpr int ID_STATUS = 2009;

std::filesystem::path ExeDir() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path DataDir() {
    wchar_t* local = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local != nullptr) {
        std::filesystem::path result = std::filesystem::path(local) / L"Amarian" / L"testnet";
        free(local);
        return result;
    }
    return std::filesystem::temp_directory_path() / L"Amarian" / L"testnet";
}

/// One argument, quoted the way the C runtime on the other side will unquote it.
///
/// This GUI builds a command line as text, which means anything typed or pasted into a field
/// is syntax until it is quoted. Without this, a recipient containing a quote character ends
/// the quote early and the remainder of the paste becomes further `amarian-cli` options --
/// including `--datadir`, which chooses the cookie the call authenticates with. An Amarian
/// address never contains a quote, so this only matters for a hostile paste, which is exactly
/// the case a wallet has to survive.
std::wstring Quote(const std::wstring& value) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            // A literal quote needs an odd number of backslashes in front of it, so the run
            // that preceded it is doubled and one more is added for the quote itself.
            quoted.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
            quoted.push_back(L'"');
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    // Trailing backslashes are doubled so they do not escape the closing quote.
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::optional<std::wstring> Run(const wchar_t* executable, const std::wstring& arguments) {
    const auto binary = ExeDir() / executable;
    std::wstring command = Quote(binary.wstring()) + L" " + arguments;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    SECURITY_ATTRIBUTES security{.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0)) return std::nullopt;
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                         .hStdOutput = write_handle, .hStdError = write_handle};
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, binary.parent_path().c_str(),
                                        &startup, &process);
    CloseHandle(write_handle);
    if (!created) { CloseHandle(read_handle); return std::nullopt; }
    std::string output;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_handle, buffer, sizeof(buffer), &got, nullptr) && got != 0) {
        output.append(buffer, got);
        if (output.size() > 2U * 1024U * 1024U) break;
    }
    CloseHandle(read_handle);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const int length = MultiByteToWideChar(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), wide.data(), length);
    return wide.empty() ? std::optional<std::wstring>{L"No output returned."} : wide;
}

std::wstring Cli(const std::wstring& method) {
    const auto dir = DataDir();
    const auto result = Run(L"amarian-cli.exe", L"--chain testnet --datadir " + Quote(dir.wstring()) +
                            L" --rpcport 12511 " + method);
    return result.value_or(L"Could not start amarian-cli.exe.");
}

std::wstring Wallet(const std::wstring& method) {
    const auto dir = DataDir();
    const auto result = Run(L"amarian-wallet.exe", L"--chain testnet --datadir " + Quote(dir.wstring()) +
                            L" --wallet " + Quote((dir / L"wallet.dat").wstring()) + L" " + method);
    return result.value_or(L"Could not start amarian-wallet.exe.");
}

/// Facets per AMR. The protocol's base unit is the facet and there are ten decimal places;
/// this GUI never holds a monetary value as anything but an integer count of facets.
constexpr uint64_t FACETS_PER_AMR = 10000000000ULL;
constexpr size_t AMR_DECIMALS = 10;

/// The largest whole-AMR part that still converts to facets inside a signed 64-bit integer.
/// The node enforces the actual supply cap -- duplicating it here would create a second place
/// where Amarian's money limit is defined -- so this bound exists only to keep the conversion
/// below from wrapping.
constexpr uint64_t MAX_WHOLE_AMR = static_cast<uint64_t>(INT64_MAX) / FACETS_PER_AMR;

std::wstring Amr(int64_t facets) {
    const bool negative = facets < 0;
    const uint64_t magnitude = negative ? static_cast<uint64_t>(-(facets + 1)) + 1U
                                        : static_cast<uint64_t>(facets);
    const uint64_t whole = magnitude / FACETS_PER_AMR;
    const uint64_t fraction = magnitude % FACETS_PER_AMR;
    std::wstring fraction_text = std::to_wstring(fraction);
    fraction_text.insert(fraction_text.begin(), AMR_DECIMALS - fraction_text.size(), L'0');
    return std::wstring(negative ? L"-" : L"") + std::to_wstring(whole) + L"." + fraction_text + L" AMR";
}

std::wstring BalanceText(const std::wstring& raw) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()), utf8.data(), bytes, nullptr, nullptr);
    const auto value = nlohmann::json::parse(utf8, nullptr, false);
    if (!value.is_object() || !value.contains("confirmed")) return raw;
    const int64_t confirmed = value.value("confirmed", int64_t{0});
    const int64_t pending = value.value("pending", int64_t{0});
    const int64_t total = value.value("total", confirmed + pending);
    return L"Confirmed: " + Amr(confirmed) + L"\r\nPending:   " + Amr(pending) +
           L"\r\nTotal:     " + Amr(total);
}

/// One typed AMR amount as a count of facets, or nothing if it is not an amount.
///
/// Every rejection here is a refusal to guess. The alternative in a Send field is not a
/// slightly wrong number, it is a different transaction than the one the operator meant, and
/// the node has no way to tell the difference.
std::optional<int64_t> ParseAmr(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    const size_t dot = text.find(L'.');
    std::wstring whole_text = dot == std::wstring::npos ? text : text.substr(0, dot);
    std::wstring frac_text = dot == std::wstring::npos ? L"" : text.substr(dot + 1);
    // A lone "." has digits on neither side and is not the number zero.
    if (whole_text.empty() && frac_text.empty()) return std::nullopt;
    if (whole_text.empty()) whole_text = L"0";
    if (frac_text.size() > AMR_DECIMALS) return std::nullopt;
    while (frac_text.size() < AMR_DECIMALS) frac_text.push_back(L'0');
    uint64_t whole = 0;
    uint64_t fraction = 0;
    // The bound is tested before each digit is folded in, not after the loop. `whole` is
    // unsigned, so a long enough run of digits wraps around modulo 2^64 rather than
    // overflowing, and a bound tested at the end sees whatever it wrapped to: "18446744073709551617"
    // would arrive as 1 and quietly send one AMR.
    for (const wchar_t c : whole_text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        if (whole > MAX_WHOLE_AMR / 10U) return std::nullopt;
        whole = whole * 10U + static_cast<uint64_t>(c - L'0');
        if (whole > MAX_WHOLE_AMR) return std::nullopt;
    }
    for (const wchar_t c : frac_text) {
        if (c < L'0' || c > L'9') return std::nullopt;
        fraction = fraction * 10U + static_cast<uint64_t>(c - L'0');
    }
    // `MAX_WHOLE_AMR` is a floor division, so the largest accepted whole part still leaves
    // room above it for a fraction that does not fit. Neither term can wrap here.
    const uint64_t facets = whole * FACETS_PER_AMR + fraction;
    if (facets > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
    return static_cast<int64_t>(facets);
}

void Output(HWND window, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(window, ID_OUTPUT), text.c_str());
    SetWindowTextW(GetDlgItem(window, ID_STATUS), text.c_str());
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        CreateWindowW(L"STATIC", L"Amarian Wallet — Testnet", WS_VISIBLE | WS_CHILD, 24, 18, 500, 30, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Balances are shown in AMR. The node must be running for online actions.", WS_VISIBLE | WS_CHILD, 24, 48, 620, 24, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Create wallet", WS_VISIBLE | WS_CHILD, 24, 88, 140, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CREATE)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Refresh balance", WS_VISIBLE | WS_CHILD, 174, 88, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Receive address", WS_VISIBLE | WS_CHILD, 334, 88, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECEIVE)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open data folder", WS_VISIBLE | WS_CHILD, 494, 88, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN_DATA)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Recipient address:", WS_VISIBLE | WS_CHILD, 24, 145, 130, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 160, 140, 484, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECIPIENT)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Amount (AMR):", WS_VISIBLE | WS_CHILD, 24, 185, 130, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 160, 180, 180, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_AMOUNT)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Send", WS_VISIBLE | WS_CHILD, 354, 178, 130, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEND)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Ready.", WS_VISIBLE | WS_CHILD, 24, 230, 620, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 24, 270, 620, 130, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OUTPUT)), nullptr, nullptr);
        return 0;
    }
    if (message == WM_COMMAND) {
        const auto dir = DataDir();
        std::filesystem::create_directories(dir);
        const int command = LOWORD(wparam);
        if (command == ID_CREATE) {
            Output(window, Wallet(L"create"));
        } else if (command == ID_REFRESH) {
            Output(window, BalanceText(Cli(L"getbalance")));
        } else if (command == ID_RECEIVE) {
            Output(window, Cli(L"getnewaddress"));
        } else if (command == ID_OPEN_DATA) {
            ShellExecuteW(window, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (command == ID_SEND) {
            wchar_t recipient[512]{}, amount_text[128]{};
            GetWindowTextW(GetDlgItem(window, ID_RECIPIENT), recipient, 512);
            GetWindowTextW(GetDlgItem(window, ID_AMOUNT), amount_text, 128);
            const auto amount = ParseAmr(amount_text);
            // Three separate refusals, because "enter a valid recipient and amount" does not
            // tell someone staring at a filled-in form which half it objected to.
            if (recipient[0] == L'\0') {
                Output(window, L"Enter a recipient address.");
            } else if (!amount.has_value()) {
                Output(window, L"Enter an amount in AMR with at most 10 decimal places.");
            } else if (*amount == 0) {
                Output(window, L"Enter an amount greater than zero.");
            } else {
                Output(window, Cli(L"sendtoaddress " + Quote(recipient) + L" " +
                                   std::to_wstring(*amount)));
            }
        }
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const wchar_t class_name[] = L"AmarianWalletGui";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = WindowProc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Could not register the wallet window class.", L"Amarian Wallet", MB_ICONERROR);
        return 1;
    }
    HWND window = CreateWindowExW(0, class_name, L"Amarian Wallet", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                                  100, 100, 690, 460, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Could not create the wallet window.", L"Amarian Wallet", MB_ICONERROR);
        return 1;
    }
    SetWindowTextW(window, L"Amarian Wallet");
    SetWindowPos(window, HWND_TOP, 100, 100, 690, 460, SWP_SHOWWINDOW);
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return static_cast<int>(message.wParam);
}
