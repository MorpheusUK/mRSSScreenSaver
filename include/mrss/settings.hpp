// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include "feed.hpp"
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

namespace mrss
{
enum class FilterMode
{
    none,
    boolean,
    regex
};
struct Filter
{
    FilterMode mode = FilterMode::none;
    std::wstring expression;
    std::wstring exclude;
    bool caseSensitive = false;
    unsigned fields = 3; // title=1 description=2 URL=4 author=8 keywords=16
};
struct MetadataOptions
{
    bool title = false, author = false, top = false, bold = false, italic = false;
    unsigned seconds = 5, points = 24;
    std::wstring font = L"Segoe UI";
};
struct FeedConfig
{
    std::wstring title, url;
    FeedPolicy policy;
    Filter filter;
    std::optional<MetadataOptions> metadata; // absent means inherit global options
};
struct Settings
{
    Defaults defaults;
    std::vector<FeedConfig> feeds;
    bool random = false, balanceFeeds = false, fill = false;
    unsigned theme = 0; // system, light, dark
    unsigned randomEffects = 127;
    unsigned background = 0;
    unsigned hdrNits = 250;
    MetadataOptions metadata;
};
const MetadataOptions &effectiveMetadata(const Settings &settings, const FeedConfig &feed);
unsigned effectiveMetadataSeconds(const Settings &settings, const FeedConfig &feed);
std::wstring metadataText(const MediaImage &image, const MetadataOptions &options);
std::string utf8(std::wstring_view value);
std::wstring wide(std::string_view value);
std::filesystem::path dataDirectory();
void setDataDirectory(std::filesystem::path path); // explicit isolated test/portable data root
std::vector<std::byte> readFile(const std::filesystem::path &path, std::size_t limit);
void atomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes, bool backup = false);
void writeText(const std::filesystem::path &path, std::string_view value, bool backup = false);
std::string digest(std::wstring_view value);
nlohmann::json encodeSettings(const Settings &settings);
nlohmann::json parseJson(std::string_view text);
Settings decodeSettings(const nlohmann::json &json);
FeedConfig decodeOutline(const Outline &outline);
Settings loadSettings(std::wstring &recovery);
void saveSettings(const Settings &settings);
void withDataLock(const std::function<void()> &action);
std::wstring filterText(const MediaImage &image, unsigned fields);
bool matchesFilter(const Filter &filter, std::wstring_view text); // throws on invalid/over-budget expression
void validateFilter(const Filter &filter);
} // namespace mrss
