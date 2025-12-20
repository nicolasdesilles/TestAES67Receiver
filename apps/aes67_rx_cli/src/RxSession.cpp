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
#include "ravennakit/nmos/nmos_node.hpp"

#include <portaudio.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#include <fmt/core.h>

namespace app {

namespace {
constexpr unsigned long k_block_size_frames = 256; // frames per PortAudio callback

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

static std::optional<PaDeviceIndex> find_output_device_index_by_name(const std::string& name) {
    const auto n = Pa_GetDeviceCount();
    if (n < 0) {
        throw std::runtime_error(std::string("PortAudio device count failed: ") + Pa_GetErrorText(n));
    }
    for (PaDeviceIndex i = 0; i < n; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        if (name == info->name) {
            return i;
        }
    }
    return std::nullopt;
}

static std::optional<PaDeviceIndex> find_output_device_index_by_substring(const std::string& needle) {
    const auto n = Pa_GetDeviceCount();
    if (n < 0) {
        throw std::runtime_error(std::string("PortAudio device count failed: ") + Pa_GetErrorText(n));
    }
    for (PaDeviceIndex i = 0; i < n; ++i) {
        const auto* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0 || !info->name) {
            continue;
        }
        if (std::string(info->name).find(needle) != std::string::npos) {
            return i;
        }
    }
    return std::nullopt;
}

static std::optional<PaSampleFormat> pa_format_for_rav_format(const rav::AudioFormat& fmt) {
    switch (fmt.encoding) {
        case rav::AudioEncoding::pcm_u8:
            return paUInt8;
        case rav::AudioEncoding::pcm_s8:
            return paInt8;
        case rav::AudioEncoding::pcm_s16:
            return paInt16;
        case rav::AudioEncoding::pcm_s24:
            return paInt24;
        case rav::AudioEncoding::pcm_s32:
            return paInt32;
        default:
            return std::nullopt;
    }
}
}  // namespace

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

    // Log the parsed network interface config for debugging
    RAV_LOG_INFO("Network interface config: {}", nic->to_string());
    const auto iface_addrs = nic->get_interface_ipv4_addresses();
    for (size_t i = 0; i < iface_addrs.size(); ++i) {
        RAV_LOG_INFO("  Interface[{}] IPv4: {}", i, iface_addrs[i].to_string());
    }

    try {
        node_.set_network_interface_config(*nic).get();  // Use .get() to rethrow exceptions
        RAV_LOG_INFO("Network interface config set successfully (PTP ports should be created on 319/320)");
    } catch (const std::exception& e) {
        RAV_LOG_ERROR("Failed to set network interface config: {} (likely permission denied for PTP ports 319/320 - try running with sudo)", e.what());
        throw;
    }

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

    // Configure NMOS node if registry URL is provided
    if (!cfg.nmos_registry_url.empty()) {
        rav::nmos::Node::Configuration nmos_cfg;
        nmos_cfg.id = boost::uuids::random_generator()();
        nmos_cfg.enabled = true;
        nmos_cfg.api_port = cfg.nmos_api_port;
        nmos_cfg.label = "AES67 Receiver CLI";
        nmos_cfg.description = "RAVENNA/AES67 receiver for " + rcfg.session_name;
        nmos_cfg.operation_mode = rav::nmos::OperationMode::manual;
        nmos_cfg.registry_address = cfg.nmos_registry_url;
        nmos_cfg.api_version = rav::nmos::ApiVersion {1, 3};

        auto nmos_result = node_.set_nmos_configuration(nmos_cfg).get();
        if (!nmos_result) {
            RAV_LOG_WARNING("Failed to configure NMOS node (will continue without registration): {}", nmos_result.error());
        } else {
            RAV_LOG_INFO("NMOS node configured, registering to registry: {}", cfg.nmos_registry_url);
        }
    }

    start_stats_thread();
    started_ = true;
}

void RxSession::stop() {
    if (!started_) {
        return;
    }

    stop_stats_thread();
    stop_portaudio();

    node_.unsubscribe_from_ptp_instance(this).wait();
    if (receiver_id_.is_valid()) {
        node_.unsubscribe_from_receiver(receiver_id_, this).wait();
        node_.remove_receiver(receiver_id_).wait();
        receiver_id_ = {};
    }

    started_ = false;
}

