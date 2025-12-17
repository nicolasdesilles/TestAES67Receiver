// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ravennakit/core/audio/audio_format.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace app {

struct AudioDeviceInfo {
    int index = -1;
    std::string name;
    int max_output_channels = 0;
};

class AudioOutput {
  public:
    virtual ~AudioOutput() = default;

    [[nodiscard]] virtual std::vector<AudioDeviceInfo> list_output_devices() = 0;
    [[nodiscard]] virtual std::optional<AudioDeviceInfo> default_output_device() = 0;
};

// Factory for the default implementation (PortAudio -> ALSA on Linux).
[[nodiscard]] std::unique_ptr<AudioOutput> create_portaudio_output();

}  // namespace app


