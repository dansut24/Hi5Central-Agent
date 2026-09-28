#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "comctl32.lib")

using json = nlohmann::json;

namespace {
constexpr UINT WM_PORTAL_RESULT = WM_APP + 301;
constexpr int ID_LIST = 1001;
constexpr int ID_REFRESH = 1002;
constexpr int ID_INSTALL = 1003;
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Hi5CentralAppPortal";

struct PortalApp {
    std::string id;
    std::string name;
    std::string publisher;
    std::string version;
    std::string intent;
    std::string status;
};
struct AsyncResult {
    std::string operation;
    json payload;
    std::string error;
};
struct PortalState {
    HWND window = nullptr;
    HWND title = nullptr;
    HWND subtitle = nullptr;
    HWND list = nullptr;
    HWND refresh = nullptr;
    HWND install = nullptr;
    HWND status = nullptr;
    std::vector<PortalApp> apps;
    bool busy = false;
};
PortalState g_state;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), needed);
    return out;
}
void SetStatus(const std::wstring& value) {
    if (g_state.status) SetWindowTextW(g_state.status, value.c_str());
}
void SetListViewText(HWND list, int row, int subItem, const std::wstring& value) {
    LVITEMW item{};
    item.iSubItem = subItem;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
}
void SetBusy(bool busy) {
    g_state.busy = busy;
    if (g_state.refresh) EnableWindow(g_state.refresh, busy ? FALSE : TRUE);
    if (g_state.install) EnableWindow(g_state.install, busy ? FALSE : TRUE);
}
json PipeRequest(const json& request) {
    if (!WaitNamedPipeW(kPipeName, 3000)) {
        throw std::runtime_error("Hi5Central Agent App Portal service is not available.");
    }
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not connect to Hi5Central Agent.");
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    const std::string encoded = request.dump();
    DWORD written = 0;
    if (!WriteFile(pipe, encoded.data(), static_cast<DWORD>(encoded.size()), &written, nullptr)
        || written != encoded.size()) {
        CloseHandle(pipe);
        throw std::runtime_error("Could not send App Portal request.");
    }

    std::vector<char> buffer(64 * 1024);
    std::string response;
    for (;;) {
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
        if (read > 0) response.append(buffer.data(), read);
        if (response.size() > 2 * 1024 * 1024) {
            CloseHandle(pipe);
            throw std::runtime_error("App Portal response was too large.");
        }
        if (ok) break;
        if (GetLastError() == ERROR_MORE_DATA) continue;
        CloseHandle(pipe);
        throw std::runtime_error("Could not read App Portal response.");
    }
    CloseHandle(pipe);

    const json payload = json::parse(response, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        throw std::runtime_error("Hi5Central returned an invalid App Portal response.");
    }
    return payload;
}

