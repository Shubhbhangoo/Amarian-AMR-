/// Minimal native Windows operator panel for the public testnet.
///
/// This is a small operator panel for the testnet node, wallet and miner.
#include <windows.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {
constexpr int ID_START_NODE = 1001;
constexpr int ID_OPEN_EXPLORER = 1002;
constexpr int ID_OPEN_DATA = 1003;
constexpr int ID_START_MINER = 1004;
constexpr int ID_STATUS = 1005;
constexpr int ID_CREATE_WALLET = 1006;
constexpr int ID_RECEIVE = 1007;
constexpr int ID_BALANCE = 1008;
constexpr int ID_SEND = 1009;
constexpr int ID_RECIPIENT = 1010;
constexpr int ID_AMOUNT = 1011;
constexpr int ID_OUTPUT = 1012;

std::filesystem::path ExeDir() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path TestnetDir() {
    wchar_t* local = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&local, &length, L"LOCALAPPDATA") == 0 && local != nullptr) {
        std::filesystem::path result = std::filesystem::path(local) / L"Amarian" / L"testnet";
        free(local);
        return result;
    }
    return std::filesystem::temp_directory_path() / L"Amarian" / L"testnet";
}

void SetStatus(HWND window, const wchar_t* text) {
    SetWindowTextW(GetDlgItem(window, ID_STATUS), text);
}

bool Launch(HWND window, const std::filesystem::path& executable, const std::wstring& arguments) {
    std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr, executable.parent_path().c_str(), &startup,
                        &process)) {
        SetStatus(window, L"Launch failed. Check that the executable is beside this panel.");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

std::optional<std::wstring> RunCommand(const wchar_t* executable_name,
                                       const std::wstring& command,
                                       bool include_wallet) {
    const auto dir = TestnetDir();
    const auto binaries = ExeDir();
    const std::wstring args = L"--chain testnet --datadir \"" + dir.wstring() + L"\" " +
                              (include_wallet ? L"--wallet \"" + (dir / L"wallet.dat").wstring() + L"\" " : L"") + command;
    SECURITY_ATTRIBUTES security{.nLength = sizeof(security), .bInheritHandle = TRUE};
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &security, 0)) return std::nullopt;
    SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
    std::wstring full = L"\"" + (binaries / executable_name).wstring() + L"\" " + args;
    std::vector<wchar_t> mutable_command(full.begin(), full.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{.cb = sizeof(startup), .dwFlags = STARTF_USESTDHANDLES,
                         .hStdOutput = write_handle, .hStdError = write_handle};
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, binaries.c_str(), &startup, &process);
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
    if (output.empty()) return std::wstring(L"Wallet command returned no output.");
    const int length = MultiByteToWideChar(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), wide.data(), length);
    return wide;
}

std::optional<std::wstring> RunWallet(const std::wstring& command) {
    return RunCommand(L"amarian-wallet.exe", command, true);
}

std::optional<std::wstring> RunCli(const std::wstring& command) {
    return RunCommand(L"amarian-cli.exe", L"--rpcport 12511 " + command, false);
}

void SetOutput(HWND window, const wchar_t* text) {
    SetWindowTextW(GetDlgItem(window, ID_OUTPUT), text);
    SetStatus(window, text);
}

std::wstring FormatAmr(int64_t facets) {
    const bool negative = facets < 0;
    const uint64_t magnitude = negative
                                   ? static_cast<uint64_t>(-(facets + 1)) + 1U
                                   : static_cast<uint64_t>(facets);
    const uint64_t whole = magnitude / 10000000000ULL;
    const uint64_t fraction = magnitude % 10000000000ULL;
    std::wstring fraction_text = std::to_wstring(fraction);
    fraction_text.insert(fraction_text.begin(),
                         static_cast<size_t>(10U - fraction_text.size()), L'0');
    return std::wstring(negative ? L"-" : L"") + std::to_wstring(whole) + L"." +
           fraction_text + L" AMR (" + std::to_wstring(facets) + L" facets)";
}

