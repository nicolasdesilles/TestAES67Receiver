// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/json/value.hpp>
#include <boost/url/url.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rav {
class HttpClient;
}

namespace app {

struct NmosSenderInfo {
    std::string id;
    std::string label;
    std::string description;
    std::string transport;
    std::string manifest_href;  // SDP URL (may be empty)
    std::string flow_id;        // may be empty
    std::string device_id;      // may be empty
};

struct NmosFlowInfo {
    std::string id;
    std::string media_type;  // e.g. audio/L24
    int bit_depth = 0;
    int sample_rate_num = 0;
    int sample_rate_den = 1;
};

class NmosQueryClient {
  public:
    explicit NmosQueryClient(boost::urls::url registry_base_url);

    // Returns NMOS senders from IS-04 Query API (e.g. /x-nmos/query/v1.3/senders).
    // Throws std::runtime_error on HTTP/parse errors.
    [[nodiscard]] std::vector<NmosSenderInfo> list_senders(std::string_view query_version = "v1.3");

    // Fetches and parses a Flow (optional, for display).
    [[nodiscard]] std::optional<NmosFlowInfo> get_flow(std::string_view query_version, std::string_view flow_id);

    // Fetches SDP text from an absolute URL (e.g. sender.manifest_href).
    [[nodiscard]] std::string fetch_text_url(const std::string& url);

  private:
    boost::urls::url registry_base_url_;
    boost::asio::io_context io_;
    std::unique_ptr<rav::HttpClient> http_;
    std::unordered_map<std::string, NmosFlowInfo> flow_cache_;

    [[nodiscard]] boost::json::value http_get_json(const boost::urls::url& base, std::string_view target);
    [[nodiscard]] std::string http_get_text(const boost::urls::url& base, std::string_view target);

    static std::string normalize_target(std::string_view target);
};

}  // namespace app


