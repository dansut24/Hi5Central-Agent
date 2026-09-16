#include "wasapi_loopback.h"
#ifdef _WIN32
#include "../../util/log.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <algorithm>
#include <utility>
#include <vector>

namespace hi5 {
WasapiLoopbackCapture::~WasapiLoopbackCapture() { Stop(); }

bool WasapiLoopbackCapture::Start(PcmCallback callback) {
    Stop();
    callback_ = std::move(callback);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_) return false;
    running_ = true;
    thread_ = std::thread([this]() { Run(); });
    return true;
}

void WasapiLoopbackCapture::Stop() {
    running_ = false;
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    callback_ = nullptr;
}

void WasapiLoopbackCapture::Run() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE audioEvent = nullptr;

    auto cleanup = [&]() {
        if (client) client->Stop();
        if (audioEvent) CloseHandle(audioEvent);
        if (capture) capture->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (SUCCEEDED(co)) CoUninitialize();
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) { LogWarn("[audio] MMDeviceEnumerator unavailable hr=" + std::to_string(hr)); cleanup(); return; }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(hr)) { LogWarn("[audio] no default render endpoint hr=" + std::to_string(hr)); cleanup(); return; }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    if (FAILED(hr)) { LogWarn("[audio] render endpoint activation failed hr=" + std::to_string(hr)); cleanup(); return; }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * (format.wBitsPerSample / 8));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &format, nullptr);
    if (FAILED(hr)) { LogWarn("[audio] loopback initialise failed hr=" + std::to_string(hr)); cleanup(); return; }

    audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioEvent || FAILED(client->SetEventHandle(audioEvent))) {
        LogWarn("[audio] loopback event setup failed"); cleanup(); return;
    }
    hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (FAILED(hr)) { LogWarn("[audio] capture service unavailable hr=" + std::to_string(hr)); cleanup(); return; }
    hr = client->Start();
    if (FAILED(hr)) { LogWarn("[audio] loopback start failed hr=" + std::to_string(hr)); cleanup(); return; }

    LogInfo("[audio] WASAPI loopback started 48kHz stereo PCM16");
    HANDLE waits[2]{ stopEvent_, audioEvent };
    while (running_.load()) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) continue;
        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufferFlags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr))) break;
            if (frames > 0 && callback_) {
                if ((bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::vector<int16_t> silence(static_cast<size_t>(frames) * 2u, 0);
                    callback_(silence.data(), frames);
                } else {
                    callback_(reinterpret_cast<const int16_t*>(data), frames);
                }
            }
            capture->ReleaseBuffer(frames);
        }
    }
    LogInfo("[audio] WASAPI loopback stopped");
    cleanup();
}
}
#endif