void RxSession::ravenna_receiver_parameters_updated(const rav::rtp::AudioReceiver::ReaderParameters& parameters) {
    // Keep this callback lightweight; it may be called from the ravennakit maintenance thread.
    if (parameters.streams.empty()) {
        RAV_LOG_WARNING("Receiver has no streams yet");
        return;
    }

    // Log stream info for debugging
    for (size_t i = 0; i < parameters.streams.size(); ++i) {
        const auto& stream = parameters.streams[i];
        if (!stream.is_valid()) {
            continue;
        }
        RAV_LOG_INFO(
            "Stream[{}]: connection_addr={}, rtp_port={}, packet_time_frames={}",
            i,
            stream.session.connection_address.to_string(),
            stream.session.rtp_port,
            stream.packet_time_frames
        );
    }

    if (!parameters.audio_format.is_valid()) {
        return;
    }

    if (audio_format_ == parameters.audio_format) {
        return;
    }

    audio_format_ = parameters.audio_format;

    try {
        stop_portaudio();
        start_portaudio_or_throw();
        RAV_LOG_INFO(
            "PortAudio started: {}Hz, {}ch, {}, device='{}'",
            audio_format_.sample_rate,
            audio_format_.num_channels,
            audio_format_.to_string(),
            cfg_.audio_device.empty() ? std::string("<default>") : cfg_.audio_device
        );
    } catch (const std::exception& e) {
        RAV_LOG_ERROR("Failed to start PortAudio output: {}", e.what());
    }
}

void RxSession::ravenna_receiver_stream_state_updated(
    const rav::rtp::AudioReceiver::StreamInfo& stream_info, const rav::rtp::AudioReceiver::StreamState state) {
    const char* state_str = "unknown";
    switch (state) {
        case rav::rtp::AudioReceiver::StreamState::inactive:
            state_str = "inactive";
            break;
        case rav::rtp::AudioReceiver::StreamState::receiving:
            state_str = "receiving";
            break;
        case rav::rtp::AudioReceiver::StreamState::no_consumer:
            state_str = "no_consumer";
            break;
    }
    RAV_LOG_INFO(
        "Stream state changed: {} -> {} (addr={}, port={})",
        state_str,
        state_str,
        stream_info.session.connection_address.to_string(),
        stream_info.session.rtp_port
    );
}

void RxSession::ptp_parent_changed(const rav::ptp::ParentDs& parent) {
    RAV_LOG_INFO(
        "PTP parent changed: GM identity={}, priority1={}, priority2={}",
        parent.grandmaster_identity.to_string(),
        parent.grandmaster_priority1,
        parent.grandmaster_priority2
    );
}

void RxSession::stop_portaudio() {
    if (!pa_stream_) {
        return;
    }
    auto* s = static_cast<PaStream*>(pa_stream_);
    (void)Pa_StopStream(s);
    (void)Pa_CloseStream(s);
    pa_stream_ = nullptr;
}

int RxSession::portaudio_stream_callback(
    const void* input,
    void* output,
    const unsigned long frame_count,
    const PaStreamCallbackTimeInfo* time_info,
    const PaStreamCallbackFlags status_flags,
    void* user_data
) {
    TRACY_ZONE_SCOPED;
    std::ignore = input;
    std::ignore = time_info;
    std::ignore = status_flags;

    auto* self = static_cast<RxSession*>(user_data);
    if (!self || !output) {
        return paAbort;
    }

    if (!self->receiver_id_.is_valid() || !self->audio_format_.is_valid()) {
        return paContinue;
    }

    const auto buffer_size = static_cast<size_t>(frame_count) * self->audio_format_.bytes_per_frame();

    auto& clock = self->get_local_clock();
    const bool clock_calibrated = clock.is_calibrated();

    // Track clock status for logging
    self->clock_valid_.store(clock.is_valid(), std::memory_order_relaxed);
    self->clock_locked_.store(clock.is_locked(), std::memory_order_relaxed);
    self->clock_calibrated_.store(clock_calibrated, std::memory_order_relaxed);

    if (!clock_calibrated) {
        std::memset(output, self->audio_format_.ground_value(), buffer_size);
        self->buffers_no_data_.fetch_add(1, std::memory_order_relaxed);
        self->signal_rms_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        self->signal_peak_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        self->signal_max_abs_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        return paContinue;
    }

    const auto ptp_ts = clock.now().to_rtp_timestamp32(self->audio_format_.sample_rate) - self->cfg_.playout_delay_frames;

    // First try to read any data
    auto rtp_ts = self->node_.read_data_realtime(self->receiver_id_, static_cast<uint8_t*>(output), buffer_size, {}, {});
    if (!rtp_ts) {
        std::memset(output, self->audio_format_.ground_value(), buffer_size);
        self->buffers_no_data_.fetch_add(1, std::memory_order_relaxed);
        self->signal_rms_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        self->signal_peak_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        self->signal_max_abs_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
        return paContinue;
    }

    self->buffers_with_data_.fetch_add(1, std::memory_order_relaxed);

    // Drift correction (same approach as ravennakit example)
    auto drift = rav::WrappingUint32(ptp_ts).diff(rav::WrappingUint32(*rtp_ts));
    if (static_cast<uint32_t>(std::abs(drift)) > frame_count * 2) {
        (void)self->node_.read_data_realtime(self->receiver_id_, static_cast<uint8_t*>(output), buffer_size, ptp_ts, {});
    }

    if (self->audio_format_.byte_order == rav::AudioFormat::ByteOrder::be) {
        rav::swap_bytes(static_cast<uint8_t*>(output), buffer_size, self->audio_format_.bytes_per_sample());
    }

    // Signal monitoring (after swap, still same numeric values)
    const double rms_db = RxSession::calculate_rms_db(static_cast<const uint8_t*>(output), buffer_size, self->audio_format_);
    const double max_abs = RxSession::calculate_max_abs(static_cast<const uint8_t*>(output), buffer_size, self->audio_format_);
    self->signal_rms_db_.store(rms_db, std::memory_order_relaxed);
    self->signal_peak_db_.store(rms_db + 3.0, std::memory_order_relaxed);
    self->signal_max_abs_.store(max_abs, std::memory_order_relaxed);

    return paContinue;
}

