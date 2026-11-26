// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include "network.hpp"
#include <functional>

namespace mrss
{
struct Item
{
    MediaImage image;
    FeedConfig feed;
    std::size_t feedIndex = 0;
};
struct FeedStatus
{
    std::wstring title, message;
    unsigned total = 0, eligible = 0;
};
struct Catalog
{
    std::vector<Item> items;
    std::vector<FeedStatus> status;
};
Catalog buildCatalog(const Settings &settings, bool refresh, std::stop_token stop,
                     std::function<void(std::size_t, const FeedStatus &)> progress = {},
                     bool cacheOnly = false);
std::filesystem::path imageFile(const Item &item, const Defaults &defaults, std::stop_token stop,
                                bool cacheOnly = false);
void trimCache(std::uint64_t limit);
void clearCache();
void pruneRemovedFeeds(const Settings &settings);
std::wstring eligibility(const MediaImage &image, const FeedConfig &feed, const Defaults &defaults);
} // namespace mrss
