// Hi5Central Virtual Display Host v0.1
// Controls a software PnP device backed by the Hi5VirtualDisplay IddCx UMDF driver.
// The virtual display is present while this service is running and is removed on stop.
// No driver signing or code-integrity settings are modified.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <swdevice.h>
#include <cwchar>
#include <cstdio>

namespace {
constexpr wchar_t kServiceName[] = L"Hi5VirtualDisplayHost";
constexpr wchar_t kDeviceName[] = L"Hi5VirtualDisplay";
constexpr wchar_t kHardwareIds[] = L"Hi5VirtualDisplay\0\0";
constexpr wchar_t kDescription[] = L"Hi5Central Virtual Display";
SERVICE_STATUS_HANDLE gServiceHandle = nullptr;
SERVICE_STATUS gServiceStatus = {};
HANDLE gStopEvent = nullptr;
HSWDEVICE gDevice = nullptr;
HRESULT gCreateStatus = E_UNEXPECTED;

void ReportStatus(DWORD state, DWORD error = NO_ERROR) {
    if (!gServiceHandle) return;
    gServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gServiceStatus.dwCurrentState = state;
    gServiceStatus.dwWin32ExitCode = error;
    gServiceStatus.dwControlsAccepted = (state == SERVICE_RUNNING)
        ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    gServiceStatus.dwCheckPoint = (state == SERVICE_START_PENDING) ? 1 : 0;
    gServiceStatus.dwWaitHint = (state == SERVICE_START_PENDING) ? 25000 : 0;
    SetServiceStatus(gServiceHandle, &gServiceStatus);
}

void CALLBACK Created(HSWDEVICE device, HRESULT result,
                      PVOID context, PCWSTR instanceId) {
    UNREFERENCED_PARAMETER(device);
    UNREFERENCED_PARAMETER(instanceId);
    gCreateStatus = result;
    SetEvent(static_cast<HANDLE>(context));
}

DWORD StartDevice() {
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready) return GetLastError();
    SW_DEVICE_CREATE_INFO info = {};
    info.cbSize = sizeof(info);
    info.pszInstanceId = kDeviceName;
    info.pszzHardwareIds = kHardwareIds;
    info.pszzCompatibleIds = kHardwareIds;
    info.pszDeviceDescription = kDescription;
    info.CapabilityFlags = SWDeviceCapabilitiesRemovable
        | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;
    gCreateStatus = E_UNEXPECTED;
    const HRESULT h = SwDeviceCreate(kDeviceName, L"HTREE\\ROOT\\0", &info,
                                     0, nullptr, Created, ready, &gDevice);
    if (FAILED(h)) {
        CloseHandle(ready);
        return HRESULT_FACILITY(h) == FACILITY_WIN32 ? HRESULT_CODE(h) : ERROR_GEN_FAILURE;
    }
    const DWORD wait = WaitForSingleObject(ready, 20000);
    if (wait != WAIT_OBJECT_0 || FAILED(gCreateStatus)) {
        if (gDevice) {
            SwDeviceClose(gDevice);
            gDevice = nullptr;
        }
        CloseHandle(ready);
        return (wait == WAIT_TIMEOUT) ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
    }
    CloseHandle(ready);
    return NO_ERROR;
}

void StopDevice() {
    if (gDevice) {
        SwDeviceClose(gDevice);
        gDevice = nullptr;
    }
}

VOID WINAPI ControlHandler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING);
        if (gStopEvent) SetEvent(gStopEvent);
    }
}

VOID WINAPI ServiceMain(DWORD, LPWSTR*) {
    gServiceHandle = RegisterServiceCtrlHandlerW(kServiceName, ControlHandler);
    if (!gServiceHandle) return;
    ReportStatus(SERVICE_START_PENDING);
    gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!gStopEvent) {
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    const DWORD result = StartDevice();
    if (result != NO_ERROR) {
        CloseHandle(gStopEvent);
        gStopEvent = nullptr;
        ReportStatus(SERVICE_STOPPED, result);
        return;
    }
    ReportStatus(SERVICE_RUNNING);
    WaitForSingleObject(gStopEvent, INFINITE);
    ReportStatus(SERVICE_STOP_PENDING);
    StopDevice();
    CloseHandle(gStopEvent);
    gStopEvent = nullptr;
    ReportStatus(SERVICE_STOPPED);
}

BOOL WINAPI ConsoleControl(DWORD control) {
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
        control == CTRL_CLOSE_EVENT) {
        if (gStopEvent) SetEvent(gStopEvent);
        return TRUE;
    }
    return FALSE;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--console") == 0) {
        gStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!gStopEvent) return 1;
        SetConsoleCtrlHandler(ConsoleControl, TRUE);
        const DWORD result = StartDevice();
        if (result != NO_ERROR) {
            fwprintf(stderr, L"Hi5DisplayHost: SwDeviceCreate failed: %lu\n", result);
        } else {
            wprintf(L"Virtual display active. Ctrl+C to stop.\n");
            WaitForSingleObject(gStopEvent, INFINITE);
            StopDevice();
        }
        CloseHandle(gStopEvent);
        gStopEvent = nullptr;
        return result == NO_ERROR ? 0 : 1;
    }
    SERVICE_TABLE_ENTRYW services[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(services)) {
        fwprintf(stderr, L"Run as a service or use --console for diagnostics.\n");
        return 1;
    }
    return 0;
}
