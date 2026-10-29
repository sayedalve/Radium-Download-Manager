// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Whole-Site Crawler & Link Grabber with SSRF Protection
#include "crawler/site_grabber.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <regex>
#include <set>

#include "brisk_engine/http_client.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::crawler {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string ExtractHostFromUrl(std::string_view url) {
    auto parsed = brisk::HttpClient::ParseUrl(url);
    if (!parsed.ok()) return "";
    return parsed.value->host;
}

} // namespace

std::string SiteGrabber::ResolveUrl(std::string_view base_url, std::string_view rel) {
    while (!rel.empty() && (rel.front() == ' ' || rel.front() == '\t' || rel.front() == '\r' || rel.front() == '\n')) {
        rel.remove_prefix(1);
    }
    while (!rel.empty() && (rel.back() == ' ' || rel.back() == '\t' || rel.back() == '\r' || rel.back() == '\n')) {
        rel.remove_suffix(1);
    }
    if (rel.empty()) return "";

    // Drop fragment
    size_t hash_pos = rel.find('#');
    if (hash_pos != std::string_view::npos) {
        rel = rel.substr(0, hash_pos);
    }
    if (rel.empty()) return "";

    std::string r(rel);
    if (r.find("javascript:") == 0 || r.find("mailto:") == 0 || r.find("tel:") == 0 || r.find("data:") == 0) {
        return "";
    }

    if (r.find("http://") == 0 || r.find("https://") == 0 || r.find("ftp://") == 0) {
        return r;
    }

    std::string base(base_url);
    size_t scheme_sep = base.find("://");
    if (scheme_sep == std::string::npos) return r;
    std::string scheme = base.substr(0, scheme_sep);

    if (r.rfind("//", 0) == 0) {
        return scheme + ":" + r;
    }

    size_t host_start = scheme_sep + 3;
    size_t path_start = base.find('/', host_start);
    std::string origin = (path_start == std::string::npos) ? base : base.substr(0, path_start);

    if (r.front() == '/') {
        return origin + r;
    }

    std::string base_dir = "/";
    if (path_start != std::string::npos) {
        size_t last_slash = base.rfind('/');
        if (last_slash >= path_start) {
            base_dir = base.substr(path_start, last_slash - path_start + 1);
        }
    }
    return origin + base_dir + r;
}

bool SiteGrabber::MatchesFilter(std::string_view url, const SiteGrabberConfig& cfg, std::string* out_category) {
    std::string filename = PathSanitizer::ExtractFromUrl(std::string(url));
    std::string cat = InferCategoryFromFilename(filename);
    if (out_category) *out_category = cat;

    if (cfg.filter_categories.empty() && cfg.custom_extensions.empty()) {
        return cat != "General";
    }

    for (const auto& fc : cfg.filter_categories) {
        if (fc == "All" || fc == cat) return true;
    }

    auto dot = filename.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < filename.size()) {
        std::string ext = ToLower(filename.substr(dot + 1));
        for (const auto& ce : cfg.custom_extensions) {
            if (ToLower(ce) == ext) return true;
        }
    }
    return false;
}

std::vector<std::string> SiteGrabber::ExtractUrlsFromHtml(std::string_view html, std::string_view base_url) {
    std::vector<std::string> urls;
    std::set<std::string> seen;

    static const std::regex kHrefSrcRegex(
        R"((?:href|src|data-src|data-url|action|poster)\s*=\s*["']([^"'#\s>]+)["'])",
        std::regex::icase | std::regex::optimize);

    static const std::regex kCssUrlRegex(
        R"(url\(\s*["']?([^"')#\s]+)["']?\s*\))",
        std::regex::icase | std::regex::optimize);

    std::string s(html);

    auto scan_regex = [&](const std::regex& re) {
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        for (std::sregex_iterator it = begin; it != end; ++it) {
            std::smatch match = *it;
            if (match.size() >= 2) {
                std::string raw = match[1].str();
                std::string resolved = ResolveUrl(base_url, raw);
                if (!resolved.empty() && !seen.count(resolved)) {
                    seen.insert(resolved);
                    urls.push_back(resolved);
                }
            }
        }
    };

    scan_regex(kHrefSrcRegex);
    scan_regex(kCssUrlRegex);

    return urls;
}

