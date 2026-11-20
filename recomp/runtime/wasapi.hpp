// Windows: the sound device in WASAPI's exclusive mode (wasapi.cpp), for the
// least latency the device allows (a USB interface: 3 ms periods where the
// shared mode gives 10). SDL has no exclusive mode, so this is the runtime's
// own; SDL still converts and resamples (an SDL audio stream with no device,
// which this pulls from). Runtime code: no std::format.
#pragma once

struct SDL_AudioStream;

namespace qilin::rt::host::wasapi {

struct Opened {
    SDL_AudioStream* stream = nullptr;  // put 32 kHz 16-bit stereo here; the device's thread takes it
    int rate = 0, channels = 0;         // the device's format
    double period_ms = 0;               // one period (a buffer the device takes at a time)
    const char* sample = "";            // the sample type, for the log
};

// Opens the default output exclusively, its thread running and playing
// silence until start(). False (with the reason in `why`) if the device
// can't be had: in use by another program exclusively, or no format it
// takes that SDL can write; the caller uses the shared mode then.
bool open(Opened& out, const char*& why);
// The queue has reached its target: play what is in the stream.
void start();
// The device went away (unplugged, the default changed): the stream is no
// longer taken, and the host opens the output again.
bool lost();

}  // namespace qilin::rt::host::wasapi
