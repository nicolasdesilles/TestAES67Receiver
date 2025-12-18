// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef __APPLE__

#include "AudioOutput.hpp"

#include <alsa/asoundlib.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app {

namespace {

class AlsaOutput final: public AudioOutput {
  public:
    std::vector<AudioDeviceInfo> list_output_devices() override {
        std::vector<AudioDeviceInfo> out;

        void** hints = nullptr;
        if (snd_device_name_hint(-1, "pcm", &hints) != 0 || hints == nullptr) {
            return out;
        }

        int idx = 0;
        for (void** it = hints; *it != nullptr; ++it) {
            const char* name = snd_device_name_get_hint(*it, "NAME");
            const char* desc = snd_device_name_get_hint(*it, "DESC");
            const char* ioid = snd_device_name_get_hint(*it, "IOID");

            // If IOID is set, keep only Output.
            if (ioid && std::string(ioid) != "Output") {
                if (name) free((void*)name);
                if (desc) free((void*)desc);
                free((void*)ioid);
                continue;
            }

            if (name) {
                AudioDeviceInfo d;
                d.index = idx++;
                d.name = std::string(name) + (desc ? (std::string("  ") + std::string(desc)) : "");
                d.max_output_channels = 0;
                out.push_back(std::move(d));
            }

            if (name) free((void*)name);
            if (desc) free((void*)desc);
            if (ioid) free((void*)ioid);
        }

        snd_device_name_free_hint(hints);
        return out;
    }

    std::optional<AudioDeviceInfo> default_output_device() override {
        // ALSA default is typically "default"; caller can override.
        AudioDeviceInfo d;
        d.index = 0;
        d.name = "default";
        return d;
    }
};

}  // namespace

std::unique_ptr<AudioOutput> create_portaudio_output() {
    // Factory name kept for compatibility; returns ALSA implementation.
    return std::make_unique<AlsaOutput>();
}

}  // namespace app

#endif  // !__APPLE__