Result<std::vector<GrabbedItem>> SiteGrabber::Crawl(
    const SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    GrabbedItemCallback on_item_found) {

    if (cfg.start_url.empty()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CRAWLER_START_URL", "Start URL cannot be empty");
    }

    auto start_pu = brisk::HttpClient::ParseUrl(cfg.start_url);
    if (!start_pu.ok()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CRAWLER_INVALID_URL", "Invalid starting URL: " + cfg.start_url);
    }
    std::string start_host = start_pu.value->host;

    // SSRF protection on starting host
    if (!cfg.allow_loopback && (start_pu.value->is_loopback_or_private || brisk::HttpClient::CheckHostResolvesToPrivate(start_host))) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_SSRF_BLOCKED", "Crawl blocked to private/loopback destination: " + start_host);
    }

    struct QueueEntry {
        std::string url;
        uint32_t depth;
        std::string page_url;
    };

    std::deque<QueueEntry> queue;
    std::set<std::string> visited_pages;
    std::set<std::string> discovered_files;
    std::vector<GrabbedItem> results;

    queue.push_back({cfg.start_url, 0, cfg.start_url});
    uint32_t pages_crawled = 0;

    while (!queue.empty() && !cancel_requested.load()) {
        auto current = queue.front();
        queue.pop_front();

        if (visited_pages.count(current.url)) continue;
        visited_pages.insert(current.url);

        auto current_pu = brisk::HttpClient::ParseUrl(current.url);
        if (!current_pu.ok()) continue;
        std::string current_host = current_pu.value->host;
        if (cfg.same_domain_only && current_host != start_host) {
            continue;
        }
        if (!cfg.allowed_domains.empty()) {
            bool domain_ok = false;
            for (const auto& dom : cfg.allowed_domains) {
                if (current_host == dom || current_host.ends_with("." + dom)) {
                    domain_ok = true;
                    break;
                }
            }
            if (!domain_ok) continue;
        }

        // SSRF protection per link
        if (!cfg.allow_loopback && (current_pu.value->is_loopback_or_private || brisk::HttpClient::CheckHostResolvesToPrivate(current_host))) {
            continue;
        }

        pages_crawled++;
        if (pages_crawled > cfg.max_pages) break;

        // Fetch page HTML
        std::string html_body;
        std::atomic<bool> req_cancel{false};
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) FluxDownloadManager/1.0"}
        };

        auto res = brisk::HttpClient::PerformGet(
            current.url,
            headers,
            0,
            4194304, // First 4MB of page
            cfg.timeout_ms,
            cfg.allow_loopback,
            req_cancel,
            [&](const brisk::HttpResponseMeta& meta) -> bool {
                if (cancel_requested.load()) return false;
                if (meta.status_code >= 400) return false;
                std::string ct = ToLower(meta.content_type);
                if (!ct.empty() && ct.find("text/html") == std::string::npos &&
                    ct.find("application/xhtml+xml") == std::string::npos) {
                    return false; // Skip non-HTML files early
                }
                return true;
            },
            [&](const uint8_t* data, size_t len) -> bool {
                if (cancel_requested.load()) return false;
                if (html_body.size() + len > 4194304) {
                    size_t allowed = 4194304 - html_body.size();
                    if (allowed > 0) {
                        html_body.append(reinterpret_cast<const char*>(data), allowed);
                    }
                    return false; // Cap at 4MB per HTML page
                }
                html_body.append(reinterpret_cast<const char*>(data), len);
                return true;
            });

        if (!res.ok()) continue;

        std::string effective_base_url = (res.ok() && !res.value->final_url.empty()) ? res.value->final_url : current.url;
        auto extracted_urls = ExtractUrlsFromHtml(html_body, effective_base_url);
        for (const auto& link : extracted_urls) {
            if (cancel_requested.load()) break;

            std::string cat;
            if (MatchesFilter(link, cfg, &cat)) {
                if (!discovered_files.count(link)) {
                    discovered_files.insert(link);
                    GrabbedItem item;
                    item.url = link;
                    item.page_url = current.url;
                    item.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(link));
                    item.category = cat;
                    item.depth = current.depth + 1;
                    results.push_back(item);
                    if (on_item_found) on_item_found(item);
                }
            } else if (current.depth + 1 <= cfg.max_depth) {
                // Potential subpage to crawl
                if (!visited_pages.count(link)) {
                    queue.push_back({link, current.depth + 1, current.url});
                }
            }
        }
    }

    if (cancel_requested.load()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CANCELED", "Site crawl canceled");
    }

    return Result<std::vector<GrabbedItem>>::Ok(std::move(results));
}

} // namespace openidm::crawler