void RxSession::start_portaudio_or_throw() {
    stop_portaudio();

    if (!audio_format_.is_valid()) {
        throw std::runtime_error("Audio format not set yet");
    }

    PortAudioInit::ensure();

    const auto pa_fmt = pa_format_for_rav_format(audio_format_);
    if (!pa_fmt) {
        throw std::runtime_error("Unsupported PortAudio format for encoding: " + audio_format_.to_string());
    }

    PaStreamParameters out {};
    out.channelCount = static_cast<int>(audio_format_.num_channels);
    out.sampleFormat = *pa_fmt;
    out.hostApiSpecificStreamInfo = nullptr;

    if (!cfg_.audio_device.empty()) {
        // First try exact match, then substring match (more forgiving for ALSA device naming).
        auto idx = find_output_device_index_by_name(cfg_.audio_device);
        if (!idx) {
            idx = find_output_device_index_by_substring(cfg_.audio_device);
        }
        if (!idx) {
            throw std::runtime_error("PortAudio output device not found: " + cfg_.audio_device + " (use --list-audio-devices)");
        }
        out.device = *idx;
        out.suggestedLatency = Pa_GetDeviceInfo(*idx)->defaultLowOutputLatency;
    } else {
        out.device = Pa_GetDefaultOutputDevice();
        if (out.device == paNoDevice) {
            throw std::runtime_error("No PortAudio default output device available");
        }
        out.suggestedLatency = Pa_GetDeviceInfo(out.device)->defaultLowOutputLatency;
    }

    PaStream* stream = nullptr;
    const auto err = Pa_OpenStream(
        &stream, nullptr, &out, static_cast<double>(audio_format_.sample_rate), k_block_size_frames, paNoFlag,
        &RxSession::portaudio_stream_callback, this
    );
    if (err != paNoError) {
        throw std::runtime_error(std::string("Pa_OpenStream failed: ") + Pa_GetErrorText(err));
    }

    if (const auto start_err = Pa_StartStream(stream); start_err != paNoError) {
        (void)Pa_CloseStream(stream);
        throw std::runtime_error(std::string("Pa_StartStream failed: ") + Pa_GetErrorText(start_err));
    }

    pa_stream_ = stream;
}

void RxSession::start_stats_thread() {
    if (stats_thread_.joinable()) {
        return;
    }
    stats_keep_going_.store(true, std::memory_order_relaxed);
    stats_thread_ = std::thread([this]() {
        uint64_t prev_buffers_with_data = 0;
        uint64_t prev_buffers_no_data = 0;
        while (stats_keep_going_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const double rms = signal_rms_db_.load(std::memory_order_relaxed);
            const double peak = signal_peak_db_.load(std::memory_order_relaxed);
            const double max_abs = signal_max_abs_.load(std::memory_order_relaxed);
            const uint64_t data_buffers = buffers_with_data_.load(std::memory_order_relaxed);
            const uint64_t empty_buffers = buffers_no_data_.load(std::memory_order_relaxed);
            const uint64_t data_per_sec = data_buffers - prev_buffers_with_data;
            const uint64_t empty_per_sec = empty_buffers - prev_buffers_no_data;
            prev_buffers_with_data = data_buffers;
            prev_buffers_no_data = empty_buffers;
            
            // PTP clock status
            const bool clk_valid = clock_valid_.load(std::memory_order_relaxed);
            const bool clk_locked = clock_locked_.load(std::memory_order_relaxed);
            const bool clk_calibrated = clock_calibrated_.load(std::memory_order_relaxed);
            const std::string ptp_status = fmt::format(
                "PTP[valid={},locked={},cal={}]",
                clk_valid ? "Y" : "N",
                clk_locked ? "Y" : "N",
                clk_calibrated ? "Y" : "N"
            );

            if (std::isnan(rms) || std::isnan(peak)) {
                fmt::println(
                    "[Signal] RMS: --- dB (no data) | data/s={}, empty/s={} | {}",
                    data_per_sec,
                    empty_per_sec,
                    ptp_status
                );
            } else {
                fmt::println(
                    "[Signal] RMS: {:.1f} dB, Peak: {:.1f} dB, max_abs={:.3f} | data/s={}, empty/s={} | {}",
                    rms,
                    peak,
                    max_abs,
                    data_per_sec,
                    empty_per_sec,
                    ptp_status
                );
            }
        }
    });
}

