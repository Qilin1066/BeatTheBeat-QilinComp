// WASAPI's exclusive mode (wasapi.hpp). Runtime code: no std::format.
#include "wasapi.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

namespace qilin::rt::host::wasapi {

namespace {

// The interfaces' and formats' identifiers, written out (the SDK's own
// definitions need __uuidof or INITGUID, which differ between compilers).
constexpr GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
constexpr GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
constexpr GUID kIidClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
constexpr GUID kIidRender = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
constexpr GUID kIidVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
constexpr GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

std::atomic<bool> g_playing{false}, g_lost{false};

// A COM pointer, released when it goes.
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T* operator->() const { return p; }
    void** out() {
        reset();
        return reinterpret_cast<void**>(&p);
    }
};

struct Format {
    WAVEFORMATEXTENSIBLE wf{};
    SDL_AudioFormat sdl = SDL_AUDIO_UNKNOWN;
    const char* name = "";
};

Format make_format(DWORD rate, WORD channels, DWORD mask, WORD bits, WORD valid, bool is_float) {
    Format f;
    WAVEFORMATEX& w = f.wf.Format;
    w.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.nChannels = channels, w.nSamplesPerSec = rate, w.wBitsPerSample = bits;
    w.nBlockAlign = WORD(channels * bits / 8), w.nAvgBytesPerSec = rate * w.nBlockAlign;
    w.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.wf.Samples.wValidBitsPerSample = valid;
    f.wf.dwChannelMask = mask;
    f.wf.SubFormat = is_float ? kSubFloat : kSubPcm;
    f.sdl = is_float ? SDL_AUDIO_F32LE : bits == 32 ? SDL_AUDIO_S32LE : SDL_AUDIO_S16LE;
    f.name = is_float ? "32-bit float" : bits == 32 ? (valid == 24 ? "24-bit in 32" : "32-bit") : "16-bit";
    return f;
}

struct Result {
    bool ok = false;
    const char* why = "";
    Opened opened;
};

// The device's thread: opens it (answering through `ready`), then fills each
// period from the stream until the device goes away.
void run(std::promise<Result> ready) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Result r;
        auto fail = [&](const char* why) {
            r.why = why;
            ready.set_value(r);
        };
        Com<IMMDeviceEnumerator> enumerator;
        Com<IMMDevice> device;
        Com<IAudioClient> client;
        if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, enumerator.out())) ||
            FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, reinterpret_cast<IMMDevice**>(device.out()))) ||
            FAILED(device->Activate(kIidClient, CLSCTX_ALL, nullptr, client.out()))) {
            fail("no default output device");
            goto done;
        }
        {
            // The device's own channels and layout, from its shared format.
            WAVEFORMATEX* mix = nullptr;
            if (FAILED(client->GetMixFormat(&mix)) || !mix) {
                fail("the device has no format");
                goto done;
            }
            const WORD channels = mix->nChannels;
            const DWORD mix_rate = mix->nSamplesPerSec;
            DWORD mask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) mask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
            CoTaskMemFree(mix);
            // The first format the device takes exclusively: 48 kHz first
            // (the game's 32 kHz resamples cleanly to any), then its own rate,
            // then the others; float, 24 in 32, 32 and 16-bit samples.
            const DWORD rates[] = {48000, mix_rate, 44100, 96000, 88200, 192000};
            Format chosen;
            bool found = false;
            for (DWORD rate : rates) {
                const Format kinds[] = {make_format(rate, channels, mask, 32, 32, true), make_format(rate, channels, mask, 32, 24, false),
                                        make_format(rate, channels, mask, 32, 32, false), make_format(rate, channels, mask, 16, 16, false)};
                for (const Format& f : kinds)
                    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &f.wf.Format, nullptr) == S_OK) {
                        chosen = f, found = true;
                        break;
                    }
                if (found) break;
            }
            if (!found) {
                fail("no exclusive format the device takes");
                goto done;
            }
            REFERENCE_TIME default_period = 0, min_period = 0;
            client->GetDevicePeriod(&default_period, &min_period);
            REFERENCE_TIME period = min_period ? min_period : default_period;
            HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                            &chosen.wf.Format, nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                // The period in whole frames the device can align to, then a
                // fresh client (a refused Initialize leaves it unusable).
                UINT32 frames = 0;
                client->GetBufferSize(&frames);
                period = REFERENCE_TIME(10'000'000.0 * frames / chosen.wf.Format.nSamplesPerSec + 0.5);
                if (FAILED(device->Activate(kIidClient, CLSCTX_ALL, nullptr, client.out()))) {
                    fail("the device went away");
                    goto done;
                }
                hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period,
                                        &chosen.wf.Format, nullptr);
            }
            if (hr == AUDCLNT_E_DEVICE_IN_USE) {
                fail("another program has the device to itself");
                goto done;
            }
            if (FAILED(hr)) {
                fail("the device refused exclusive mode");
                goto done;
            }
            const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            UINT32 frames = 0;
            Com<IAudioRenderClient> render;
            if (!event || FAILED(client->SetEventHandle(event)) || FAILED(client->GetBufferSize(&frames)) || frames == 0 ||
                FAILED(client->GetService(kIidRender, render.out()))) {
                if (event) CloseHandle(event);
                fail("the device's buffer can't be set up");
                goto done;
            }
            // Exclusive mode bypasses Windows' volume and mute unless the
            // device does them itself: then they're applied here, as the
            // shared mode would.
            Com<IAudioEndpointVolume> volume;
            bool soft_volume = false;
            if (SUCCEEDED(device->Activate(kIidVolume, CLSCTX_ALL, nullptr, volume.out()))) {
                DWORD hw = 0;
                volume->QueryHardwareSupport(&hw);
                soft_volume = !(hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME);
            } else {
                volume.reset();
            }
            const SDL_AudioSpec src{SDL_AUDIO_S16, 2, 32000};
            const SDL_AudioSpec dst{chosen.sdl, int(channels), int(chosen.wf.Format.nSamplesPerSec)};
            SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
            if (!stream) {
                CloseHandle(event);
                fail("SDL can't convert to the device's format");
                goto done;
            }
            const UINT32 block = chosen.wf.Format.nBlockAlign;
            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(frames, &data))) render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            DWORD task = 0;
            const HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
            if (FAILED(client->Start())) {
                CloseHandle(event);
                fail("the device won't start");
                goto done;
            }
            r.ok = true;
            r.opened.stream = stream, r.opened.rate = int(chosen.wf.Format.nSamplesPerSec), r.opened.channels = channels;
            r.opened.period_ms = 1000.0 * frames / chosen.wf.Format.nSamplesPerSec, r.opened.sample = chosen.name;
            ready.set_value(r);

            const int bytes = int(frames * block);
            for (unsigned n = 0;; ++n) {
                if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) continue;  // a stall: wait on
                if (FAILED(render->GetBuffer(frames, &data))) break;             // the device went away
                int got = 0;
                if (g_playing.load(std::memory_order_acquire)) got = std::max(SDL_GetAudioStreamData(stream, data, bytes), 0);
                if (got < bytes) std::memset(data + got, 0, size_t(bytes - got));
                render->ReleaseBuffer(frames, 0);
                if (volume.p && n % 128 == 0) {  // every few tenths of a second
                    BOOL mute = FALSE;
                    float db = 0;
                    volume->GetMute(&mute);
                    volume->GetMasterVolumeLevel(&db);
                    const float gain = mute ? 0.0f : soft_volume ? std::pow(10.0f, db / 20.0f) : 1.0f;
                    SDL_SetAudioStreamGain(stream, gain);
                }
            }
            client->Stop();
            if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
            CloseHandle(event);
            g_lost.store(true, std::memory_order_release);
        }
    done:;
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace

bool open(Opened& out, const char*& why) {
    g_playing = false, g_lost = false;
    std::promise<Result> ready;
    std::future<Result> answer = ready.get_future();
    std::thread(run, std::move(ready)).detach();
    const Result r = answer.get();
    if (!r.ok) {
        why = r.why;
        return false;
    }
    out = r.opened;
    return true;
}

void start() { g_playing.store(true, std::memory_order_release); }
bool lost() { return g_lost.load(std::memory_order_acquire); }

}  // namespace qilin::rt::host::wasapi
