// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mrss
{
using Time = std::chrono::sys_seconds;
enum class AgeUnit
{
    minutes,
    hours,
    days,
    weeks,
    months
};
enum class Transition
{
    fade,
    slide,
    crossFade,
    flip,
    random,
    none,
    cube,
    collage,
    panZoom
};
enum class Rating
{
    unrestricted,
    adult,
    unrated,
    unknown
};
enum class ImageFormat
{
    jpeg,
    png,
    gif,
    bmp,
    tiff,
    webp,
    heif
};

struct AgeLimit
{
    bool entireFeed = false;
    unsigned value = 7;
    AgeUnit unit = AgeUnit::days;
    bool includeUndated = false;
};
struct Defaults
{
    AgeLimit age;
    unsigned displaySeconds = 10;
    unsigned transitionSeconds = 2;
    unsigned refreshHours = 24;
    std::uint64_t cacheBytes = 50ULL * 1024 * 1024;
    unsigned simultaneousDownloads = 4;
    unsigned downloadsPerServer = 2;
    unsigned prefetchImages = 2;
    bool allowMature = false;
    bool excludeUnrated = false;
    Transition transition = Transition::crossFade;
    std::vector<std::wstring> excludedUrlText;
};
struct FeedPolicy
{
    bool enabled = true;
    bool allowHttp = false;
    bool allowPrivateNetwork = false;
    AgeLimit age;
    bool overrideAge = false;
    std::optional<unsigned> displaySeconds;
    std::vector<std::wstring> excludedUrlText;
    // Mature content is deliberately global only.
};
struct Url
{
    std::wstring original;
    std::wstring host;
    unsigned short port = 0;
    bool secure = true;
};
struct UrlResult
{
    std::optional<Url> url;
    std::wstring error;
};
// Structural validation only. The network layer must also validate all resolved
// addresses and redirect destinations before connecting.
UrlResult validateUrl(std::wstring_view value, bool allowHttp);
std::wstring redactUrl(std::wstring_view value);
bool excludedUrl(std::wstring_view url, std::span<const std::wstring> patterns);
std::optional<ImageFormat> identifyImage(std::span<const std::byte> header);
Time ageCutoff(Time now, unsigned value, AgeUnit unit);
bool eligibleAge(std::optional<Time> published, Time now, const AgeLimit &limit);
bool eligibleRating(Rating rating, const Defaults &settings);
unsigned effectiveDisplaySeconds(const Defaults &settings, const FeedPolicy &feed);
const AgeLimit &effectiveAgeLimit(const Defaults &settings, const FeedPolicy &feed);
std::vector<std::size_t> balancedOrder(std::span<const std::size_t> feedIds, std::uint64_t seed);

// Indices in a shuffle cycle are unique. New source collections start a new
// cycle; the first item avoids the previous item when possible.
class Shuffle
{
  public:
    explicit Shuffle(std::uint64_t seed);
    void reset(std::size_t count);
    std::optional<std::size_t> next();

  private:
    std::mt19937_64 random_;
    std::vector<std::size_t> order_;
    std::size_t cursor_ = 0;
    std::optional<std::size_t> previous_;
    void shuffle();
};
} // namespace mrss
