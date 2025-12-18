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

#include <alsa/asoundlib.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fmt/core.h>

namespace app {

namespace {
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

    // ALSA will be opened once we know the stream audio format.
    close_alsa();

    // ALSA will be opened once we know the stream audio format.
    close_alsa();

    start_stats_thread();
    started_ = true;
}

void RxSession::stop() {
    if (!started_) {
        return;
    }

    stop_stats_thread();
    stop_audio_thread();
    close_alsa();

    node_.unsubscribe_from_ptp_instance(this).wait();
    if (receiver_id_.is_valid()) {
        node_.unsubscribe_from_receiver(receiver_id_, this).wait();
        node_.remove_receiver(receiver_id_).wait();
        receiver_id_ = {};
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

    try {
        stop_audio_thread();
        open_alsa_or_throw();
        start_audio_thread();
        RAV_LOG_INFO(
            "ALSA started: {}Hz, {}ch, {}, device='{}'",
            audio_format_.sample_rate,
            audio_format_.num_channels,
            audio_format_.to_string(),
            cfg_.alsa_device
        );
    } catch (const std::exception& e) {
        RAV_LOG_ERROR("Failed to open ALSA output: {}", e.what());
    }
}

void RxSession::close_alsa() {
    if (!alsa_pcm_) {
        return;
    }
    auto* pcm = static_cast<snd_pcm_t*>(alsa_pcm_);
    snd_pcm_close(pcm);
    alsa_pcm_ = nullptr;
}

void RxSession::open_alsa_or_throw() {
    close_alsa();

    if (!audio_format_.is_valid()) {
        throw std::runtime_error("Audio format not set yet");
    }

    snd_pcm_t* pcm = nullptr;
    const int open_err = snd_pcm_open(&pcm, cfg_.alsa_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (open_err < 0) {
        throw std::runtime_error(std::string("snd_pcm_open failed: ") + snd_strerror(open_err));
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_channels(pcm, hw, static_cast<unsigned int>(audio_format_.num_channels));

    unsigned int rate = static_cast<unsigned int>(audio_format_.sample_rate);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, nullptr);

    // Try format negotiation with fallbacks (many embedded codecs don't support all formats).
    snd_pcm_format_t fmt = SND_PCM_FORMAT_UNKNOWN;
    std::vector<snd_pcm_format_t> format_candidates;

    switch (audio_format_.encoding) {
        case rav::AudioEncoding::pcm_s16:
            format_candidates = {SND_PCM_FORMAT_S16_LE};
            break;
        case rav::AudioEncoding::pcm_s24:
            // Many codecs don't support S24_3LE; try S32_LE (pad MSB) then S16_LE (truncate) as fallbacks.
            format_candidates = {SND_PCM_FORMAT_S24_3LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE};
            break;
        case rav::AudioEncoding::pcm_s32:
            format_candidates = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE};
            break;
        case rav::AudioEncoding::pcm_s8:
            format_candidates = {SND_PCM_FORMAT_S8};
            break;
        case rav::AudioEncoding::pcm_u8:
            format_candidates = {SND_PCM_FORMAT_U8};
            break;
        default:
            snd_pcm_close(pcm);
            throw std::runtime_error("Unsupported ALSA format for encoding: " + audio_format_.to_string());
    }

    bool format_set = false;
    for (const auto candidate : format_candidates) {
        const int fmt_err = snd_pcm_hw_params_set_format(pcm, hw, candidate);
        if (fmt_err >= 0) {
            fmt = candidate;
            format_set = true;
            if (candidate != format_candidates[0]) {
                RAV_LOG_WARNING(
                    "Requested format {} not supported; using fallback {}",
                    snd_pcm_format_name(format_candidates[0]),
                    snd_pcm_format_name(candidate)
                );
            }
            break;
        }
    }

    if (!format_set) {
        snd_pcm_close(pcm);
        throw std::runtime_error(
            std::string("No supported ALSA format found (tried: ") + snd_pcm_format_name(format_candidates[0]) + ")"
        );
    }

    // Store the actual format we're using (may differ from audio_format_ if we used a fallback).
    alsa_format_ = fmt;

    snd_pcm_uframes_t period = 256;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
    snd_pcm_uframes_t buffer = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);

    const int hw_err = snd_pcm_hw_params(pcm, hw);
    if (hw_err < 0) {
        snd_pcm_close(pcm);
        throw std::runtime_error(std::string("snd_pcm_hw_params failed: ") + snd_strerror(hw_err));
    }

    const int prep_err = snd_pcm_prepare(pcm);
    if (prep_err < 0) {
        snd_pcm_close(pcm);
        throw std::runtime_error(std::string("snd_pcm_prepare failed: ") + snd_strerror(prep_err));
    }

    alsa_pcm_ = pcm;
}

void RxSession::start_audio_thread() {
    if (audio_thread_.joinable()) {
        return;
    }
    audio_keep_going_.store(true, std::memory_order_relaxed);

    audio_thread_ = std::thread([this]() {
        TRACY_SET_THREAD_NAME("alsa_playout");

        auto* pcm = static_cast<snd_pcm_t*>(alsa_pcm_);
        if (!pcm) {
            return;
        }

        const unsigned long frames = 256;
        std::vector<uint8_t> buf;
        buf.resize(static_cast<size_t>(frames) * audio_format_.bytes_per_frame());

        while (audio_keep_going_.load(std::memory_order_relaxed)) {
            if (!receiver_id_.is_valid() || !audio_format_.is_valid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            const auto buffer_size = static_cast<size_t>(frames) * audio_format_.bytes_per_frame();

            auto& clock = get_local_clock();
            if (!clock.is_calibrated()) {
                std::memset(buf.data(), audio_format_.ground_value(), buffer_size);
            } else {
                const auto ptp_ts = clock.now().to_rtp_timestamp32(audio_format_.sample_rate)
                    - static_cast<uint32_t>(cfg_.playout_delay_frames);

                auto rtp_ts = node_.read_data_realtime(receiver_id_, buf.data(), buffer_size, {}, {});
                if (!rtp_ts) {
                    std::memset(buf.data(), audio_format_.ground_value(), buffer_size);
                    // Silence
                    signal_rms_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
                    signal_peak_db_.store(std::numeric_limits<double>::quiet_NaN(), std::memory_order_relaxed);
                } else {
                    auto drift = rav::WrappingUint32(ptp_ts).diff(rav::WrappingUint32(*rtp_ts));
                    if (static_cast<uint32_t>(std::abs(drift)) > frames * 2) {
                        (void)node_.read_data_realtime(receiver_id_, buf.data(), buffer_size, ptp_ts, {});
                    }
                    if (audio_format_.byte_order == rav::AudioFormat::ByteOrder::be) {
                        rav::swap_bytes(buf.data(), buffer_size, audio_format_.bytes_per_sample());
                    }

                    // Calculate signal levels for monitoring (before format conversion).
                    const double rms_db = calculate_rms_db(buf.data(), buffer_size, audio_format_);
                    signal_rms_db_.store(rms_db, std::memory_order_relaxed);
                    // Peak is approximated as RMS * sqrt(2) for simplicity.
                    signal_peak_db_.store(rms_db + 3.0, std::memory_order_relaxed);
                }
            }

            // Format conversion if ALSA format differs from input format (e.g., 24-bit → S32_LE or S16_LE).
            const auto fmt = static_cast<snd_pcm_format_t>(alsa_format_);
            std::vector<uint8_t> converted_buf;
            const uint8_t* write_buf = buf.data();
            size_t write_frames = frames;

            if (audio_format_.encoding == rav::AudioEncoding::pcm_s24 && fmt == SND_PCM_FORMAT_S32_LE) {
                // 24-bit (3 bytes) → 32-bit: unpack and pad MSB (sign-extend).
                converted_buf.resize(frames * audio_format_.num_channels * 4);
                const uint8_t* src = buf.data();
                int32_t* dst = reinterpret_cast<int32_t*>(converted_buf.data());
                for (size_t i = 0; i < frames * audio_format_.num_channels; ++i) {
                    // 24-bit little-endian: bytes [0,1,2] → int32_t (sign-extend).
                    const int32_t sample = static_cast<int32_t>(
                        static_cast<uint32_t>(src[i * 3]) | (static_cast<uint32_t>(src[i * 3 + 1]) << 8)
                        | (static_cast<uint32_t>(static_cast<int8_t>(src[i * 3 + 2])) << 16)
                    );
                    // Sign-extend to 32-bit.
                    dst[i] = (sample << 8) >> 8;
                }
                write_buf = converted_buf.data();
            } else if (audio_format_.encoding == rav::AudioEncoding::pcm_s24 && fmt == SND_PCM_FORMAT_S16_LE) {
                // 24-bit (3 bytes) → 16-bit: truncate (keep top 16 bits).
                converted_buf.resize(frames * audio_format_.num_channels * 2);
                const uint8_t* src = buf.data();
                int16_t* dst = reinterpret_cast<int16_t*>(converted_buf.data());
                for (size_t i = 0; i < frames * audio_format_.num_channels; ++i) {
                    // Take top 16 bits from 24-bit sample.
                    const int32_t sample = static_cast<int32_t>(
                        static_cast<uint32_t>(src[i * 3]) | (static_cast<uint32_t>(src[i * 3 + 1]) << 8)
                        | (static_cast<uint32_t>(static_cast<int8_t>(src[i * 3 + 2])) << 16)
                    );
                    dst[i] = static_cast<int16_t>(sample >> 8);
                }
                write_buf = converted_buf.data();
            } else if (audio_format_.encoding == rav::AudioEncoding::pcm_s32 && fmt == SND_PCM_FORMAT_S16_LE) {
                // 32-bit → 16-bit: truncate (keep top 16 bits).
                converted_buf.resize(frames * audio_format_.num_channels * 2);
                const int32_t* src = reinterpret_cast<const int32_t*>(buf.data());
                int16_t* dst = reinterpret_cast<int16_t*>(converted_buf.data());
                for (size_t i = 0; i < frames * audio_format_.num_channels; ++i) {
                    dst[i] = static_cast<int16_t>(src[i] >> 16);
                }
                write_buf = converted_buf.data();
            }

            snd_pcm_sframes_t wrote = snd_pcm_writei(pcm, write_buf, write_frames);
            if (wrote < 0) {
                wrote = snd_pcm_recover(pcm, static_cast<int>(wrote), 1);
                if (wrote < 0) {
                    // If recover fails, back off a bit.
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }
    });
}

void RxSession::stop_audio_thread() {
    audio_keep_going_.store(false, std::memory_order_relaxed);
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
}

void RxSession::start_stats_thread() {
    if (stats_thread_.joinable()) {
        return;
    }
    stats_keep_going_.store(true, std::memory_order_relaxed);
    stats_thread_ = std::thread([this]() {
        while (stats_keep_going_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const double rms = signal_rms_db_.load(std::memory_order_relaxed);
            const double peak = signal_peak_db_.load(std::memory_order_relaxed);

            if (std::isnan(rms) || std::isnan(peak)) {
                fmt::println("[Signal] RMS: --- dB, Peak: --- dB (silence/no data)");
            } else {
                fmt::println("[Signal] RMS: {:.1f} dB, Peak: {:.1f} dB", rms, peak);
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
    // Convert to dB (avoid log(0))
    const double rms_db = rms > 0.0 ? 20.0 * std::log10(rms) : std::numeric_limits<double>::lowest();
    return rms_db;
}

}  // namespace app


