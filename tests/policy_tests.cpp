// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/policy.hpp"
#include <gtest/gtest.h>
#include <array>
#include <set>

using namespace mrss;
using namespace std::chrono;
namespace
{
Time at(int y, unsigned m, unsigned d)
{
    return sys_days(year(y) / month(m) / day(d));
}
std::span<const std::byte> bytes(std::string_view s)
{
    return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
}
} // namespace
TEST(Defaults, AgreedValues)
{
    Defaults d;
    EXPECT_EQ(d.displaySeconds, 10u);
    EXPECT_EQ(d.transitionSeconds, 2u);
    EXPECT_EQ(d.refreshHours, 24u);
    EXPECT_EQ(d.cacheBytes, 50ULL * 1024 * 1024);
    EXPECT_EQ(d.simultaneousDownloads, 4u);
    EXPECT_FALSE(d.allowMature);
}
TEST(Urls, HttpsDefaultAndHttpOverride)
{
    EXPECT_TRUE(validateUrl(L"https://example.com/feed?token=abc", false).url);
    EXPECT_FALSE(validateUrl(L"http://example.com/feed", false).url);
    EXPECT_TRUE(validateUrl(L"http://example.com/feed", true).url);
}
TEST(Urls, RefuseCredentialsAndOtherSchemes)
{
    for (auto url : {L"https://user:pass@example.com/", L"https://@example.com/", L"file:///C:/image.jpg",
                     L"ftp://example.com/", L"https://example.com/\r\nheader", L"https://example.com\\evil/"})
        EXPECT_FALSE(validateUrl(url, true).url);
}
TEST(Urls, PreserveTokensButRedactDiagnostics)
{
    auto url = validateUrl(L"https://example.com/private/token?signature=secret", false);
    ASSERT_TRUE(url.url);
    EXPECT_EQ(url.url->original, L"https://example.com/private/token?signature=secret");
    auto diagnostic = redactUrl(url.url->original);
    EXPECT_EQ(diagnostic.find(L"secret"), std::wstring::npos);
    EXPECT_EQ(diagnostic.find(L"token"), std::wstring::npos);
    EXPECT_EQ(redactUrl(L"https://user:secret@example.com/"), L"[invalid URL redacted]");
}
TEST(Exclusions, LiteralCaseInsensitiveAndEmptyIgnored)
{
    const std::vector<std::wstring> p{L"", L"blur_", L"[draft]"};
    EXPECT_TRUE(excludedUrl(L"https://example.com/BLUR_photo.jpg", p));
    EXPECT_TRUE(excludedUrl(L"https://example.com/[draft].png", p));
    EXPECT_FALSE(excludedUrl(L"https://example.com/draft.png", p));
}
TEST(Age, BoundariesAndMissingDates)
{
    const auto now = at(2026, 11, 25);
    AgeLimit limit;
    EXPECT_TRUE(eligibleAge(now - days(7), now, limit));
    EXPECT_FALSE(eligibleAge(now - days(7) - seconds(1), now, limit));
    EXPECT_FALSE(eligibleAge({}, now, limit));
    EXPECT_FALSE(eligibleAge(now + hours(1), now, limit));
    limit.includeUndated = true;
    EXPECT_TRUE(eligibleAge({}, now, limit));
    limit.entireFeed = true;
    EXPECT_TRUE(eligibleAge(now - days(500), now, limit));
}
TEST(Age, CalendarMonthsClampAndKeepTimeOfDay)
{
    EXPECT_EQ(ageCutoff(at(2024, 3, 31) + hours(12), 1, AgeUnit::months), at(2024, 2, 29) + hours(12));
    EXPECT_EQ(ageCutoff(at(2025, 3, 31), 1, AgeUnit::months), at(2025, 2, 28));
    EXPECT_EQ(ageCutoff(at(2026, 1, 31), 2, AgeUnit::months), at(2025, 11, 30));
    EXPECT_THROW(ageCutoff(at(2026, 1, 1), 0, AgeUnit::days), std::invalid_argument);
}
TEST(Ratings, GlobalMatureGateAndUnratedPolicy)
{
    Defaults d;
    EXPECT_FALSE(eligibleRating(Rating::adult, d));
    EXPECT_TRUE(eligibleRating(Rating::unrated, d));
    d.excludeUnrated = true;
    EXPECT_FALSE(eligibleRating(Rating::unrated, d));
    EXPECT_FALSE(eligibleRating(Rating::unknown, d));
    d.allowMature = true;
    EXPECT_TRUE(eligibleRating(Rating::adult, d));
    EXPECT_TRUE(eligibleRating(Rating::unrestricted, d));
}
TEST(Timing, ExplicitOverrideAndValidation)
{
    Defaults d;
    FeedPolicy f;
    EXPECT_EQ(effectiveDisplaySeconds(d, f), 10u);
    f.displaySeconds = 25;
    EXPECT_EQ(effectiveDisplaySeconds(d, f), 25u);
    f.displaySeconds = 0;
    EXPECT_THROW(effectiveDisplaySeconds(d, f), std::invalid_argument);
}
TEST(Formats, RejectExecutablesDocumentsAndGenericMedia)
{
    for (auto s : {"MZexecutable", "%PDF-1.7", "<svg></svg>", "<html>image.jpg</html>"})
        EXPECT_FALSE(identifyImage(bytes(s)));
    EXPECT_FALSE(identifyImage(bytes(std::string_view("\0\0\0\x10"
                                                      "ftypisom\0\0\0\0",
                                                      16))));
}
TEST(Formats, RecogniseAllowlistedSignatures)
{
    EXPECT_EQ(identifyImage(bytes(std::string_view("\xff\xd8\xff", 3))), ImageFormat::jpeg);
    EXPECT_EQ(identifyImage(bytes(std::string_view("\x89PNG\r\n\x1a\n", 8))), ImageFormat::png);
    EXPECT_EQ(identifyImage(bytes("GIF89a")), ImageFormat::gif);
    EXPECT_EQ(identifyImage(bytes("BM")), ImageFormat::bmp);
    EXPECT_EQ(identifyImage(bytes(std::string_view("II\x2a\0", 4))), ImageFormat::tiff);
    EXPECT_EQ(identifyImage(bytes("RIFF1234WEBP")), ImageFormat::webp);
    EXPECT_EQ(identifyImage(bytes(std::string_view("\0\0\0\x10"
                                                   "ftypheic\0\0\0\0",
                                                   16))),
              ImageFormat::heif);
}
TEST(Shuffle, UniqueCycleAndNoBoundaryRepeat)
{
    Shuffle s(1234);
    s.reset(20);
    for (int cycle = 0; cycle < 5; ++cycle)
    {
        std::set<std::size_t> seen;
        for (int i = 0; i < 20; ++i)
        {
            auto item = s.next();
            ASSERT_TRUE(item);
            seen.insert(*item);
        }
        EXPECT_EQ(seen.size(), 20u);
    }
    s.reset(2);
    const auto a = s.next();
    const auto b = s.next();
    EXPECT_NE(a, b);
    EXPECT_NE(b, s.next());
}
TEST(Shuffle, EmptyAndSingleItem)
{
    Shuffle s(1);
    s.reset(0);
    EXPECT_FALSE(s.next());
    s.reset(1);
    EXPECT_EQ(s.next(), 0u);
    EXPECT_EQ(s.next(), 0u);
}
TEST(Shuffle, BalancedFeedsInterleaveWithoutRepeats)
{
    const std::vector<std::size_t> ids{0, 0, 0, 0, 1, 1};
    const auto order = balancedOrder(ids, 1234);
    ASSERT_EQ(order.size(), ids.size());
    EXPECT_EQ(std::set<std::size_t>(order.begin(), order.end()).size(), ids.size());
    EXPECT_NE(ids[order[0]], ids[order[1]]);
    EXPECT_NE(ids[order[2]], ids[order[3]]);
}
