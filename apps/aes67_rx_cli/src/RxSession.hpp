// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ravennakit/ravenna/ravenna_node.hpp"
#include "ravennakit/ravenna/ravenna_receiver.hpp"

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <portaudio.h>

namespace app {

struct RxConfig {
    std::string interfaces;         // comma-separated for ravennakit parser
    // PortAudio output device name (see --list-audio-devices). Empty = PortAudio default output device.
    // Note: PortAudio uses ALSA backend on Linux (Option A).
    std::string audio_device;
    uint32_t playout_delay_frames = 240;
    std::string nmos_registry_url;  // If set, enable NMOS node and register to this registry
    uint16_t nmos_api_port = 0;     // NMOS node API port (0 = auto-assign)
};

// Owns a RavennaNode + a single receiver, and provides PortAudio playback (ALSA backend on Linux).
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
    void ravenna_receiver_stream_state_updated(
        const rav::rtp::AudioReceiver::StreamInfo& stream_info, const rav::rtp::AudioReceiver::StreamState state) override;

    // ptp::Instance::Subscriber - PTP status callbacks
    void ptp_parent_changed(const rav::ptp::ParentDs& parent) override;

  private:
    rav::RavennaNode node_;
    RxConfig cfg_;
    rav::Id receiver_id_;
    rav::AudioFormat audio_format_;
    bool started_ = false;

    // PortAudio playout (callback-driven)
    void* pa_stream_ = nullptr; // PaStream*
    void start_portaudio_or_throw();
    void stop_portaudio();
    static int portaudio_stream_callback(
        const void* input,
        void* output,
        unsigned long frame_count,
        const PaStreamCallbackTimeInfo* time_info,
        PaStreamCallbackFlags status_flags,
        void* user_data
    );

    // Signal level monitoring
    std::atomic<double> signal_rms_db_ {std::numeric_limits<double>::quiet_NaN()};
    std::atomic<double> signal_peak_db_ {std::numeric_limits<double>::quiet_NaN()};
    std::atomic<double> signal_max_abs_ {std::numeric_limits<double>::quiet_NaN()};
    std::atomic<uint64_t> buffers_with_data_ {0};
    std::atomic<uint64_t> buffers_no_data_ {0};
    
    // PTP clock status tracking
    std::atomic<bool> clock_valid_ {false};
    std::atomic<bool> clock_locked_ {false};
    std::atomic<bool> clock_calibrated_ {false};
    
    std::thread stats_thread_;
    std::atomic<bool> stats_keep_going_ {false};
    void start_stats_thread();
    void stop_stats_thread();
    static double calculate_rms_db(const uint8_t* data, size_t bytes, const rav::AudioFormat& fmt);
    static double calculate_max_abs(const uint8_t* data, size_t bytes, const rav::AudioFormat& fmt);
};

}  // namespace app


