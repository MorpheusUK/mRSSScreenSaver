// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include "settings.hpp"
#include <stop_token>

namespace mrss
{
struct Response
{
    unsigned status = 0;
    std::wstring finalUrl, etag, modified;
    std::vector<std::byte> body;
};
bool publicAddress(std::wstring_view numericAddress);
bool offlineHint();
Response download(std::wstring url, const FeedPolicy &policy, std::span<const std::wstring> exclusions,
                  std::size_t limit, std::stop_token stop = {}, std::wstring_view etag = {},
                  std::wstring_view modified = {}, unsigned maxConcurrent = 4);
} // namespace mrss
