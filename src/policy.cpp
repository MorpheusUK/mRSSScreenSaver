// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/policy.hpp"
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <map>
#include <stdexcept>

namespace mrss
{
namespace
{
std::wstring lower(std::wstring_view text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        throw std::length_error("Text too long");
    std::wstring result(text.size(), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), static_cast<int>(text.size()),
                       result.data(), static_cast<int>(result.size()), nullptr, nullptr, 0))
        throw std::runtime_error("Case mapping failed");
    return result;
}
bool bytesAt(std::span<const std::byte> b, std::size_t offset, std::string_view signature)
{
    if (offset > b.size() || signature.size() > b.size() - offset)
        return false;
    for (std::size_t i = 0; i < signature.size(); ++i)
        if (std::to_integer<unsigned char>(b[offset + i]) != static_cast<unsigned char>(signature[i]))
            return false;
    return true;
}
} // namespace
UrlResult validateUrl(std::wstring_view value, bool allowHttp)
{
    if (value.empty() || value.size() > 16384)
        return {{}, L"URL is empty or too long"};
    for (const auto c : value)
        if (c <= L' ' || c == 127 || c == L'\\')
            return {{}, L"URL contains spaces, controls or backslashes"};
    const std::wstring owned(value);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength =
        parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(owned.c_str(), static_cast<DWORD>(owned.size()), 0, &parts))
        return {{}, L"Invalid URL"};
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)
        return {{}, L"Only HTTPS and explicitly allowed HTTP are supported"};
    if (parts.nScheme == INTERNET_SCHEME_HTTP && !allowHttp)
        return {{}, L"HTTP requires this feed's Allow HTTP option"};
    if (!parts.dwHostNameLength)
        return {{}, L"URL has no host"};
    if (parts.dwUserNameLength || parts.dwPasswordLength)
        return {{}, L"Username/password URLs are not supported"};
    // Empty user information, e.g. https://@host/, is also rejected.
    const auto authorityStart = owned.find(L"://") + 3;
    const auto authorityEnd = owned.find_first_of(L"/?#", authorityStart);
    if (owned.substr(authorityStart, authorityEnd - authorityStart).find(L'@') != std::wstring::npos)
        return {{}, L"User information is not supported"};
    if (owned.find(L'#') != std::wstring::npos)
        return {{}, L"URL fragments are not supported"};
    return {Url{owned, lower(std::wstring_view(parts.lpszHostName, parts.dwHostNameLength)), parts.nPort,
                parts.nScheme == INTERNET_SCHEME_HTTPS},
            {}};
}
std::wstring redactUrl(std::wstring_view value)
{
    // Never put an unparsed URL into a diagnostic: credentials may be in its
    // authority or path, not just in its query string.
    const auto parsed = validateUrl(value, true);
    if (!parsed.url)
        return L"[invalid URL redacted]";
    return (parsed.url->secure ? L"https://" : L"http://") + parsed.url->host + L"/[path and query redacted]";
}
bool excludedUrl(std::wstring_view url, std::span<const std::wstring> patterns)
{
    const auto folded = lower(url);
    return std::any_of(patterns.begin(), patterns.end(), [&](const auto &p) {
        return !p.empty() && folded.find(lower(p)) != std::wstring::npos;
    });
}
std::optional<ImageFormat> identifyImage(std::span<const std::byte> b)
{
    if (bytesAt(b, 0, std::string_view("\xff\xd8\xff", 3)))
        return ImageFormat::jpeg;
    if (bytesAt(b, 0, std::string_view("\x89PNG\r\n\x1a\n", 8)))
        return ImageFormat::png;
    if (bytesAt(b, 0, "GIF87a") || bytesAt(b, 0, "GIF89a"))
        return ImageFormat::gif;
    if (bytesAt(b, 0, "BM"))
        return ImageFormat::bmp;
    if (bytesAt(b, 0, std::string_view("II\x2a\0", 4)) || bytesAt(b, 0, std::string_view("MM\0\x2a", 4)))
        return ImageFormat::tiff;
    if (bytesAt(b, 0, "RIFF") && bytesAt(b, 8, "WEBP"))
        return ImageFormat::webp;
    // Recognise HEVC still-image brands only, not generic ISO media containers
    // or AVIF. Decoding must independently confirm the primary still image.
    if (b.size() >= 16 && bytesAt(b, 4, "ftyp"))
    {
        const auto boxSize = (std::to_integer<unsigned>(b[0]) << 24) |
                             (std::to_integer<unsigned>(b[1]) << 16) |
                             (std::to_integer<unsigned>(b[2]) << 8) | std::to_integer<unsigned>(b[3]);
        if (boxSize < 16 || boxSize % 4 != 0)
            return {};
        const auto end = (std::min)(b.size(), static_cast<std::size_t>(boxSize));
        for (std::size_t i = 8; i + 4 <= end; i += 4)
        {
            if (i == 12)
                continue; // minor version is not a brand
            if (bytesAt(b, i, "heic") || bytesAt(b, i, "heix"))
                return ImageFormat::heif;
        }
    }
    return {};
}
Time ageCutoff(Time now, unsigned value, AgeUnit unit)
{
    if (!value || value > 100000)
        throw std::invalid_argument("Age must be between 1 and 100000");
    using namespace std::chrono;
    switch (unit)
    {
    case AgeUnit::minutes:
        return now - minutes(value);
    case AgeUnit::hours:
        return now - hours(value);
    case AgeUnit::days:
        return now - days(value);
    case AgeUnit::weeks:
        return now - days(static_cast<std::int64_t>(value) * 7);
    case AgeUnit::months: {
        if (value > 1200)
            throw std::invalid_argument("Month limit exceeds 100 years");
        const auto day = floor<days>(now);
        const year_month_day date(day);
        const auto target = date.year() / date.month() - months(value);
        const auto lastDay = year_month_day(target / last_spec{}).day();
        return sys_days(target / (std::min)(date.day(), lastDay)) + (now - day);
    }
    }
    throw std::invalid_argument("Invalid age unit");
}
bool eligibleAge(std::optional<Time> published, Time now, const AgeLimit &limit)
{
    if (limit.entireFeed)
        return true;
    if (!published)
        return limit.includeUndated;
    // Permit a small publisher clock skew, not arbitrarily future-dated items.
    return *published >= ageCutoff(now, limit.value, limit.unit) &&
           *published <= now + std::chrono::minutes(5);
}
bool eligibleRating(Rating rating, const Defaults &settings)
{
    if (rating == Rating::adult)
        return settings.allowMature;
    if (rating == Rating::unrated || rating == Rating::unknown)
        return !settings.excludeUnrated;
    return true;
}
unsigned effectiveDisplaySeconds(const Defaults &settings, const FeedPolicy &feed)
{
    const auto value = feed.displaySeconds.value_or(settings.displaySeconds);
    if (value < 1 || value > 86400)
        throw std::invalid_argument("Display time must be 1 to 86400 seconds");
    return value;
}
const AgeLimit &effectiveAgeLimit(const Defaults &settings, const FeedPolicy &feed)
{
    return feed.overrideAge ? feed.age : settings.age;
}
Shuffle::Shuffle(std::uint64_t seed) : random_(seed) {}
std::vector<std::size_t> balancedOrder(std::span<const std::size_t> feedIds, std::uint64_t seed)
{
    std::mt19937_64 random(seed);
    std::map<std::size_t, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < feedIds.size(); ++i)
        groups[feedIds[i]].push_back(i);
    for (auto &[id, items] : groups)
    {
        (void)id;
        std::shuffle(items.begin(), items.end(), random);
    }
    std::vector<std::size_t> result;
    while (result.size() < feedIds.size())
    {
        std::vector<std::size_t> ids;
        for (const auto &[id, items] : groups)
            if (!items.empty())
                ids.push_back(id);
        std::shuffle(ids.begin(), ids.end(), random);
        for (const auto id : ids)
        {
            result.push_back(groups[id].back());
            groups[id].pop_back();
        }
    }
    return result;
}
void Shuffle::shuffle()
{
    std::shuffle(order_.begin(), order_.end(), random_);
    if (order_.size() > 1 && previous_ && order_[0] == *previous_)
        std::swap(order_[0], order_[1]);
    cursor_ = 0;
}
void Shuffle::reset(std::size_t count)
{
    order_.resize(count);
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    shuffle();
}
std::optional<std::size_t> Shuffle::next()
{
    if (order_.empty())
        return {};
    if (cursor_ == order_.size())
        shuffle();
    previous_ = order_[cursor_++];
    return previous_;
}
} // namespace mrss
