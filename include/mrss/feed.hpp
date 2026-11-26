// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#pragma once
#include "policy.hpp"
#include <stdexcept>

namespace mrss
{
struct ParseError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
struct MediaImage
{
    std::wstring url;
    std::wstring title;
    std::wstring description;
    Rating rating = Rating::unrated;
    std::optional<Time> published;
    std::wstring author;
    std::wstring keywords;
};
struct Feed
{
    std::wstring title;
    std::vector<MediaImage> images;
};
struct Outline
{
    std::wstring title;
    std::wstring url;
    std::string settings;
};
// Bounded RSS/Atom XML, JSON Feed 1/1.1, or known Peapix endpoint parsing.
// COM must be initialised for XML. JSON is UTF-8; XML retains its encoding.
Feed parseFeed(std::span<const std::byte> xml, std::wstring_view baseUrl);
Feed parseJsonImageFeed(std::span<const std::byte> json, std::wstring_view baseUrl);
std::vector<Outline> parseOpml(std::span<const std::byte> xml);
std::string exportOpml(std::span<const Outline> feeds);
std::optional<Time> parseDate(std::wstring_view value);
} // namespace mrss
