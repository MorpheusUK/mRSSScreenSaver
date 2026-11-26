// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/engine.hpp"
#include <algorithm>
#include <ctime>
#include <set>

namespace mrss
{
namespace
{
using json = nlohmann::json;
std::filesystem::path cache()
{
    const auto p = dataDirectory() / L"cache";
    std::filesystem::create_directories(p);
    return p;
}
json readJson(const std::filesystem::path &p)
{
    const auto b = readFile(p, 1024 * 1024);
    return json::parse(b.begin(), b.end());
}
std::vector<std::wstring> exclusions(const FeedConfig &f, const Defaults &d)
{
    auto p = d.excludedUrlText;
    p.insert(p.end(), f.policy.excludedUrlText.begin(), f.policy.excludedUrlText.end());
    return p;
}
Time now()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}
std::wstring localTimestamp(std::int64_t seconds)
{
    const auto timestamp = std::chrono::system_clock::to_time_t(Time(std::chrono::seconds(seconds)));
    std::tm local{};
    wchar_t text[32]{};
    if (localtime_s(&local, &timestamp) == 0 &&
        std::wcsftime(text, std::size(text), L"%Y-%m-%d %H:%M:%S", &local) != 0)
        return text;
    return L"unknown";
}
Feed readFeed(const FeedConfig &config, const Defaults &defaults, bool force, std::stop_token stop,
              FeedStatus &status, bool cacheOnly)
{
    const auto base = cache() / digest(config.url);
    auto xml = base;
    xml += L".feed";
    auto meta = base;
    meta += L".feed.json";
    json m = json::object();
    try
    {
        m = readJson(meta);
    }
    catch (...)
    {
    }
    // Legacy metadata did not record when the body was cached. The feed file's
    // modification time records that event; a 304 must not advance it.
    if (!m.contains("cached"))
    {
        std::error_code ec;
        const auto written = std::filesystem::last_write_time(xml, ec);
        if (!ec)
            m["cached"] = std::chrono::floor<std::chrono::seconds>(
                              std::chrono::clock_cast<std::chrono::system_clock>(written))
                              .time_since_epoch()
                              .count();
    }
    bool fetch = force || !std::filesystem::exists(xml) ||
                 now().time_since_epoch().count() - m.value("checked", std::int64_t{0}) >=
                     static_cast<std::int64_t>(defaults.refreshHours) * 3600;
    auto source = config.url;
    std::wstring message = L"Available";
    if (fetch && !cacheOnly)
    {
        try
        {
            const auto valid = validateUrl(config.url, config.policy.allowHttp);
            if (!valid.url)
                throw std::runtime_error(utf8(valid.error));
            const auto response = download(
                config.url, config.policy, {}, 8 * 1024 * 1024, stop, wide(m.value("etag", std::string{})),
                wide(m.value("modified", std::string{})), defaults.simultaneousDownloads);
            source = response.finalUrl;
            if (response.status != 304)
            {
                (void)parseFeed(response.body, source);
                atomicWrite(xml, response.body);
                m = {{"etag", utf8(response.etag)},
                     {"modified", utf8(response.modified)},
                     {"final", utf8(source)},
                     {"cached", now().time_since_epoch().count()}};
            }
            m["checked"] = now().time_since_epoch().count();
            writeText(meta, m.dump());
            message = L"Available";
        }
        catch (const std::exception &e)
        {
            if (stop.stop_requested())
                throw;
            message = wide(e.what());
            if (offlineHint())
                message = L"Offline (Windows connectivity hint): " + message;
            if (!std::filesystem::exists(xml))
                throw;
            message += L"; using cached feed";
        }
    }
    if (m.contains("final"))
        source = wide(m.at("final").get<std::string>());
    auto result = parseFeed(readFile(xml, 8 * 1024 * 1024), source);
    status.message = message;
    if (m.contains("checked"))
    {
        status.message += L" • last success " + localTimestamp(m.at("checked").get<std::int64_t>());
    }
    if (m.contains("cached"))
        status.message += L" • cached " + localTimestamp(m.at("cached").get<std::int64_t>());
    if (m.contains("checked") || m.contains("cached"))
        status.message += L" (local)";
    return result;
}
} // namespace
std::wstring eligibility(const MediaImage &image, const FeedConfig &feed, const Defaults &defaults)
{
    if (!feed.policy.enabled)
        return L"Feed disabled";
    if (!eligibleRating(image.rating, defaults))
        return L"Rating excluded";
    if (!eligibleAge(image.published, now(), effectiveAgeLimit(defaults, feed.policy)))
        return L"Outside age limit or undated";
    if (!validateUrl(image.url, feed.policy.allowHttp).url)
        return L"Image URL not allowed";
    if (excludedUrl(image.url, exclusions(feed, defaults)))
        return L"URL substring excluded";
    if (!matchesFilter(feed.filter, filterText(image, feed.filter.fields)))
        return L"Advanced filter excluded";
    return {};
}
Catalog buildCatalog(const Settings &settings, bool refresh, std::stop_token stop,
                     std::function<void(std::size_t, const FeedStatus &)> progress, bool cacheOnly)
{
    Catalog result;
    std::set<std::wstring> urls;
    for (std::size_t i = 0; i < settings.feeds.size(); ++i)
    {
        if (stop.stop_requested())
            break;
        const auto &f = settings.feeds[i];
        FeedStatus s;
        s.title = f.title;
        if (!f.policy.enabled)
            s.message = L"Disabled";
        else
            try
            {
                validateFilter(f.filter);
                auto feed = readFeed(f, settings.defaults, refresh, stop, s, cacheOnly);
                s.title = feed.title;
                s.total = static_cast<unsigned>(feed.images.size());
                for (auto &image : feed.images)
                    if (eligibility(image, f, settings.defaults).empty())
                    {
                        ++s.eligible;
                        if (urls.insert(image.url).second)
                            result.items.push_back({std::move(image), f, i});
                    }
            }
            catch (const std::exception &e)
            {
                s.message = wide(e.what());
            }
        result.status.push_back(s);
        if (progress)
            progress(i, s);
    }
    return result;
}
std::filesystem::path imageFile(const Item &item, const Defaults &defaults, std::stop_token stop,
                                bool cacheOnly)
{
    const auto reason = eligibility(item.image, item.feed, defaults);
    if (!reason.empty())
        throw std::runtime_error(utf8(reason));
    const auto base = cache() / digest(item.image.url);
    auto path = base;
    path += L".img";
    auto meta = base;
    meta += L".image.json";
    const auto rules = exclusions(item.feed, defaults);
    json cached = json::object();
    bool usableCache = false;
    if (std::filesystem::exists(path))
        try
        {
            const auto m = readJson(meta);
            cached = m;
            const auto final = wide(m.at("final").get<std::string>());
            if (!validateUrl(final, item.feed.policy.allowHttp).url || excludedUrl(final, rules))
                throw std::runtime_error("Cached destination excluded");
            if (!item.feed.policy.allowPrivateNetwork && !m.value("public", false))
                throw std::runtime_error("Cached destination policy differs");
            const auto b = readFile(path, 25 * 1024 * 1024);
            if (!identifyImage(b))
                throw std::runtime_error("Not a supported image");
            std::error_code ec;
            std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
            usableCache = true;
            if (cacheOnly ||
                now().time_since_epoch().count() - m.value("checked", now().time_since_epoch().count()) <
                    static_cast<std::int64_t>(defaults.refreshHours) * 3600)
                return path;
        }
        catch (...)
        {
            if (cacheOnly)
                throw;
        }
    if (cacheOnly)
        throw std::runtime_error("No cached image");
    try
    {
        const auto response =
            download(item.image.url, item.feed.policy, rules, 25 * 1024 * 1024, stop,
                     wide(cached.value("etag", std::string{})), wide(cached.value("modified", std::string{})),
                     defaults.simultaneousDownloads);
        if (response.status != 304 && !identifyImage(response.body))
            throw std::runtime_error("Unsupported image content (not an allowed still-image format)");
        if (response.status == 304 && !usableCache)
            throw std::runtime_error("Server returned 304 without usable cached image");
        withDataLock([&] {
            if (response.status != 304)
                atomicWrite(path, response.body);
            writeText(meta, json{{"url", utf8(item.image.url)},
                                 {"final", utf8(response.finalUrl)},
                                 {"public", !item.feed.policy.allowPrivateNetwork},
                                 {"checked", now().time_since_epoch().count()},
                                 {"etag", utf8(response.etag)},
                                 {"modified", utf8(response.modified)}}
                                .dump());
        });
    }
    catch (...)
    {
        if (stop.stop_requested() || !usableCache)
            throw;
    }
    return path;
}
void trimCache(std::uint64_t limit)
{
    withDataLock([&] {
        const auto dir = cache();
        std::vector<std::filesystem::directory_entry> images;
        std::uint64_t total = 0;
        for (const auto &f : std::filesystem::directory_iterator(dir))
            if (f.path().extension() == L".img")
            {
                images.push_back(f);
                total += f.file_size();
            }
        std::sort(images.begin(), images.end(),
                  [](const auto &a, const auto &b) { return a.last_write_time() < b.last_write_time(); });
        for (const auto &f : images)
        {
            if (total <= limit)
                break;
            total -= f.file_size();
            std::error_code ec;
            std::filesystem::remove(f.path(), ec);
            auto meta = f.path();
            meta.replace_extension(L".image.json");
            std::filesystem::remove(meta, ec);
        }
    });
}
void clearCache()
{
    withDataLock([&] {
        for (const auto &f : std::filesystem::directory_iterator(cache()))
        {
            const auto ext = f.path().extension();
            if (ext == L".img" || ext == L".feed" || ext == L".json")
            {
                std::error_code ec;
                std::filesystem::remove(f.path(), ec);
            }
        }
    });
}
void pruneRemovedFeeds(const Settings &settings)
{
    withDataLock([&] {
        std::set<std::wstring> retained;
        std::set<std::string> feedKeys;
        for (const auto &f : settings.feeds)
        {
            feedKeys.insert(digest(f.url));
            if (!f.policy.enabled)
                continue;
            auto path = cache() / digest(f.url);
            path += L".feed";
            try
            {
                const auto parsed = parseFeed(readFile(path, 8 * 1024 * 1024), f.url);
                for (const auto &image : parsed.images)
                    retained.insert(image.url);
            }
            catch (...)
            {
            }
        }
        for (const auto &f : std::filesystem::directory_iterator(cache()))
        {
            if (f.path().extension() == L".feed" && !feedKeys.contains(f.path().stem().string()))
            {
                std::error_code ec;
                std::filesystem::remove(f.path(), ec);
                auto meta = f.path();
                meta += L".json";
                std::filesystem::remove(meta, ec);
            }
            if (f.path().filename().wstring().ends_with(L".image.json"))
                try
                {
                    auto m = readJson(f.path());
                    if (!retained.contains(wide(m.at("url").get<std::string>())))
                    {
                        std::error_code ec;
                        auto image =
                            f.path().parent_path() / (f.path().filename().wstring().substr(0, 64) + L".img");
                        std::filesystem::remove(image, ec);
                        std::filesystem::remove(f.path(), ec);
                    }
                }
                catch (...)
                {
                }
        }
    });
}
} // namespace mrss