void RunAsync(const std::string& operation, json request) {
    SetBusy(true);
    std::thread([operation, request = std::move(request)]() {
        auto result = std::make_unique<AsyncResult>();
        result->operation = operation;
        try {
            result->payload = PipeRequest(request);
            if (result->payload.value("success", false) != true) {
                result->error = result->payload.value("error", std::string("Request failed."));
            }
        } catch (const std::exception& ex) {
            result->error = ex.what();
        }
        HWND window = g_state.window;
        if (window) PostMessageW(window, WM_PORTAL_RESULT, 0, reinterpret_cast<LPARAM>(result.release()));
    }).detach();
}
std::string DisplayIntent(const std::string& value) {
    if (value == "approval_required") return "Approval required";
    if (value == "required") return "Required";
    if (value == "uninstall") return "Removal assigned";
    return "Available";
}
void PopulateApps(const json& payload) {
    g_state.apps.clear();
    ListView_DeleteAllItems(g_state.list);

    std::unordered_map<std::string, std::string> liveStatus;
    const auto installations = payload.value("installations", json::array());
    if (installations.is_array()) {
        for (const auto& item : installations) {
            const std::string appId = item.value("app_id", std::string());
            if (appId.empty() || liveStatus.count(appId)) continue;
            const std::string jobStatus = item.value("job_status", std::string());
            if (jobStatus == "queued" || jobStatus == "claimed") liveStatus[appId] = "Installing";
            else if (jobStatus == "completed") liveStatus[appId] = "Installed";
            else if (jobStatus == "failed" || jobStatus == "cancelled") liveStatus[appId] = "Failed";
        }
    }

    const auto apps = payload.value("apps", json::array());
    if (!apps.is_array()) return;
    for (const auto& item : apps) {
        PortalApp app;
        app.id = item.value("id", std::string());
        app.name = item.value("name", std::string("Application"));
        app.publisher = item.value("publisher", std::string());
        app.version = item.value("version", std::string());
        app.intent = item.value("intent", std::string("available"));
        const auto current = liveStatus.find(app.id);
        app.status = current == liveStatus.end() ? DisplayIntent(app.intent) : current->second;
        const int row = static_cast<int>(g_state.apps.size());
        g_state.apps.push_back(app);

        LVITEMW listItem{};
        listItem.mask = LVIF_TEXT | LVIF_PARAM;
        listItem.iItem = row;
        std::wstring name = Utf8ToWide(app.name);
        listItem.pszText = name.data();
        listItem.lParam = row;
        ListView_InsertItem(g_state.list, &listItem);
        const std::wstring version = Utf8ToWide(app.version.empty() ? "—" : app.version);
        SetListViewText(g_state.list, row, 1, version);
        const std::wstring publisher = Utf8ToWide(app.publisher.empty() ? "Company application" : app.publisher);
        SetListViewText(g_state.list, row, 2, publisher);
        const std::wstring status = Utf8ToWide(app.status);
        SetListViewText(g_state.list, row, 3, status);
    }
    if (g_state.apps.empty()) SetStatus(L"No applications are currently assigned to you or this device.");
    else SetStatus(std::to_wstring(g_state.apps.size()) + L" application" + (g_state.apps.size() == 1 ? L"" : L"s") + L" available");
}
int SelectedIndex() {
    return ListView_GetNextItem(g_state.list, -1, LVNI_SELECTED);
}
void UpdateInstallButton() {
    const int selected = SelectedIndex();
    if (selected < 0 || selected >= static_cast<int>(g_state.apps.size())) {
        SetWindowTextW(g_state.install, L"Install");
        EnableWindow(g_state.install, FALSE);
        return;
    }
    const auto& app = g_state.apps[static_cast<size_t>(selected)];
    if (app.status == "Installing") {
        SetWindowTextW(g_state.install, L"Installing...");
        EnableWindow(g_state.install, FALSE);
    } else if (app.intent == "approval_required") {
        SetWindowTextW(g_state.install, L"Request approval");
        EnableWindow(g_state.install, g_state.busy ? FALSE : TRUE);
    } else {
        SetWindowTextW(g_state.install, L"Install");
        EnableWindow(g_state.install, g_state.busy ? FALSE : TRUE);
    }
}
void RefreshCatalogue() {
    SetStatus(L"Refreshing software catalogue...");
    RunAsync("catalogue", {{"type", "catalogue"}});
}
void InstallSelected() {
    const int selected = SelectedIndex();
    if (selected < 0 || selected >= static_cast<int>(g_state.apps.size())) return;
    const auto app = g_state.apps[static_cast<size_t>(selected)];
    if (app.status == "Installing") return;
    SetStatus(Utf8ToWide(app.intent == "approval_required"
        ? "Sending approval request for " + app.name + "..."
        : "Starting installation of " + app.name + "..."));
    RunAsync("install", {{"type", "install"}, {"appId", app.id}});
}
void Layout(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int margin = 24;
    const int buttonWidth = 126;
    MoveWindow(g_state.title, margin, 18, std::max(100, width - margin * 2 - buttonWidth), 32, TRUE);
    MoveWindow(g_state.subtitle, margin, 52, std::max(100, width - margin * 2), 34, TRUE);
    MoveWindow(g_state.refresh, width - margin - buttonWidth, 20, buttonWidth, 32, TRUE);
    MoveWindow(g_state.list, margin, 98, std::max(100, width - margin * 2), std::max(100, height - 170), TRUE);
    MoveWindow(g_state.status, margin, height - 57, std::max(100, width - margin * 2 - buttonWidth - 12), 32, TRUE);
    MoveWindow(g_state.install, width - margin - buttonWidth, height - 57, buttonWidth, 34, TRUE);
    const int listWidth = std::max(400, width - margin * 2 - 24);
    ListView_SetColumnWidth(g_state.list, 0, listWidth * 34 / 100);
    ListView_SetColumnWidth(g_state.list, 1, listWidth * 15 / 100);
    ListView_SetColumnWidth(g_state.list, 2, listWidth * 27 / 100);
    ListView_SetColumnWidth(g_state.list, 3, listWidth * 24 / 100);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_state.window = hwnd;
        HFONT uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        g_state.title = CreateWindowExW(0, L"STATIC", L"Company Software", WS_CHILD | WS_VISIBLE,
            0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        g_state.subtitle = CreateWindowExW(0, L"STATIC",
            L"Install software approved for you or this device. Administrator rights are not required.",
            WS_CHILD | WS_VISIBLE, 0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        g_state.refresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0,0,0,0, hwnd, reinterpret_cast<HMENU>(ID_REFRESH), nullptr, nullptr);
        g_state.install = CreateWindowExW(0, L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            0,0,0,0, hwnd, reinterpret_cast<HMENU>(ID_INSTALL), nullptr, nullptr);
        g_state.status = CreateWindowExW(0, L"STATIC", L"Loading assigned software...", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
            0,0,0,0, hwnd, nullptr, nullptr, nullptr);
        g_state.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0,0,0,0, hwnd, reinterpret_cast<HMENU>(ID_LIST), nullptr, nullptr);
        for (HWND control : {g_state.title,g_state.subtitle,g_state.refresh,g_state.install,g_state.status,g_state.list}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
        }
        ListView_SetExtendedListViewStyle(g_state.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* headings[] = {L"Application",L"Version",L"Publisher",L"Status"};
        for (int i=0;i<4;i++) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(headings[i]);
            column.cx = 140;
            column.iSubItem = i;
            ListView_InsertColumn(g_state.list, i, &column);
        }
        EnableWindow(g_state.install, FALSE);
        PostMessageW(hwnd, WM_COMMAND, ID_REFRESH, 0);
        return 0;
    }
    case WM_SIZE: Layout(hwnd); return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_REFRESH && !g_state.busy) RefreshCatalogue();
        if (LOWORD(wParam) == ID_INSTALL && !g_state.busy) InstallSelected();
        return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == ID_LIST) {
            const auto code = reinterpret_cast<NMHDR*>(lParam)->code;
            if (code == LVN_ITEMCHANGED) UpdateInstallButton();
            if (code == NM_DBLCLK && !g_state.busy) InstallSelected();
        }
        return 0;
    case WM_PORTAL_RESULT: {
        std::unique_ptr<AsyncResult> result(reinterpret_cast<AsyncResult*>(lParam));
        SetBusy(false);
        if (!result) return 0;
        if (!result->error.empty()) {
            SetStatus(Utf8ToWide(result->error));
            MessageBoxW(hwnd, Utf8ToWide(result->error).c_str(), L"Hi5Central App Portal", MB_OK | MB_ICONWARNING);
        } else if (result->operation == "catalogue") {
            PopulateApps(result->payload);
        } else if (result->operation == "install") {
            if (result->payload.value("approvalRequired", false)) SetStatus(L"Approval request sent.");
            else SetStatus(L"Installation started. You can continue working while Hi5Central installs the application.");
            RunAsync("catalogue", {{"type", "catalogue"}});
        }
        UpdateInstallButton();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_state.window = nullptr;
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    const wchar_t className[] = L"Hi5CentralAppPortalWindow";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

    HWND window = CreateWindowExW(0, className, L"Hi5Central App Portal", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 620, nullptr, nullptr, instance, nullptr);
    if (!window) return 3;
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
