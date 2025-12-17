// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AudioOutput.hpp"

#include <portaudio.h>

#include <functional>
#include <stdexcept>

namespace app {

namespace {

class PortAudioInit {
  public:
    static void ensure() {
        static PortAudioInit instance;
        (void)instance;
    }

  private:
    PortAudioInit() {
        if (const auto err = Pa_Initialize(); err != paNoError) {
            throw std::runtime_error(std::string("PortAudio init failed: ") + Pa_GetErrorText(err));
        }
    }

    ~PortAudioInit() {
        (void)Pa_Terminate();
    }
};

}  // namespace

class PortAudioOutput final: public AudioOutput {
  public:
    std::vector<AudioDeviceInfo> list_output_devices() override {
        PortAudioInit::ensure();
        std::vector<AudioDeviceInfo> out;

        const auto num = Pa_GetDeviceCount();
        if (num < 0) {
            throw std::runtime_error(std::string("PortAudio device count failed: ") + Pa_GetErrorText(num));
        }

        for (PaDeviceIndex i = 0; i < num; ++i) {
            const auto* info = Pa_GetDeviceInfo(i);
            if (!info) {
                continue;
            }
            if (info->maxOutputChannels <= 0) {
                continue;
            }
            AudioDeviceInfo d;
            d.index = static_cast<int>(i);
            d.name = info->name ? info->name : "";
            d.max_output_channels = info->maxOutputChannels;
            out.push_back(std::move(d));
        }
        return out;
    }

    std::optional<AudioDeviceInfo> default_output_device() override {
        PortAudioInit::ensure();
        const auto idx = Pa_GetDefaultOutputDevice();
        if (idx == paNoDevice) {
            return std::nullopt;
        }
        const auto* info = Pa_GetDeviceInfo(idx);
        if (!info) {
            return std::nullopt;
        }
        AudioDeviceInfo d;
        d.index = static_cast<int>(idx);
        d.name = info->name ? info->name : "";
        d.max_output_channels = info->maxOutputChannels;
        return d;
    }
};

std::unique_ptr<AudioOutput> create_portaudio_output() {
    return std::make_unique<PortAudioOutput>();
}

}  // namespace app