void RxSession::stop_stats_thread() {
    stats_keep_going_.store(false, std::memory_order_relaxed);
    if (stats_thread_.joinable()) {
        stats_thread_.join();
    }
}

double RxSession::calculate_rms_db(const uint8_t* data, size_t bytes, const rav::AudioFormat& fmt) {
    if (bytes == 0 || !fmt.is_valid()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const size_t num_samples = bytes / fmt.bytes_per_sample();
    if (num_samples == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double sum_squares = 0.0;

    switch (fmt.encoding) {
        case rav::AudioEncoding::pcm_s16: {
            const int16_t* samples = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < num_samples; ++i) {
                const double sample = static_cast<double>(samples[i]) / 32768.0;
                sum_squares += sample * sample;
            }
            break;
        }
        case rav::AudioEncoding::pcm_s24: {
            // 24-bit packed (3 bytes per sample, little-endian)
            for (size_t i = 0; i < num_samples; ++i) {
                const int32_t sample_raw = static_cast<int32_t>(
                    static_cast<uint32_t>(data[i * 3]) | (static_cast<uint32_t>(data[i * 3 + 1]) << 8)
                    | (static_cast<uint32_t>(static_cast<int8_t>(data[i * 3 + 2])) << 16)
                );
                // Sign-extend
                const int32_t sample = (sample_raw << 8) >> 8;
                const double normalized = static_cast<double>(sample) / 8388608.0;
                sum_squares += normalized * normalized;
            }
            break;
        }
        case rav::AudioEncoding::pcm_s32: {
            const int32_t* samples = reinterpret_cast<const int32_t*>(data);
            for (size_t i = 0; i < num_samples; ++i) {
                const double sample = static_cast<double>(samples[i]) / 2147483648.0;
                sum_squares += sample * sample;
            }
            break;
        }
        default:
            return std::numeric_limits<double>::quiet_NaN();
    }

    const double rms = std::sqrt(sum_squares / static_cast<double>(num_samples));
    // Convert to dB (return NaN for digital silence to display as "--- dB")
    if (rms <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return 20.0 * std::log10(rms);
}

double RxSession::calculate_max_abs(const uint8_t* data, size_t bytes, const rav::AudioFormat& fmt) {
    if (bytes == 0 || !fmt.is_valid()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const size_t num_samples = bytes / fmt.bytes_per_sample();
    if (num_samples == 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    double max_abs = 0.0;

    switch (fmt.encoding) {
        case rav::AudioEncoding::pcm_s16: {
            const int16_t* samples = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < num_samples; ++i) {
                const double normalized = static_cast<double>(samples[i]) / 32768.0;
                max_abs = std::max(max_abs, std::abs(normalized));
            }
            break;
        }
        case rav::AudioEncoding::pcm_s24: {
            for (size_t i = 0; i < num_samples; ++i) {
                const int32_t sample_raw = static_cast<int32_t>(
                    static_cast<uint32_t>(data[i * 3]) | (static_cast<uint32_t>(data[i * 3 + 1]) << 8)
                    | (static_cast<uint32_t>(static_cast<int8_t>(data[i * 3 + 2])) << 16)
                );
                const int32_t sample = (sample_raw << 8) >> 8;
                const double normalized = static_cast<double>(sample) / 8388608.0;
                max_abs = std::max(max_abs, std::abs(normalized));
            }
            break;
        }
        case rav::AudioEncoding::pcm_s32: {
            const int32_t* samples = reinterpret_cast<const int32_t*>(data);
            for (size_t i = 0; i < num_samples; ++i) {
                const double normalized = static_cast<double>(samples[i]) / 2147483648.0;
                max_abs = std::max(max_abs, std::abs(normalized));
            }
            break;
        }
        default:
            return std::numeric_limits<double>::quiet_NaN();
    }

    return max_abs;
}

}  // namespace app