std::wstring FormatBalance(const std::wstring& raw) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()),
                                       utf8.data(), bytes, nullptr, nullptr);
    const auto value = nlohmann::json::parse(utf8, nullptr, false);
    if (!value.is_object() || !value.contains("confirmed")) return raw;
    const int64_t confirmed = value.value("confirmed", int64_t{0});
    const int64_t pending = value.value("pending", int64_t{0});
    const int64_t total = value.value("total", confirmed + pending);
    return L"Confirmed: " + FormatAmr(confirmed) + L"\r\nPending:   " + FormatAmr(pending) +
           L"\r\nTotal:     " + FormatAmr(total);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Amarian public testnet", WS_VISIBLE | WS_CHILD,
                      24, 20, 420, 30, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Testnet node, wallet, explorer and CUDA miner. Mainnet is not enabled.",
                      WS_VISIBLE | WS_CHILD, 24, 52, 520, 24, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Create wallet", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      24, 94, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CREATE_WALLET)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Start testnet node", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      184, 94, 180, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_START_NODE)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Start GPU miner", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      374, 94, 170, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_START_MINER)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open explorer", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      24, 140, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN_EXPLORER)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Open data folder", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      184, 140, 180, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN_DATA)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Receive address", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      374, 140, 170, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECEIVE)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Balance", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      24, 182, 150, 34, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BALANCE)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Recipient:", WS_VISIBLE | WS_CHILD, 184, 187, 70, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 254, 182, 290, 28, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECIPIENT)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Amount:", WS_VISIBLE | WS_CHILD, 24, 225, 60, 22, window, nullptr, nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER, 84, 220, 150, 28, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_AMOUNT)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Send", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                      254, 220, 110, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEND)), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Ready. Testnet RPC: 127.0.0.1:12511", WS_VISIBLE | WS_CHILD,
                      24, 262, 520, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), nullptr, nullptr);
        CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                      24, 296, 520, 90, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OUTPUT)), nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND: {
        const int command = LOWORD(wparam);
        const auto dir = TestnetDir();
        std::filesystem::create_directories(dir);
        const auto binaries = ExeDir();
        if (command == ID_CREATE_WALLET) {
            const auto output = RunWallet(L"create");
            SetOutput(window, output ? output->c_str() : L"Wallet creation failed.");
        } else if (command == ID_START_NODE) {
            const std::wstring args = L"--chain testnet --datadir \"" + dir.wstring() +
                                      L"\" --wallet \"" + (dir / L"wallet.dat").wstring() +
                                      L"\" --rpc --rpcport 12511 --p2p-port 12510 --explorer-port 12512";
            if (Launch(window, binaries / L"amariand.exe", args)) {
                SetStatus(window, L"Node and wallet launched. Wait a moment, then start the miner.");
            }
        } else if (command == ID_START_MINER) {
            const std::wstring args = L"--chain testnet --datadir \"" + dir.wstring() +
                                      L"\" --rpcport 12511 --wallet \"" + (dir / L"wallet.dat").wstring() + L"\"";
            if (Launch(window, binaries / L"amarian-miner.exe", args)) {
                SetStatus(window, L"Wallet-backed GPU miner launched against testnet.");
            }
        } else if (command == ID_OPEN_EXPLORER) {
            ShellExecuteW(window, L"open", L"http://127.0.0.1:12512/explorer/", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (command == ID_OPEN_DATA) {
            ShellExecuteW(window, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (command == ID_RECEIVE) {
            const auto output = RunCli(L"getnewaddress");
            SetOutput(window, output ? output->c_str() : L"Could not create a receive address.");
        } else if (command == ID_BALANCE) {
            const auto output = RunCli(L"getbalance");
            const std::wstring formatted = output ? FormatBalance(*output) : L"Could not read wallet balance.";
            SetOutput(window, formatted.c_str());
        } else if (command == ID_SEND) {
            wchar_t recipient[512]{}; wchar_t amount[64]{};
            GetWindowTextW(GetDlgItem(window, ID_RECIPIENT), recipient, 512);
            GetWindowTextW(GetDlgItem(window, ID_AMOUNT), amount, 64);
            if (recipient[0] == L'\0' || amount[0] == L'\0') {
                SetOutput(window, L"Enter a recipient address and amount first.");
            } else {
                const std::wstring send = L"\"" + std::wstring(recipient) + L"\" " + amount;
                const auto output = RunCli(L"sendtoaddress " + send);
                SetOutput(window, output ? output->c_str() : L"Send failed to start.");
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    const wchar_t class_name[] = L"AmarianTestnetPanel";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = WindowProc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, class_name, L"Amarian Testnet", WS_OVERLAPPED | WS_CAPTION |
                                  WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                  570, 445, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
