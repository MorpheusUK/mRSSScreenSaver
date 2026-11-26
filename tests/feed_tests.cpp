// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/feed.hpp"
#include <windows.h>
#include <objbase.h>
#include <gtest/gtest.h>

using namespace mrss;
namespace
{
std::span<const std::byte> bytes(std::string_view s)
{
    return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
}
class XmlTest : public testing::Test
{
    HRESULT com_ = E_FAIL;
    void SetUp() override
    {
        com_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(com_));
    }
    void TearDown() override
    {
        if (SUCCEEDED(com_))
            CoUninitialize();
    }
};
} // namespace
TEST_F(XmlTest, RssRatingInheritanceAndOverrides)
{
    const auto feed = parseFeed(bytes(R"(<rss version="2.0" xmlns:m="http://search.yahoo.com/mrss/">
      <channel><title>Test &amp; photos</title><m:rating>adult</m:rating><item><title>Picture</title>
      <pubDate>Wed, 25 Nov 2026 12:00:00 GMT</pubDate>
      <m:content medium="image" url="/a.jpg"/>
      <m:group><m:rating>nonadult</m:rating><m:content type="image/png" url="/b.png"/>
      <m:content medium="image" url="/c.jpg"><m:rating>adult</m:rating></m:content></m:group>
      <m:content medium="video" type="video/mp4" url="/evil.mp4"/>
      </item></channel></rss>)"),
                                L"https://example.com/feed");
    EXPECT_EQ(feed.title, L"Test & photos");
    ASSERT_EQ(feed.images.size(), 3u);
    EXPECT_EQ(feed.images[0].url, L"https://example.com/a.jpg");
    EXPECT_EQ(feed.images[0].rating, Rating::adult);
    EXPECT_TRUE(feed.images[0].published);
    EXPECT_EQ(feed.images[1].rating, Rating::unrestricted);
    EXPECT_EQ(feed.images[2].rating, Rating::adult);
}
TEST_F(XmlTest, AtomEnclosuresAndLegacyAdult)
{
    const auto feed =
        parseFeed(bytes(R"(<feed xmlns="http://www.w3.org/2005/Atom" xmlns:m="http://search.yahoo.com/mrss/">
      <title>Atom</title><entry><title>One</title><published>2026-11-25T12:30:00+01:00</published>
      <m:adult>true</m:adult><link rel="enclosure" type="image/webp" href="https://example.com/a.webp"/>
      <link rel="enclosure" type="application/pdf" href="https://example.com/a.pdf"/>
      </entry></feed>)"),
                  L"https://example.com/feed");
    ASSERT_EQ(feed.images.size(), 1u);
    EXPECT_EQ(feed.images[0].rating, Rating::adult);
    EXPECT_EQ(feed.images[0].published, parseDate(L"2026-11-25T11:30:00Z"));
}
TEST_F(XmlTest, RejectDoctypeMalformedXmlAndWrongNamespace)
{
    EXPECT_THROW(
        parseFeed(
            bytes(
                R"(<!DOCTYPE rss [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><rss version="2.0"><channel><title>&x;</title></channel></rss>)"),
            L"https://example.com/"),
        ParseError);
    EXPECT_THROW(parseFeed(bytes("<rss version=\"2.0\"><channel>"), L"https://example.com/"), ParseError);
    EXPECT_THROW(parseFeed(bytes("<feed><entry/></feed>"), L"https://example.com/"), ParseError);
}
TEST_F(XmlTest, NeverScrapeHtmlOrAcceptDocuments)
{
    const auto feed = parseFeed(bytes(R"(<rss version="2.0"><channel><item>
      <description><![CDATA[<img src="https://example.com/photo.jpg"/>]]></description>
      <enclosure type="application/pdf" url="https://example.com/doc.pdf"/>
      </item></channel></rss>)"),
                                L"https://example.com/feed");
    EXPECT_TRUE(feed.images.empty());
}
TEST_F(XmlTest, NestedOpmlDeduplicatesAndRoundTripsTokens)
{
    const auto list = parseOpml(bytes(R"(<opml version="2.0"><body><outline text="Folder">
      <outline title="A &amp; B" xmlUrl="https://example.com/feed?token=abc&amp;x=1"/>
      <outline title="Duplicate" xmlUrl="https://example.com/feed?token=abc&amp;x=1"/>
      </outline></body></opml>)"));
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].title, L"A & B");
    const auto exported = exportOpml(list);
    const auto roundTrip = parseOpml(bytes(exported));
    ASSERT_EQ(roundTrip.size(), 1u);
    EXPECT_EQ(roundTrip[0].url, list[0].url);
    EXPECT_EQ(roundTrip[0].title, list[0].title);
}
TEST_F(XmlTest, OpmlRejectsEmbeddedCredentialsAndEntities)
{
    EXPECT_THROW(parseOpml(bytes(
                     R"(<opml><body><outline xmlUrl="https://user:pass@example.com/feed"/></body></opml>)")),
                 ParseError);
    EXPECT_THROW(parseOpml(bytes(R"(<!DOCTYPE opml [<!ENTITY x "x">]><opml><body/></opml>)")), ParseError);
}
TEST(Dates, ValidateAndConvertOffsets)
{
    EXPECT_EQ(parseDate(L"Wed, 25 Nov 2026 10:30:00 -0800"), parseDate(L"2026-11-25T18:30:00Z"));
    EXPECT_EQ(parseDate(L"2024-02-29T10:15:01.123+01:00"), parseDate(L"2024-02-29T09:15:01Z"));
    EXPECT_FALSE(parseDate(L"2025-02-29T10:00:00Z"));
    EXPECT_FALSE(parseDate(L"2026-11-25T10:00:00"));
    EXPECT_FALSE(parseDate(L"2026-11-25T10:00:00Ztrailing"));
    EXPECT_FALSE(parseDate(L"2026-11-25T25:00:00Z"));
}
TEST_F(XmlTest, OpmlPreservesNamespacedSettings)
{
    std::vector<Outline> list{{L"Example", L"https://example.com/feed", R"({"http":false,"age":7})"}};
    const auto xml = exportOpml(list);
    const auto parsed = parseOpml(bytes(xml));
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].settings, list[0].settings);
}
TEST_F(XmlTest, EquivalentAdultRatings)
{
    const auto f =
        parseFeed(bytes(R"(<rss version="2.0" xmlns:m="http://search.yahoo.com/mrss/"><channel><item>
      <m:rating scheme="urn:mpaa">R</m:rating><m:content medium="image" url="https://example.com/a.jpg"/>
      </item></channel></rss>)"),
                  L"https://example.com/feed");
    ASSERT_EQ(f.images.size(), 1u);
    EXPECT_EQ(f.images[0].rating, Rating::adult);
}
TEST_F(XmlTest, ConflictingRatingsPreferMatureAtSameScope)
{
    const auto f =
        parseFeed(bytes(R"(<rss version="2.0" xmlns:m="http://search.yahoo.com/mrss/"><channel><item>
      <m:rating>nonadult</m:rating><m:rating>adult</m:rating><m:content medium="image" url="https://example.com/a.jpg"/>
      </item></channel></rss>)"),
                  L"https://example.com/feed");
    ASSERT_EQ(f.images.size(), 1u);
    EXPECT_EQ(f.images[0].rating, Rating::adult);
}
TEST_F(XmlTest, AuthorAndMediaCreditInheritance)
{
    const auto feed = parseFeed(bytes(R"(<rss version="2.0" xmlns:m="http://search.yahoo.com/mrss/"
      xmlns:dc="http://purl.org/dc/elements/1.1/"><channel><m:credit>Publisher</m:credit>
      <item><dc:creator>Item author</dc:creator><m:content medium="image" url="https://example.com/a.jpg"/>
      <m:group><m:credit>Group photographer</m:credit>
      <m:content medium="image" url="https://example.com/b.jpg"><m:credit>Image credit</m:credit></m:content>
      <m:content medium="image" url="https://example.com/c.jpg"/></m:group></item>
      <item><enclosure type="image/jpeg" url="https://example.com/d.jpg"/></item></channel></rss>)"),
                                L"https://example.com/feed");
    ASSERT_EQ(feed.images.size(), 4u);
    EXPECT_EQ(feed.images[0].author, L"Item author");
    EXPECT_EQ(feed.images[1].author, L"Image credit");
    EXPECT_EQ(feed.images[2].author, L"Group photographer");
    EXPECT_EQ(feed.images[3].author, L"Publisher");
}
TEST(JsonFeed, Version11ImagesAttachmentsAndMetadata)
{
    const auto feed = parseFeed(bytes(R"({"version":"https://jsonfeed.org/version/1.1","title":"Photos",
      "authors":[{"name":"Publisher"}],"items":[{"id":"a","title":"Mountains",
      "content_text":"Winter snow","date_published":"2026-11-26T10:00:00+01:00",
      "authors":[{"name":"Photographer"}],"tags":["winter","mountain"],"image":"/a.jpg",
      "banner_image":"/decorative.jpg","attachments":[
        {"url":"/a.jpg","mime_type":"image/jpeg"},
        {"url":"/b.webp","mime_type":"image/webp"},
        {"url":"/video.mp4","mime_type":"video/mp4"},
        {"url":"/document.pdf","mime_type":"application/pdf"}]}]})"),
                                L"https://example.com/feed");
    EXPECT_EQ(feed.title, L"Photos");
    ASSERT_EQ(feed.images.size(), 2u);
    EXPECT_EQ(feed.images[0].url, L"https://example.com/a.jpg");
    EXPECT_EQ(feed.images[1].url, L"https://example.com/b.webp");
    EXPECT_EQ(feed.images[0].title, L"Mountains");
    EXPECT_EQ(feed.images[0].description, L"Winter snow");
    EXPECT_EQ(feed.images[0].author, L"Photographer");
    EXPECT_EQ(feed.images[0].published, parseDate(L"2026-11-26T09:00:00Z"));
    EXPECT_NE(feed.images[0].keywords.find(L"winter"), std::wstring::npos);
    EXPECT_EQ(feed.images[0].rating, Rating::unrated);
}
TEST(JsonFeed, VersionOneAndAuthorInheritance)
{
    const auto feed = parseFeed(bytes(R"({"version":"https://jsonfeed.org/version/1","title":"Legacy",
      "author":{"name":"Publisher"},"items":[{"id":12,"image":"https://example.com/a.jpg"},
      {"id":"b","author":{"name":"Artist"},"image":"https://example.com/b.jpg"}]})"),
                                L"https://example.com/feed");
    ASSERT_EQ(feed.images.size(), 2u);
    EXPECT_EQ(feed.images[0].author, L"Publisher");
    EXPECT_EQ(feed.images[1].author, L"Artist");
    EXPECT_FALSE(feed.images[0].published);
}
TEST(JsonFeed, MissingIdsAndHtmlAreNotImages)
{
    const auto feed = parseFeed(bytes(R"({"version":"https://jsonfeed.org/version/1.1","title":"Test",
      "items":[{"image":"https://example.com/no-id.jpg"},
      {"id":12,"image":"https://example.com/numeric-id.jpg"},
      {"id":"html","content_html":"<img src='https://example.com/scraped.jpg'>"},
      {"id":"bad-url","image":"file:///C:/unsafe.jpg"},
      {"id":"credentials","image":"https://user:pass@example.com/private.jpg"}]})"),
                                L"https://example.com/feed");
    EXPECT_TRUE(feed.images.empty());
}
TEST(JsonFeed, PeapixSpotlightIsUndatedAndPrefersFullImage)
{
    const auto feed = parseFeed(bytes(R"([{"title":"Bridge","copyright":"Photo credit",
      "fullUrl":"https://img.peapix.com/full_1920.jpg","imageUrl":"https://img.peapix.com/full.jpg",
      "thumbUrl":"https://img.peapix.com/full_640.jpg","pageUrl":"https://peapix.com/spotlight/1"}])"),
                                L"https://peapix.com/spotlight/feed");
    EXPECT_EQ(feed.title, L"Peapix: Windows Spotlight");
    ASSERT_EQ(feed.images.size(), 1u);
    EXPECT_EQ(feed.images[0].url, L"https://img.peapix.com/full_1920.jpg");
    EXPECT_EQ(feed.images[0].author, L"Photo credit");
    EXPECT_FALSE(feed.images[0].published);
    EXPECT_EQ(feed.images[0].rating, Rating::unrated);
}
TEST(JsonFeed, PeapixBingCalendarDateAndFallback)
{
    const auto feed = parseFeed(bytes(R"([{"title":"Landscape","date":"2026-11-26",
      "imageUrl":"https://img.peapix.com/landscape.jpg"},
      {"title":"Bad date","date":"2025-02-29","fullUrl":"https://img.peapix.com/other.jpg"}])"),
                                L"https://peapix.com/bing/feed?country=gb");
    ASSERT_EQ(feed.images.size(), 2u);
    EXPECT_EQ(feed.images[0].published, parseDate(L"2026-11-26T00:00:00Z"));
    EXPECT_FALSE(feed.images[1].published);
}
TEST(JsonFeed, RejectsUnrecognisedShapesVersionsAndDuplicateKeys)
{
    EXPECT_THROW(parseFeed(bytes("[]"), L"https://example.com/feed"), ParseError);
    EXPECT_THROW(parseFeed(bytes("[]"), L"https://peapix.com.evil.test/spotlight/feed"), ParseError);
    EXPECT_THROW(parseFeed(bytes("[]"), L"https://peapix.com/other/feed"), ParseError);
    EXPECT_THROW(
        parseFeed(bytes(R"({"version":"https://jsonfeed.org/version/2","title":"Future","items":[]})"),
                  L"https://example.com/feed"),
        ParseError);
    EXPECT_THROW(
        parseFeed(
            bytes(R"({"version":"https://jsonfeed.org/version/1.1","title":"A","title":"B","items":[]})"),
            L"https://example.com/feed"),
        ParseError);
    EXPECT_THROW(parseFeed(bytes(R"({"version":"https://jsonfeed.org/version/1.1","items":[]})"),
                           L"https://example.com/feed"),
                 ParseError);
    EXPECT_THROW(parseFeed(bytes("{bad json"), L"https://example.com/feed"), ParseError);
}
TEST(JsonFeed, BoundedDepthAndBomDetection)
{
    const std::string deep = std::string(70, '[') + "0" + std::string(70, ']');
    EXPECT_THROW(parseFeed(bytes(deep), L"https://peapix.com/spotlight/feed"), ParseError);
    const std::string bom = "\xEF\xBB\xBF \r\n";
    const auto feed =
        parseFeed(bytes(bom + R"({"version":"https://jsonfeed.org/version/1.1","title":"Empty","items":[]})"),
                  L"https://example.com/feed");
    EXPECT_EQ(feed.title, L"Empty");
}
