#pragma once
// Isolated Media Foundation codec pilot only. Hardware encoder MFTs can be
// asynchronous: ProcessInput requires METransformNeedInput and ProcessOutput
// requires METransformHaveOutput. Production H.264 uses a separate proven gate.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfidl.h>
#include <mferror.h>
#include <wrl/client.h>
#include <chrono>
#include <string>
#include <sstream>

struct MfAsyncPilotEvents {
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator;
    unsigned int needInput = 0;
    unsigned int haveOutput = 0;

    static std::string hrText(HRESULT hr) {
        std::ostringstream s;
        s << "0x" << std::hex << static_cast<unsigned long>(hr);
        return s.str();
    }

    bool waitFor(bool input, DWORD timeoutMs, std::string* error) {
        if (!generator) {
            if (error) *error = "async Media Foundation encoder lacks event generator";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto& credits = input ? needInput : haveOutput;
            if (credits > 0) {
                --credits;
                return true;
            }
            Microsoft::WRL::ComPtr<IMFMediaEvent> event;
            const HRESULT hr = generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
            if (SUCCEEDED(hr) && event) {
                MediaEventType type = MEUnknown;
                if (FAILED(event->GetType(&type))) {
                    if (error) *error = "async Media Foundation event type unavailable";
                    return false;
                }
                if (type == METransformNeedInput) ++needInput;
                else if (type == METransformHaveOutput) ++haveOutput;
                else if (type == MEError) {
                    HRESULT status = E_FAIL;
                    event->GetStatus(&status);
                    if (error) *error = "async Media Foundation MEError " + hrText(status);
                    return false;
                }
                continue;
            }
            if (hr != MF_E_NO_EVENTS_AVAILABLE) {
                if (error) *error = "async Media Foundation GetEvent failed " + hrText(hr);
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                if (error) *error = input
                    ? "async Media Foundation timed out waiting for METransformNeedInput"
                    : "async Media Foundation timed out waiting for METransformHaveOutput";
                return false;
            }
            Sleep(2);
        }
    }
};
