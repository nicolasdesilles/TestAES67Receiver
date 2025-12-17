// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ravennakit/ravenna/ravenna_node.hpp"
#include "ravennakit/ravenna/ravenna_receiver.hpp"

#include <optional>
#include <string>

struct PaStreamCallbackTimeInfo;
using PaStreamCallbackFlags = unsigned long;

namespace app {

struct RxConfig {
    std::string interfaces;         // comma-separated for ravennakit parser
    std::string audio_device_name;  // PortAudio device name (as shown in list)
    uint32_t playout_delay_frames = 240;
};

// Owns a RavennaNode + a single receiver, and provides a PortAudio callback for playout.
class RxSession final: public rav::RavennaReceiver::Subscriber, public rav::ptp::Instance::Subscriber {
  public:
    RxSession();
    ~RxSession() override;

    RxSession(const RxSession&) = delete;
    RxSession& operator=(const RxSession&) = delete;

    void start_from_sdp(const rav::sdp::SessionDescription& sdp, const RxConfig& cfg);
    void stop();

    // ravenna_receiver::subscriber
    void ravenna_receiver_parameters_updated(const rav::rtp::AudioReceiver::ReaderParameters& parameters) override;

    // PortAudio stream callback
    int pa_callback(const void* input, void* output, unsigned long frame_count);

  private:
    class PortAudioStream;
    static int pa_callback_trampoline(
        const void* input,
        void* output,
        unsigned long frame_count,
        const PaStreamCallbackTimeInfo* time_info,
        PaStreamCallbackFlags status_flags,
        void* user_data
    );

    rav::RavennaNode node_;
    RxConfig cfg_;
    rav::Id receiver_id_;
    rav::AudioFormat audio_format_;
    std::unique_ptr<PortAudioStream> pa_stream_;
    bool started_ = false;
};

}  // namespace app


