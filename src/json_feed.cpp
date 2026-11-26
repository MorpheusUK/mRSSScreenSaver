// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/settings.hpp"
#include <shlwapi.h>
#include <algorithm>
#include <set>

namespace mrss
{
namespace
{
using json = nlohmann::json;
std::wstring string(const json &object, const char *key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? wide(it->get_ref<const std::string &>()) : L"";
}
std::wstring imageUrl(std::wstring_view base, const std::wstring &value)
{
    if (value.empty() || value.size() > 16384)
        return {};
    wchar_t combined[16385]{};
    DWORD length = static_cast<DWORD>(std::size(combined));
    if (FAILED(UrlCombineW(std::wstring(base).c_str(), value.c_str(), combined, &length, 0)))
        return {};
    const auto valid = validateUrl(std::wstring_view(combined, length), true);
    return valid.url ? valid.url->original : L"";
}
std::wstring authors(const json &object)
{
    std::wstring result;
    if (object.contains("authors") && object.at("authors").is_array())
    {
        for (const auto &author : object.at("authors"))
            if (author.is_object())
            {
                const auto name = string(author, "name");
                if (!name.empty())
                    result += (result.empty() ? L"" : L", ") + name;
            }
    }
    else if (object.contains("author") && object.at("author").is_object())
        result = string(object.at("author"), "name");
    return result;
}
std::optional<Time> peapixDate(const std::wstring &date)
{
    // Peapix Bing publishes calendar dates, not timestamps. Use UTC midnight;
    // Spotlight currently omits dates and must remain undated.
    if (date.size() == 10)
        return parseDate(date + L"T00:00:00Z");
    return parseDate(date);
}
} // namespace

Feed parseJsonImageFeed(std::span<const std::byte> bytes, std::wstring_view baseUrl)
{
    if (bytes.empty() || bytes.size() > 8 * 1024 * 1024)
        throw ParseError("JSON feed exceeds size limit or is empty");
    try
    {
        unsigned nodes = 0;
        std::vector<std::set<std::string>> keys;
        const auto document = json::parse(reinterpret_cast<const char *>(bytes.data()),
                                          reinterpret_cast<const char *>(bytes.data()) + bytes.size(),
                                          [&](int depth, json::parse_event_t event, json &value) {
                                              if (depth > 64 || ++nodes > 100000)
                                                  throw ParseError("JSON feed structure exceeds limits");
                                              if (event == json::parse_event_t::object_start)
                                                  keys.emplace_back();
                                              else if (event == json::parse_event_t::object_end)
                                                  keys.pop_back();
                                              else if (event == json::parse_event_t::key &&
                                                       !keys.back().insert(value.get<std::string>()).second)
                                                  throw ParseError("JSON feed contains duplicate keys");
                                              if (value.is_string() &&
                                                  value.get_ref<const std::string &>().size() > 1024 * 1024)
                                                  throw ParseError("JSON feed text exceeds limits");
                                              return true;
                                          });
        Feed result;
        const json *items = nullptr;
        const bool peapix = document.is_array();
        bool versionOne = false;
        if (peapix)
        {
            const auto base = validateUrl(baseUrl, true);
            auto path = std::wstring(baseUrl);
            if (base.url)
            {
                const auto start = path.find(L'/', path.find(L"://") + 3);
                path = start == path.npos ? L"/" : path.substr(start);
                path = path.substr(0, path.find_first_of(L"?#"));
            }
            if (!base.url || _wcsicmp(base.url->host.c_str(), L"peapix.com") != 0 ||
                (path != L"/spotlight/feed" && path != L"/bing/feed"))
                throw ParseError("Unsupported JSON array feed (only Peapix Spotlight/Bing supported)");
            result.title =
                path == L"/bing/feed" ? L"Peapix: Bing image of the day" : L"Peapix: Windows Spotlight";
            items = &document;
        }
        else
        {
            if (!document.is_object())
                throw ParseError("Unsupported JSON feed format");
            const auto version = string(document, "version");
            versionOne = version == L"https://jsonfeed.org/version/1";
            if (!versionOne && version != L"https://jsonfeed.org/version/1.1")
                throw ParseError("Unsupported JSON Feed version (expected 1 or 1.1)");
            result.title = string(document, "title");
            if (result.title.empty() || !document.contains("items") || !document.at("items").is_array())
                throw ParseError("JSON Feed requires a title and items array");
            items = &document.at("items");
        }
        if (items->size() > 10000)
            throw ParseError("JSON feed contains too many items");
        std::set<std::wstring> seen;
        std::size_t extractedText = 0;
        auto append = [&](MediaImage image, const std::wstring &url) {
            image.url = imageUrl(baseUrl, url);
            if (image.url.empty() || !seen.insert(image.url).second)
                return;
            extractedText += (image.url.size() + image.title.size() + image.description.size() +
                              image.author.size() + image.keywords.size()) *
                             sizeof(wchar_t);
            if (result.images.size() >= 10000 || extractedText > 16 * 1024 * 1024)
                throw ParseError("Extracted JSON feed images exceed limits");
            result.images.push_back(std::move(image));
        };
        const auto feedAuthors = peapix ? L"" : authors(document);
        for (const auto &item : *items)
        {
            if (!item.is_object())
                continue;
            MediaImage image;
            image.title = string(item, "title");
            // Neither JSON Feed nor Peapix declares standard maturity ratings.
            image.rating = Rating::unrated;
            if (peapix)
            {
                image.description = string(item, "copyright");
                image.author = image.description;
                image.published = peapixDate(string(item, "date"));
                auto url = string(item, "fullUrl");
                if (url.empty())
                    url = string(item, "imageUrl");
                append(std::move(image), url);
                continue;
            }
            const auto id = item.find("id");
            if (id == item.end() || !(id->is_string() ? !id->get_ref<const std::string &>().empty()
                                                      : versionOne && (id->is_number() || id->is_boolean())))
                continue;
            image.description = string(item, "content_text");
            if (image.description.empty())
                image.description = string(item, "summary");
            if (image.description.empty())
                image.description = string(item, "content_html");
            image.published = parseDate(string(item, "date_published"));
            image.author = item.contains("authors") || item.contains("author") ? authors(item) : feedAuthors;
            if (item.contains("tags") && item.at("tags").is_array())
                for (const auto &tag : item.at("tags"))
                    if (tag.is_string())
                        image.keywords += L" " + wide(tag.get_ref<const std::string &>());
            // Explicit item images and image attachments only. Never scrape
            // content_html, use banner images, or follow pagination links.
            append(image, string(item, "image"));
            if (item.contains("attachments") && item.at("attachments").is_array())
                for (const auto &attachment : item.at("attachments"))
                    if (attachment.is_object() && string(attachment, "mime_type").starts_with(L"image/"))
                        append(image, string(attachment, "url"));
        }
        return result;
    }
    catch (const json::exception &)
    {
        // Parser exceptions can contain response text or token-bearing URLs.
        throw ParseError("Invalid JSON feed");
    }
}
} // namespace mrss
