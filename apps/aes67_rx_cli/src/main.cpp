// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AudioOutput.hpp"
#include "NmosQueryClient.hpp"
#include "RxSession.hpp"

#include "ravennakit/core/system.hpp"
#include "ravennakit/sdp/sdp_session_description.hpp"

#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <fmt/core.h>

#include <boost/url/parse.hpp>

#include <cctype>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

boost::urls::url parse_registry_url_or_throw(const std::string& url_str) {
    auto parsed = boost::urls::parse_uri_reference(url_str);
    if (parsed.has_error()) {
        throw std::runtime_error("Invalid --registry URL: " + url_str);
    }
    boost::urls::url url = *parsed;
    if (url.scheme().empty()) {
        url.set_scheme("http");
    }
    if (url.host().empty()) {
        throw std::runtime_error("Invalid --registry URL (missing host): " + url_str);
    }
    if (url.port().empty()) {
        url.set_port("80");
    }
    return url;
}

std::optional<size_t> prompt_index(size_t max_exclusive) {
    while (true) {
        fmt::print("Select stream [0-{}] (or 'q' to quit): ", max_exclusive ? (max_exclusive - 1) : 0);
        std::string line;
        if (!std::getline(std::cin, line)) {
            return std::nullopt;
        }
        if (line == "q" || line == "quit" || line == "exit") {
            return std::nullopt;
        }
        // trim
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        if (line.empty()) {
            continue;
        }
        try {
            const auto idx = static_cast<size_t>(std::stoul(line));
            if (idx < max_exclusive) {
                return idx;
            }
        } catch (...) {
        }
        fmt::println("Invalid selection.");
    }
}

}  // namespace

int main(int argc, char** argv) {
    rav::set_log_level_from_env();
    rav::do_system_checks();

    CLI::App app {"aes67_rx_cli (NMOS registry -> SDP -> RAVENNA receiver -> ALSA)"};
    argv = app.ensure_utf8(argv);

    std::string registry;
    std::string interfaces;
    std::string alsa_device = "default";
    std::string query_version = "v1.3";
    bool list_audio_devices = false;

    app.add_option("--registry", registry, "NMOS registry base URL, e.g. http://192.168.1.10:80")->required();
    app.add_option(
           "--interfaces",
           interfaces,
           "Comma-separated interface selector(s). Each entry can be identifier/display name/description/MAC/IP (ravennakit parser)"
    )
        ->required();
    app.add_option("--alsa-device", alsa_device, "ALSA PCM name (e.g. hw:1,0 for rk3528-acodec; default: default)");
    app.add_option("--query-version", query_version, "NMOS Query API version (default: v1.3)");
    app.add_flag("--list-audio-devices", list_audio_devices, "List ALSA output devices and exit");

    CLI11_PARSE(app, argc, argv);

    // Audio device listing
    if (list_audio_devices) {
        auto audio = app::create_portaudio_output();
        const auto devices = audio->list_output_devices();
        fmt::println("ALSA output devices:");
        for (const auto& d : devices) {
            fmt::println("  - {}", d.name);
        }
        return 0;
    }

    // NMOS discovery via registry Query API
    const auto registry_url = parse_registry_url_or_throw(registry);
    app::NmosQueryClient query(registry_url);

    fmt::println("Querying NMOS registry {} ...", registry_url.buffer());
    auto all_senders = query.list_senders(query_version);

    // Keep RTP variants (rtp, rtp.mcast, rtp.ucast, etc.)
    std::vector<app::NmosSenderInfo> senders;
    senders.reserve(all_senders.size());
    for (auto& s : all_senders) {
        if (s.transport.find("urn:x-nmos:transport:rtp") != std::string::npos) {
            senders.push_back(std::move(s));
        }
    }

    if (senders.empty()) {
        fmt::println("No NMOS RTP senders found in Query API response.");
        fmt::println("Tip: check the registry Query API at /x-nmos/query/{}/senders", query_version);
        return 1;
    }

    fmt::println("Discovered senders:");
    for (size_t i = 0; i < senders.size(); ++i) {
        auto flow = query.get_flow(query_version, senders[i].flow_id);
        const auto sdp_note = senders[i].manifest_href.empty() ? " (no manifest_href)" : "";
        if (flow) {
            const auto sr =
                (flow->sample_rate_den != 0) ? (static_cast<double>(flow->sample_rate_num) / static_cast<double>(flow->sample_rate_den)) : 0.0;
            fmt::println(
                "  [{}] {}{}  ({}, {}bit, {:.0f}Hz)  id={}",
                i,
                senders[i].label,
                sdp_note,
                flow->media_type.empty() ? "audio/?" : flow->media_type,
                flow->bit_depth,
                sr,
                senders[i].id
            );
        } else {
            fmt::println("  [{}] {}{}  id={}", i, senders[i].label, sdp_note, senders[i].id);
        }
    }

    const auto choice = prompt_index(senders.size());
    if (!choice) {
        return 0;
    }

    const auto& sender = senders[*choice];
    if (sender.manifest_href.empty()) {
        fmt::println("Selected sender has no manifest_href, so this CLI cannot fetch SDP yet.");
        fmt::println("Workaround: configure your sender/registry so the Sender resource provides manifest_href (SDP URL).");
        return 1;
    }
    fmt::println("Fetching SDP from manifest_href: {}", sender.manifest_href);
    const auto sdp_text = query.fetch_text_url(sender.manifest_href);

    const auto parsed = rav::sdp::parse_session_description(sdp_text);
    if (!parsed) {
        fmt::println("Failed to parse SDP: {}", parsed.error());
        return 1;
    }

    app::RxSession session;
    app::RxConfig cfg;
    cfg.interfaces = interfaces;
    cfg.alsa_device = alsa_device;
    cfg.nmos_registry_url = registry;  // Enable NMOS registration to the same registry we query
    cfg.alsa_device = alsa_device;

    fmt::println("Starting receiver for SDP session: {}", parsed->session_name);
    session.start_from_sdp(*parsed, cfg);

    fmt::println("Receiving. Press return to stop...");
    std::string line;
    std::getline(std::cin, line);

    session.stop();
    return 0;
}


