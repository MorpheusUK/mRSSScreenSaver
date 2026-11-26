// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/network.hpp"
#include <objbase.h>
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        return 1;
    int result = 0;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--opml")
    {
        try
        {
            const auto outlines =
                mrss::parseOpml(mrss::readFile(std::filesystem::path(argv[2]), 8 * 1024 * 1024));
            unsigned ageOverrides = 0, durationOverrides = 0;
            for (const auto &outline : outlines)
            {
                const auto feed = mrss::decodeOutline(outline);
                if (feed.policy.overrideAge)
                    ++ageOverrides;
                if (feed.policy.displaySeconds)
                    ++durationOverrides;
            }
            std::cout << "OPML import valid: " << outlines.size()
                      << " unique feeds; explicit age overrides: " << ageOverrides
                      << "; display-time overrides: " << durationOverrides << "\n";
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << "\n";
            result = 1;
        }
        CoUninitialize();
        return result;
    }
    for (int i = 1; i < argc; ++i)
    {
        std::wstring url = argv[i];
        if (url.starts_with(L"http://"))
            url.replace(0, 7, L"https://");
        try
        {
            mrss::FeedPolicy policy;
            const auto response = mrss::download(url, policy, {}, 8 * 1024 * 1024);
            const auto feed = mrss::parseFeed(response.body, response.finalUrl);
            unsigned adult = 0, undated = 0;
            for (const auto &image : feed.images)
            {
                if (image.rating == mrss::Rating::adult)
                    ++adult;
                if (!image.published)
                    ++undated;
            }
            std::cout << mrss::utf8(mrss::redactUrl(url)) << "\nTitle: " << mrss::utf8(feed.title)
                      << "\nImages: " << feed.images.size() << "; adult-rated: " << adult
                      << "; undated: " << undated << "\n";
        }
        catch (const std::exception &e)
        {
            std::cout << mrss::utf8(mrss::redactUrl(url)) << "\nError: " << e.what() << "\n";
            result = 1;
        }
    }
    CoUninitialize();
    return result;
}
