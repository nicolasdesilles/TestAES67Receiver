// SPDX-License-Identifier: AGPL-3.0-or-later

#include "RxSession.hpp"

#include "ravennakit/core/clock.hpp"
#include "ravennakit/core/byte_order.hpp"
#include "ravennakit/core/exception.hpp"
#include "ravennakit/core/log.hpp"
#include "ravennakit/core/system.hpp"
#include "ravennakit/core/util/tracy.hpp"
#include "ravennakit/core/util/wrapping_uint.hpp"
#include "ravennakit/core/net/interfaces/network_interface_config.hpp"

#include <portaudio.h>

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace app {

namespace {

constexpr unsigned long k_block_size = 32;  // frames

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

std::optional<PaDeviceIndex> find_device_index_by_name(const std::string& device_name) {
    const auto num = Pa_GetDeviceCount();
    if (num < 0) {
        throw std::runtime_error(std::string("PortAudio device count failed: ") + Pa_GetErrorText(num));
    }
    for (PaDeviceIndex i = 0; i < num; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || !info->name) {
            continue;
        }
        if (device_name == info->name) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<PaSampleFormat> pa_format_for_audio_format(const rav::AudioFormat& f) {
    // Keep in sync with supported encodings in rav::RavennaReceiver.
    if (f.encoding == rav::AudioEncoding::pcm_s16) {
        return paInt16;
    }
    if (f.encoding == rav::AudioEncoding::pcm_s24) {
        return paInt24;
    }
    if (f.encoding == rav::AudioEncoding::pcm_s32) {
        return paInt32;
    }
    if (f.encoding == rav::AudioEncoding::pcm_s8) {
        return paInt8;
    }
    if (f.encoding == rav::AudioEncoding::pcm_u8) {
        return paUInt8;
    }
    return std::nullopt;
}

}  // namespace

class RxSession::PortAudioStream {
  public:
    PortAudioStream() { PortAudioInit::ensure(); }
    ~PortAudioStream() { close(); }

    void open_output_by_name(
        const std::string& device_name,
        double sample_rate,
        int channels,
        PaSampleFormat sample_format,
        PaStreamCallback* callback,
        void* user_data
    ) {
        close();

        PaDeviceIndex device = Pa_GetDefaultOutputDevice();
        if (!device_name.empty()) {
            const auto idx = find_device_index_by_name(device_name);
            if (!idx) {
                throw std::runtime_error("Audio device not found: " + device_name);
            }
            device = *idx;
        }
        if (device == paNoDevice) {
            throw std::runtime_error("No default audio output device found");
        }

        PaStreamParameters out;
        std::memset(&out, 0, sizeof(out));
        out.device = device;
        out.channelCount = channels;
        out.sampleFormat = sample_format;
        out.suggestedLatency = Pa_GetDeviceInfo(device)->defaultLowOutputLatency;
        out.hostApiSpecificStreamInfo = nullptr;

        const auto err = Pa_OpenStream(
            &stream_,
            nullptr,
            &out,
            sample_rate,
            k_block_size,
            paNoFlag,
            callback,
            user_data
        );
        if (err != paNoError) {
            throw std::runtime_error(std::string("PortAudio open failed: ") + Pa_GetErrorText(err));
        }

        const auto start_err = Pa_StartStream(stream_);
        if (start_err != paNoError) {
            throw std::runtime_error(std::string("PortAudio start failed: ") + Pa_GetErrorText(start_err));
        }
    }

    void close() {
        if (!stream_) {
            return;
        }
        (void)Pa_StopStream(stream_);
        (void)Pa_CloseStream(stream_);
        stream_ = nullptr;
    }

  private:
    PaStream* stream_ = nullptr;
};

RxSession::RxSession() {
    rav::set_log_level_from_env();
    rav::do_system_checks();
}

RxSession::~RxSession() {
    stop();
}

void RxSession::start_from_sdp(const rav::sdp::SessionDescription& sdp, const RxConfig& cfg) {
    if (started_) {
        stop();
    }
    cfg_ = cfg;

    const auto nic = rav::parse_network_interface_config_from_string(cfg.interfaces);
    if (!nic) {
        throw std::runtime_error("Failed to parse --interfaces (must match a system interface identifier/name/MAC/IP)");
    }

    node_.set_network_interface_config(*nic).wait();

    rav::RavennaReceiver::Configuration rcfg;
    rcfg.sdp = sdp;
    rcfg.session_name = sdp.session_name.empty() ? std::string("rx") : sdp.session_name;
    rcfg.enabled = true;
    rcfg.auto_update_sdp = false;
    rcfg.delay_frames = cfg_.playout_delay_frames;

    auto id_res = node_.create_receiver(rcfg).get();
    if (!id_res) {
        throw std::runtime_error("Failed to create receiver: " + id_res.error());
    }
    receiver_id_ = *id_res;

    node_.subscribe_to_receiver(receiver_id_, this).wait();
    node_.subscribe_to_ptp_instance(this).wait();

    pa_stream_ = std::make_unique<PortAudioStream>();
    started_ = true;
}

void RxSession::stop() {
    if (!started_) {
        return;
    }

    node_.unsubscribe_from_ptp_instance(this).wait();
    if (receiver_id_.is_valid()) {
        node_.unsubscribe_from_receiver(receiver_id_, this).wait();
        node_.remove_receiver(receiver_id_).wait();
        receiver_id_ = {};
    }

    if (pa_stream_) {
        pa_stream_.reset();
    }

    started_ = false;
}

void RxSession::ravenna_receiver_parameters_updated(const rav::rtp::AudioReceiver::ReaderParameters& parameters) {
    if (parameters.streams.empty()) {
        RAV_LOG_WARNING("Receiver has no streams yet");
        return;
    }

    if (!parameters.audio_format.is_valid()) {
        return;
    }

    if (audio_format_ == parameters.audio_format) {
        return;
    }

    audio_format_ = parameters.audio_format;

    const auto pa_format = pa_format_for_audio_format(audio_format_);
    if (!pa_format) {
        RAV_LOG_ERROR("Unsupported audio format for PortAudio: {}", audio_format_.to_string());
        return;
    }

    try {
        pa_stream_->open_output_by_name(
            cfg_.audio_device_name,
            static_cast<double>(audio_format_.sample_rate),
            static_cast<int>(audio_format_.num_channels),
            *pa_format,
            &RxSession::pa_callback_trampoline,
            this
        );
        RAV_LOG_INFO(
            "Audio started: {}Hz, {}ch, {}, device='{}'",
            audio_format_.sample_rate,
            audio_format_.num_channels,
            audio_format_.to_string(),
            cfg_.audio_device_name.empty() ? "<default>" : cfg_.audio_device_name
        );
    } catch (const std::exception& e) {
        RAV_LOG_ERROR("Failed to open audio output: {}", e.what());
    }
}

int RxSession::pa_callback(const void* input, void* output, const unsigned long frame_count) {
    TRACY_ZONE_SCOPED;

    std::ignore = input;

    if (!audio_format_.is_valid() || !receiver_id_.is_valid()) {
        return paContinue;
    }

    const auto buffer_size = static_cast<size_t>(frame_count) * audio_format_.bytes_per_frame();

    auto& local_clock = get_local_clock();
    if (!local_clock.is_calibrated()) {
        std::memset(output, audio_format_.ground_value(), buffer_size);
        return paContinue;
    }

    const auto ptp_ts =
        local_clock.now().to_rtp_timestamp32(audio_format_.sample_rate) - static_cast<uint32_t>(cfg_.playout_delay_frames);

    // First read: use receiver's internal playout alignment.
    auto rtp_ts = node_.read_data_realtime(receiver_id_, static_cast<uint8_t*>(output), buffer_size, {}, {});

    if (!rtp_ts) {
        std::memset(output, audio_format_.ground_value(), buffer_size);
        return paContinue;
    }

    auto drift = rav::WrappingUint32(ptp_ts).diff(rav::WrappingUint32(*rtp_ts));

    // If drift is too big, re-align by forcing the target timestamp.
    if (static_cast<uint32_t>(std::abs(drift)) > frame_count * 2) {
        rtp_ts = node_.read_data_realtime(receiver_id_, static_cast<uint8_t*>(output), buffer_size, ptp_ts, {});
        RAV_LOG_WARNING("Re-aligned stream by {} samples", -drift);
        if (rtp_ts) {
            drift = rav::WrappingUint32(ptp_ts).diff(rav::WrappingUint32(*rtp_ts));
        }
    }

    TRACY_PLOT("drift", static_cast<double>(drift));

    if (audio_format_.byte_order == rav::AudioFormat::ByteOrder::be) {
        rav::swap_bytes(static_cast<uint8_t*>(output), buffer_size, audio_format_.bytes_per_sample());
    }

    return paContinue;
}

int RxSession::pa_callback_trampoline(
    const void* input,
    void* output,
    const unsigned long frame_count,
    const PaStreamCallbackTimeInfo* time_info,
    const PaStreamCallbackFlags status_flags,
    void* user_data
) {
    std::ignore = time_info;
    std::ignore = status_flags;
    auto* self = static_cast<RxSession*>(user_data);
    return self->pa_callback(input, output, frame_count);
}

}  // namespace app


