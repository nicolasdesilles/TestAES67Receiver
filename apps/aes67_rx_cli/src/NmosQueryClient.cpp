// SPDX-License-Identifier: AGPL-3.0-or-later

#include "NmosQueryClient.hpp"

#include "ravennakit/core/exception.hpp"
#include "ravennakit/core/net/http/http_client.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/url/parse.hpp>

#include <future>
#include <stdexcept>

namespace app {

namespace {

boost::urls::url parse_url_or_throw(const std::string& url_str) {
    auto parsed = boost::urls::parse_uri_reference(url_str);
    if (parsed.has_error()) {
        throw std::runtime_error("Invalid URL: " + url_str);
    }
    boost::urls::url url = *parsed;
    if (url.scheme().empty()) {
        url.set_scheme("http");
    }
    if (url.host().empty()) {
        throw std::runtime_error("Invalid URL (missing host): " + url_str);
    }
    if (url.port().empty()) {
        url.set_port("80");
    }
    return url;
}

boost::json::value parse_json_or_throw(const std::string& body) {
    boost::json::error_code ec;
    auto jv = boost::json::parse(body, ec);
    if (ec) {
        throw std::runtime_error("Failed to parse JSON: " + std::string(ec.message()));
    }
    return jv;
}

std::string get_string_or_empty(const boost::json::object& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return {};
    }
    if (it->value().is_null()) {
        return {};
    }
    if (!it->value().is_string()) {
        return {};
    }
    return std::string(it->value().as_string());
}

int get_int_or_zero(const boost::json::object& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        return 0;
    }
    if (it->value().is_int64()) {
        return static_cast<int>(it->value().as_int64());
    }
    if (it->value().is_uint64()) {
        return static_cast<int>(it->value().as_uint64());
    }
    return 0;
}

}  // namespace

NmosQueryClient::NmosQueryClient(boost::urls::url registry_base_url) :
    registry_base_url_(std::move(registry_base_url)), http_(std::make_unique<rav::HttpClient>(io_)) {}

std::string NmosQueryClient::normalize_target(const std::string_view target) {
    if (target.empty()) {
        return "/";
    }
    if (target.front() == '/') {
        return std::string(target);
    }
    return "/" + std::string(target);
}

boost::json::value NmosQueryClient::http_get_json(const boost::urls::url& base, const std::string_view target) {
    return parse_json_or_throw(http_get_text(base, target));
}

std::string NmosQueryClient::http_get_text(const boost::urls::url& base, const std::string_view target) {
    http_->set_host(base.host(), base.port());

    std::promise<boost::system::result<rav::HttpClientBase::Response>> prom;
    auto fut = prom.get_future();

    http_->get_async(
        normalize_target(target),
        [&prom](boost::system::result<rav::HttpClientBase::Response> response) mutable { prom.set_value(std::move(response)); }
    );

    io_.restart();
    io_.run();

    auto result = fut.get();
    if (!result) {
        throw std::runtime_error("HTTP request failed: " + result.error().message());
    }

    const auto& res = result.value();
    if (res.result_int() < 200 || res.result_int() >= 300) {
        throw std::runtime_error("HTTP error " + std::to_string(res.result_int()) + ": " + res.body());
    }

    return res.body();
}

std::vector<NmosSenderInfo> NmosQueryClient::list_senders(const std::string_view query_version) {
    const auto target = std::string("/x-nmos/query/") + std::string(query_version) + "/senders";
    auto json = http_get_json(registry_base_url_, target);

    // Query API typically returns an array. Some implementations may wrap in an object (e.g. { "items": [...] }).
    boost::json::array items;
    if (json.is_array()) {
        items = json.as_array();
    } else if (json.is_object()) {
        const auto& obj = json.as_object();
        auto it = obj.find("items");
        if (it != obj.end() && it->value().is_array()) {
            items = it->value().as_array();
        } else {
            throw std::runtime_error("Unexpected Query API response shape (expected array or object with items[])");
        }
    } else {
        throw std::runtime_error("Unexpected Query API response type (expected JSON array)");
    }

    std::vector<NmosSenderInfo> out;
    out.reserve(items.size());

    for (const auto& v : items) {
        if (!v.is_object()) {
            continue;
        }
        const auto& obj = v.as_object();
        NmosSenderInfo s;
        s.id = get_string_or_empty(obj, "id");
        s.label = get_string_or_empty(obj, "label");
        s.description = get_string_or_empty(obj, "description");
        s.transport = get_string_or_empty(obj, "transport");
        s.manifest_href = get_string_or_empty(obj, "manifest_href");
        s.flow_id = get_string_or_empty(obj, "flow_id");
        s.device_id = get_string_or_empty(obj, "device_id");

        // Only keep RTP senders with a manifest href.
        if (s.transport != "urn:x-nmos:transport:rtp") {
            continue;
        }
        if (s.manifest_href.empty()) {
            continue;
        }
        if (s.label.empty()) {
            s.label = s.id;
        }
        out.push_back(std::move(s));
    }

    return out;
}

std::optional<NmosFlowInfo> NmosQueryClient::get_flow(const std::string_view query_version, const std::string_view flow_id) {
    if (flow_id.empty()) {
        return std::nullopt;
    }

    auto it = flow_cache_.find(std::string(flow_id));
    if (it != flow_cache_.end()) {
        return it->second;
    }

    const auto target = std::string("/x-nmos/query/") + std::string(query_version) + "/flows/" + std::string(flow_id);
    auto json = http_get_json(registry_base_url_, target);
    if (!json.is_object()) {
        return std::nullopt;
    }

    const auto& obj = json.as_object();
    NmosFlowInfo flow;
    flow.id = get_string_or_empty(obj, "id");
    flow.media_type = get_string_or_empty(obj, "media_type");
    flow.bit_depth = get_int_or_zero(obj, "bit_depth");

    auto sr_it = obj.find("sample_rate");
    if (sr_it != obj.end() && sr_it->value().is_object()) {
        const auto& sr = sr_it->value().as_object();
        flow.sample_rate_num = get_int_or_zero(sr, "numerator");
        flow.sample_rate_den = get_int_or_zero(sr, "denominator");
        if (flow.sample_rate_den == 0) {
            flow.sample_rate_den = 1;
        }
    }

    flow_cache_.emplace(flow.id.empty() ? std::string(flow_id) : flow.id, flow);
    return flow;
}

std::string NmosQueryClient::fetch_text_url(const std::string& url) {
    const auto parsed = parse_url_or_throw(url);
    const auto target = parsed.encoded_target().empty() ? "/" : std::string(parsed.encoded_target());
    return http_get_text(parsed, target);
}

}  // namespace app


