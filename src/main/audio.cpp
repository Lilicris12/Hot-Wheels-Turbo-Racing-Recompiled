// Audio output: receives the stereo 16-bit samples the game's audio driver writes through
// osAiSetNextBuffer and plays them with SDL, resampling to the device rate.

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <vector>

#include "hwtr/hwtr.hpp"

#include "SDL.h"

namespace {
    SDL_AudioDeviceID audio_device = 0;
    SDL_AudioStream* audio_stream = nullptr;

    // Game (input) sample rate, set by osAiSetFrequency. libmus games typically run at 22050 Hz.
    uint32_t sample_rate = 22050;
    uint32_t output_sample_rate = 48000;
    constexpr uint32_t channels = 2;
    constexpr uint32_t bytes_per_output_frame = channels * sizeof(float);

    void rebuild_stream() {
        if (audio_stream != nullptr) {
            SDL_FreeAudioStream(audio_stream);
        }
        audio_stream = SDL_NewAudioStream(AUDIO_F32, channels, sample_rate, AUDIO_F32, channels, output_sample_rate);
        if (audio_stream == nullptr) {
            throw std::runtime_error(std::string("Failed to create SDL audio stream: ") + SDL_GetError());
        }
    }
}

void hwtr::audio::init() {
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_AudioSpec desired{};
    desired.freq = (int)output_sample_rate;
    desired.format = AUDIO_F32;
    desired.channels = channels;
    desired.samples = 0x100; // Small buffer to keep latency low.

    SDL_AudioSpec obtained{};
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (audio_device == 0) {
        fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
        return;
    }
    output_sample_rate = obtained.freq;
    rebuild_stream();
    SDL_PauseAudioDevice(audio_device, 0);
}

void hwtr::audio::set_frequency(uint32_t freq) {
    if (freq == 0 || freq == sample_rate) {
        return;
    }
    sample_rate = freq;
    if (audio_device != 0) {
        rebuild_stream();
    }
}

void hwtr::audio::queue_samples(int16_t* audio_data, size_t sample_count) {
    if (audio_device == 0) {
        return;
    }

    static std::vector<float> converted;
    static std::vector<uint8_t> resampled;
    converted.resize(sample_count);

    const float volume = std::clamp(hwtr::config().master_volume, 0, 100) / 100.0f;

    // Samples arrive as big-endian stereo words that the runtime has already swapped to host
    // order as 32-bit units, so the left/right halfwords are reversed. Swap them back.
    for (size_t i = 0; i + 1 < sample_count; i += channels) {
        converted[i + 0] = audio_data[i + 1] * (1.0f / 32768.0f) * volume;
        converted[i + 1] = audio_data[i + 0] * (1.0f / 32768.0f) * volume;
    }

    SDL_AudioStreamPut(audio_stream, converted.data(), (int)(sample_count * sizeof(float)));
    int available = SDL_AudioStreamAvailable(audio_stream);
    if (available <= 0) {
        return;
    }
    resampled.resize(available);
    int got = SDL_AudioStreamGet(audio_stream, resampled.data(), available);
    if (got <= 0) {
        return;
    }

    // Drop audio instead of building latency if the game produces samples faster than they play
    // (e.g. after a hitch). Keep at most ~100 ms queued.
    uint32_t queued = SDL_GetQueuedAudioSize(audio_device);
    uint32_t max_queued = output_sample_rate / 10 * bytes_per_output_frame;
    if (queued > max_queued) {
        return;
    }

    SDL_QueueAudio(audio_device, resampled.data(), (Uint32)got);
}

size_t hwtr::audio::get_frames_remaining() {
    if (audio_device == 0) {
        return 0;
    }
    // Convert queued output bytes to input frames so the game's buffer pacing stays accurate.
    uint64_t queued_output_frames = SDL_GetQueuedAudioSize(audio_device) / bytes_per_output_frame;
    uint64_t queued_input_frames = queued_output_frames * sample_rate / output_sample_rate;

    // Report one VI less than what's buffered so the game stays slightly ahead, which avoids
    // underruns when the audio thread is briefly delayed.
    uint64_t frames_per_vi = sample_rate / 60;
    return queued_input_frames > frames_per_vi ? (size_t)(queued_input_frames - frames_per_vi) : 0;
}
