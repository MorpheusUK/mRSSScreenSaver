// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include <winsock2.h>
#include <ws2tcpip.h>
#include "mrss/engine.hpp"
#include "mrss/graphics.hpp"
#include <cstring>
#include <gtest/gtest.h>
#include <mutex>
#include <objbase.h>
#include <thread>
#include <wincodec.h>
#include <wrl/client.h>

using namespace mrss;
namespace
{
std::vector<std::byte> bitmap(unsigned seed = 0)
{
    constexpr unsigned w = 640, h = 360, row = w * 3, size = 54 + row * h;
    std::vector<std::byte> b(size);
    auto put = [&](unsigned i, unsigned v, unsigned n) {
        for (unsigned k = 0; k < n; ++k)
            b[i + k] = static_cast<std::byte>(v >> (8 * k) & 255);
    };
    b[0] = std::byte{'B'};
    b[1] = std::byte{'M'};
    put(2, size, 4);
    put(10, 54, 4);
    put(14, 40, 4);
    put(18, w, 4);
    put(22, h, 4);
    put(26, 1, 2);
    put(28, 24, 2);
    put(34, row * h, 4);
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x)
        {
            const auto offset = 54 + y * row + x * 3;
            b[offset] = static_cast<std::byte>((x + seed * 77) % 256);
            b[offset + 1] = static_cast<std::byte>((y + seed * 100) % 256);
            b[offset + 2] = static_cast<std::byte>((x / 3 + y / 2 + seed * 150) % 256);
        }
    return b;
}
class Server
{
    SOCKET socket_ = INVALID_SOCKET;
    std::jthread worker_;
    std::mutex mutex_;
    std::vector<std::string> requests_;

  public:
    unsigned port = 0;
    Server()
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data))
            throw std::runtime_error("WSAStartup failed");
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) || listen(socket_, 4))
            throw std::runtime_error("Local test server failed");
        int size = sizeof(a);
        getsockname(socket_, reinterpret_cast<sockaddr *>(&a), &size);
        port = ntohs(a.sin_port);
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                fd_set f;
                FD_ZERO(&f);
                FD_SET(socket_, &f);
                timeval timeout{0, 100000};
                if (select(0, &f, nullptr, nullptr, &timeout) <= 0)
                    continue;
                SOCKET client = accept(socket_, nullptr, nullptr);
                if (client == INVALID_SOCKET)
                    continue;
                DWORD ms = 2000;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&ms), sizeof(ms));
                std::string request;
                char buffer[4096]{};
                while (request.find("\r\n\r\n") == request.npos && request.size() < 16384)
                {
                    const auto n = recv(client, buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        break;
                    request.append(buffer, static_cast<std::size_t>(n));
                }
                {
                    std::lock_guard lock(mutex_);
                    requests_.push_back(request);
                }
                std::string status = "200 OK", headers, body = "hello";
                if (request.starts_with("GET /redirect "))
                {
                    status = "302 Found";
                    headers = "Location: /image.bmp\r\n";
                    body.clear();
                }
                else if (request.starts_with("GET /auth "))
                {
                    status = "401 Unauthorized";
                    headers = "WWW-Authenticate: NTLM\r\n";
                }
                else if (request.starts_with("GET /conditional ") &&
                         request.find("If-None-Match: \"test\"") != request.npos)
                {
                    status = "304 Not Modified";
                    body.clear();
                }
                else if (request.starts_with("GET /feed "))
                {
                    if (request.find("If-None-Match: \"test\"") != request.npos)
                    {
                        status = "304 Not Modified";
                        body.clear();
                    }
                    else
                        body = "<rss version=\"2.0\" xmlns:media=\"http://search.yahoo.com/mrss/\">"
                               "<channel><title>Refreshed feed</title><item>"
                               "<media:content url=\"https://example.com/photo.jpg\" type=\"image/jpeg\"/>"
                               "</item></channel></rss>";
                }
                else if (request.starts_with("GET /json-feed "))
                {
                    headers = "Content-Type: application/feed+json\r\n";
                    body = "{\"version\":\"https://jsonfeed.org/version/1.1\",\"title\":\"JSON Photos\","
                           "\"items\":[{\"id\":\"photo\",\"title\":\"Native fixture\",\"image\":\"" +
                           utf8(url(L"/image.bmp")) + "\"}]}";
                }
                else if (request.starts_with("GET /image.bmp "))
                {
                    const auto b = bitmap();
                    body.assign(reinterpret_cast<const char *>(b.data()), b.size());
                    headers = "Content-Type: image/bmp\r\n";
                }
                headers += "ETag: \"test\"\r\n";
                std::string response = "HTTP/1.1 " + status + "\r\n" + headers +
                                       "Content-Length: " + std::to_string(body.size()) +
                                       "\r\nConnection: close\r\n\r\n" + body;
                std::size_t sent = 0;
                while (sent < response.size())
                {
                    const int n =
                        send(client, response.data() + sent,
                             static_cast<int>((std::min)(response.size() - sent, std::size_t{32768})), 0);
                    if (n <= 0)
                        break;
                    sent += static_cast<std::size_t>(n);
                }
                closesocket(client);
            }
        });
    }
    ~Server()
    {
        worker_.request_stop();
        worker_.join();
        closesocket(socket_);
        WSACleanup();
    }
    std::wstring url(const wchar_t *path)
    {
        return L"http://127.0.0.1:" + std::to_wstring(port) + path;
    }
    std::vector<std::string> requests()
    {
        std::lock_guard lock(mutex_);
        return requests_;
    }
};
class Integration : public testing::Test
{
    HRESULT com_ = E_FAIL;

  protected:
    std::filesystem::path root;
    void SetUp() override
    {
        com_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ASSERT_TRUE(SUCCEEDED(com_));
        root = std::filesystem::current_path() / L"test-data" /
               wide(testing::UnitTest::GetInstance()->current_test_info()->name());
        setDataDirectory(root);
        std::filesystem::create_directories(root);
    }
    void TearDown() override
    {
        if (SUCCEEDED(com_))
            CoUninitialize();
    }
};
} // namespace
TEST(Settings, RoundTripOverridesAndGlobalMature)
{
    Settings s;
    FeedConfig f;
    f.url = L"https://example.com/feed?token=abc";
    f.title = L"Unicode \u00e9 & photos";
    f.policy.allowHttp = true;
    f.policy.age.unit = AgeUnit::months;
    f.policy.overrideAge = true;
    f.policy.displaySeconds = 27;
    f.policy.excludedUrlText = {L"blur_"};
    f.filter.mode = FilterMode::boolean;
    f.filter.expression = L"portrait AND NOT sketch";
    s.feeds.push_back(f);
    s.defaults.allowMature = true;
    const auto decoded = decodeSettings(encodeSettings(s));
    ASSERT_EQ(decoded.feeds.size(), 1u);
    EXPECT_EQ(decoded.feeds[0].url, f.url);
    EXPECT_EQ(decoded.feeds[0].title, f.title);
    EXPECT_EQ(decoded.feeds[0].policy.displaySeconds, 27u);
    EXPECT_TRUE(decoded.defaults.allowMature);
    EXPECT_EQ(decoded.feeds[0].policy.age.unit, AgeUnit::months);
}
TEST(Settings, RejectCorruptOrFutureVersions)
{
    auto j = encodeSettings(Settings{});
    j["version"] = 100;
    EXPECT_THROW(decodeSettings(j), std::exception);
    j["version"] = 1;
    j["downloads"] = 100;
    EXPECT_THROW(decodeSettings(j), std::exception);
}
TEST(Settings, RejectExcessiveJsonNesting)
{
    EXPECT_THROW(parseJson(std::string(1000, '[') + "0" + std::string(1000, ']')), std::exception);
}
TEST(Filters, BooleanPrecedencePhrasesAndCase)
{
    Filter f;
    f.mode = FilterMode::boolean;
    f.expression = L"\"blue sky\" AND NOT sketch OR mountain";
    EXPECT_TRUE(matchesFilter(f, L"BLUE SKY photo"));
    EXPECT_FALSE(matchesFilter(f, L"blue sky sketch"));
    EXPECT_TRUE(matchesFilter(f, L"mountain sketch"));
    f.expression = L"a b";
    EXPECT_THROW(validateFilter(f), std::exception);
    f.expression = L"(a OR b";
    EXPECT_THROW(validateFilter(f), std::exception);
}
TEST(Settings, GlobalAgeAndExplicitOverridesRoundTrip)
{
    Settings s;
    s.defaults.age.value = 3;
    s.defaults.age.unit = AgeUnit::months;
    s.defaults.age.includeUndated = true;
    FeedConfig inherited;
    inherited.url = L"https://example.com/inherited";
    FeedConfig local;
    local.url = L"https://example.com/local";
    local.policy.overrideAge = true;
    local.policy.age.entireFeed = true;
    s.feeds = {inherited, local};
    const auto restored = decodeSettings(encodeSettings(s));
    ASSERT_EQ(restored.feeds.size(), 2u);
    EXPECT_FALSE(restored.feeds[0].policy.overrideAge);
    EXPECT_TRUE(restored.feeds[1].policy.overrideAge);
    EXPECT_EQ(effectiveAgeLimit(restored.defaults, restored.feeds[0].policy).value, 3u);
    EXPECT_EQ(effectiveAgeLimit(restored.defaults, restored.feeds[0].policy).unit, AgeUnit::months);
    EXPECT_TRUE(effectiveAgeLimit(restored.defaults, restored.feeds[0].policy).includeUndated);
    EXPECT_TRUE(effectiveAgeLimit(restored.defaults, restored.feeds[1].policy).entireFeed);
}
TEST(Settings, OpmlExtensionImportsSingleFeed)
{
    Outline outline{
        L"Test", L"https://example.com/feed",
        R"({"age":7,"duration":0,"enabled":true,"entire":false,"excludedUrls":["blur_"],"filter":{"case":false,"exclude":"","expression":"","fields":3,"mode":0},"http":false,"lan":false,"title":"","undated":false,"unit":2,"url":"https://example.com/feed"})"};
    const auto imported = decodeOutline(outline);
    EXPECT_EQ(imported.url, outline.url);
    EXPECT_FALSE(imported.policy.overrideAge);
    EXPECT_FALSE(imported.policy.displaySeconds.has_value());
}
TEST(Settings, VersionOneDefaultsToGlobalAgeUnlessExplicitlyOverridden)
{
    Settings s;
    FeedConfig f;
    f.url = L"https://example.com/feed";
    f.policy.age.value = 2;
    f.policy.age.unit = AgeUnit::hours;
    s.feeds = {f};
    auto old = encodeSettings(s);
    old["version"] = 1;
    old.erase("maximumAge");
    old["feeds"][0].erase("ageOverride");
    const auto restored = decodeSettings(old);
    ASSERT_EQ(restored.feeds.size(), 1u);
    EXPECT_FALSE(restored.feeds[0].policy.overrideAge);
    EXPECT_EQ(effectiveAgeLimit(restored.defaults, restored.feeds[0].policy).value, 7u);
    EXPECT_EQ(effectiveAgeLimit(restored.defaults, restored.feeds[0].policy).unit, AgeUnit::days);
    EXPECT_EQ(restored.feeds[0].policy.age.value, 2u);
    EXPECT_EQ(restored.feeds[0].policy.age.unit, AgeUnit::hours);
    EXPECT_EQ(encodeSettings(restored).at("version"), 2);
}
TEST(Age, EligibilityUsesGlobalLimitUnlessLocalOverrideIsEnabled)
{
    Defaults defaults;
    FeedConfig f;
    f.policy.age.entireFeed = true;
    MediaImage image;
    image.url = L"https://example.com/photo.jpg";
    image.rating = Rating::unrestricted;
    image.published =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - std::chrono::days(10);
    EXPECT_EQ(eligibility(image, f, defaults), L"Outside age limit or undated");
    defaults.age.value = 14;
    EXPECT_TRUE(eligibility(image, f, defaults).empty());
    defaults.age.value = 7;
    f.policy.overrideAge = true;
    EXPECT_TRUE(eligibility(image, f, defaults).empty());
    f.policy.age.entireFeed = false;
    f.policy.age.value = 2;
    EXPECT_FALSE(eligibility(image, f, defaults).empty());
    f.policy.overrideAge = false;
    defaults.age.entireFeed = true;
    image.published.reset();
    EXPECT_TRUE(eligibility(image, f, defaults).empty());
}
TEST(Filters, RegexIncludeExcludeAndBudget)
{
    Filter f;
    f.mode = FilterMode::regex;
    f.expression = L"photo(graph)?";
    f.exclude = L"sketch";
    EXPECT_TRUE(matchesFilter(f, L"PHOTOGRAPH"));
    EXPECT_FALSE(matchesFilter(f, L"photo sketch"));
    f.expression = L"[";
    EXPECT_THROW(validateFilter(f), std::exception);
    f.expression = L"(a+)+$";
    f.exclude.clear();
    EXPECT_THROW(matchesFilter(f, std::wstring(10000, L'a') + L"!"), std::exception);
}
TEST(Network, SpecialUseAddressesBlocked)
{
    for (auto s : {L"127.0.0.1", L"10.0.0.1", L"192.168.1.1", L"169.254.169.254", L"100.64.0.1", L"::1",
                   L"fc00::1", L"fe80::1", L"::ffff:127.0.0.1", L"2001:db8::1"})
        EXPECT_FALSE(publicAddress(s)) << utf8(s);
    EXPECT_TRUE(publicAddress(L"8.8.8.8"));
    EXPECT_TRUE(publicAddress(L"2606:4700:4700::1111"));
}
TEST_F(Integration, HttpAndLanRequireSeparateAllowances)
{
    Server server;
    FeedPolicy p;
    EXPECT_THROW(download(server.url(L"/"), p, {}, 100), std::exception);
    p.allowHttp = true;
    EXPECT_THROW(download(server.url(L"/"), p, {}, 100), std::exception);
    p.allowPrivateNetwork = true;
    EXPECT_EQ(download(server.url(L"/"), p, {}, 100).status, 200u);
}
TEST_F(Integration, OpmlWithManyLegacyExtensionsImportsAndDeduplicates)
{
    Settings source;
    FeedConfig sample;
    sample.url = L"https://example.com/feed";
    sample.policy.excludedUrlText = {L"blur_"};
    source.feeds = {sample};
    auto extension = encodeSettings(source).at("feeds").at(0);
    extension.erase("ageOverride");
    std::vector<Outline> list;
    for (unsigned i = 0; i < 35; ++i)
    {
        const auto url = L"https://example.com/feed?tag=" + std::to_wstring(i % 34) + L"&format=rss2";
        list.push_back({L"Example", url, extension.dump()});
    }
    const auto xml = exportOpml(list);
    const auto parsed = parseOpml({reinterpret_cast<const std::byte *>(xml.data()), xml.size()});
    ASSERT_EQ(parsed.size(), 34u);
    for (const auto &outline : parsed)
    {
        const auto feed = decodeOutline(outline);
        EXPECT_EQ(feed.url, outline.url);
        EXPECT_EQ(feed.title, L"Example");
        EXPECT_FALSE(feed.policy.overrideAge);
        EXPECT_FALSE(feed.policy.displaySeconds);
        EXPECT_EQ(feed.policy.excludedUrlText, sample.policy.excludedUrlText);
    }
}
TEST(Settings, ExplicitImportedOverridesAreRetained)
{
    Outline outline{L"Explicit", L"https://example.com/feed",
                    R"({"ageOverride":true,"age":2,"unit":1,"duration":23})"};
    const auto feed = decodeOutline(outline);
    EXPECT_TRUE(feed.policy.overrideAge);
    EXPECT_EQ(feed.policy.age.value, 2u);
    EXPECT_EQ(feed.policy.displaySeconds, 23u);
}
TEST(Settings, NewTransitionsPreserveExistingSelections)
{
    EXPECT_EQ(static_cast<int>(Transition::random), 4);
    EXPECT_EQ(static_cast<int>(Transition::none), 5);
    Settings settings;
    for (const auto effect : {Transition::cube, Transition::collage, Transition::panZoom})
    {
        settings.defaults.transition = effect;
        settings.randomEffects = 127;
        const auto restored = decodeSettings(encodeSettings(settings));
        EXPECT_EQ(restored.defaults.transition, effect);
        EXPECT_EQ(restored.randomEffects, 127u);
    }
    auto legacy = encodeSettings(Settings{});
    legacy["transition"] = 4;
    legacy["randomEffects"] = 15;
    const auto restored = decodeSettings(legacy);
    EXPECT_EQ(restored.defaults.transition, Transition::random);
    EXPECT_EQ(restored.randomEffects, 15u);
}
TEST(Settings, MetadataInheritanceDurationAndOpmlRoundTrip)
{
    Settings settings;
    FeedConfig feed;
    feed.url = L"https://example.com/feed";
    EXPECT_FALSE(feed.metadata.has_value());
    EXPECT_FALSE(settings.metadata.title);
    EXPECT_FALSE(settings.metadata.author);
    EXPECT_FALSE(settings.metadata.bold);
    EXPECT_FALSE(settings.metadata.italic);
    settings.metadata.title = true;
    settings.metadata.bold = true;
    settings.metadata.seconds = 30;
    EXPECT_EQ(effectiveMetadataSeconds(settings, feed), 10u);
    feed.policy.displaySeconds = 3;
    EXPECT_EQ(effectiveMetadataSeconds(settings, feed), 3u);
    feed.metadata = MetadataOptions{};
    feed.metadata->author = true;
    feed.metadata->font = L"Arial";
    feed.metadata->points = 32;
    feed.metadata->top = true;
    feed.metadata->bold = true;
    feed.metadata->italic = true;
    feed.metadata->seconds = 2;
    EXPECT_FALSE(effectiveMetadata(settings, feed).title);
    EXPECT_TRUE(effectiveMetadata(settings, feed).author);
    EXPECT_EQ(effectiveMetadataSeconds(settings, feed), 2u);
    settings.feeds = {feed};
    const auto encoded = encodeSettings(settings);
    const auto restored = decodeSettings(encoded);
    EXPECT_TRUE(restored.metadata.title);
    EXPECT_TRUE(restored.metadata.bold);
    EXPECT_FALSE(restored.metadata.italic);
    ASSERT_TRUE(restored.feeds[0].metadata.has_value());
    EXPECT_EQ(restored.feeds[0].metadata->font, L"Arial");
    EXPECT_EQ(restored.feeds[0].metadata->points, 32u);
    EXPECT_TRUE(restored.feeds[0].metadata->top);
    EXPECT_TRUE(restored.feeds[0].metadata->bold);
    EXPECT_TRUE(restored.feeds[0].metadata->italic);
    const auto imported = decodeOutline({L"Example", feed.url, encoded.at("feeds").at(0).dump()});
    ASSERT_TRUE(imported.metadata.has_value());
    EXPECT_TRUE(imported.metadata->author);
    EXPECT_EQ(imported.metadata->seconds, 2u);
    EXPECT_TRUE(imported.metadata->bold);
    EXPECT_TRUE(imported.metadata->italic);
    auto oldStyles = encoded;
    oldStyles["metadataOverlay"].erase("bold");
    oldStyles["metadataOverlay"].erase("italic");
    oldStyles["feeds"][0]["metadataOverlay"].erase("bold");
    oldStyles["feeds"][0]["metadataOverlay"].erase("italic");
    const auto plain = decodeSettings(oldStyles);
    EXPECT_FALSE(plain.metadata.bold);
    EXPECT_FALSE(plain.metadata.italic);
    EXPECT_FALSE(plain.feeds[0].metadata->bold);
    EXPECT_FALSE(plain.feeds[0].metadata->italic);
    auto legacy = encoded;
    legacy.erase("metadataOverlay");
    legacy["feeds"][0].erase("metadataOverlay");
    const auto old = decodeSettings(legacy);
    EXPECT_FALSE(old.metadata.title);
    EXPECT_FALSE(old.feeds[0].metadata.has_value());
}
TEST(Settings, MetadataTextIsBoundedAndDoesNotInventMissingFields)
{
    MediaImage image;
    MetadataOptions options;
    options.title = options.author = true;
    image.title = L"  Sunset\r\nphoto  ";
    EXPECT_EQ(metadataText(image, options), L"Sunset  photo");
    image.author = L"Photographer";
    EXPECT_EQ(metadataText(image, options), L"Sunset  photo\nPhotographer");
    options.title = false;
    EXPECT_EQ(metadataText(image, options), L"Photographer");
    options.author = false;
    EXPECT_TRUE(metadataText(image, options).empty());
    options.title = true;
    image.title.assign(5000, L'a');
    EXPECT_EQ(metadataText(image, options).size(), 1024u);
    auto invalid = encodeSettings(Settings{});
    invalid["metadataOverlay"]["points"] = 0;
    EXPECT_THROW(decodeSettings(invalid), std::exception);
    invalid["metadataOverlay"]["points"] = 24;
    invalid["metadataOverlay"]["seconds"] = 0;
    EXPECT_THROW(decodeSettings(invalid), std::exception);
}
TEST(Settings, FeedArrayShapeAndActualCountHaveDistinctErrors)
{
    auto settings = encodeSettings(Settings{});
    settings["feeds"] = nlohmann::json::object();
    try
    {
        (void)decodeSettings(settings);
        FAIL() << "Expected a shape error";
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_STREQ(e.what(), "Settings feed list must be a JSON array");
    }
    FeedConfig feed;
    feed.url = L"https://example.com/feed";
    Settings one;
    one.feeds = {feed};
    const auto record = encodeSettings(one).at("feeds").at(0);
    settings["feeds"] = nlohmann::json::array();
    for (unsigned i = 0; i < 1000; ++i)
        settings["feeds"].push_back(record);
    EXPECT_EQ(decodeSettings(settings).feeds.size(), 1000u);
    settings["feeds"].push_back(record);
    try
    {
        (void)decodeSettings(settings);
        FAIL() << "Expected a count error";
    }
    catch (const std::runtime_error &e)
    {
        EXPECT_STREQ(e.what(), "Feed limit exceeded (maximum 1000 feeds)");
    }
}
TEST_F(Integration, ForcedRefreshReportsEachFeedAndBypassesFreshCache)
{
    Server server;
    Settings settings;
    FeedConfig feed;
    feed.url = server.url(L"/feed");
    feed.policy.allowHttp = true;
    feed.policy.allowPrivateNetwork = true;
    settings.feeds = {feed};
    ASSERT_EQ(buildCatalog(settings, true, {}).status.at(0).title, L"Refreshed feed");
    const auto initialRequests = server.requests().size();
    (void)buildCatalog(settings, false, {});
    EXPECT_EQ(server.requests().size(), initialRequests);
    auto bad = feed;
    bad.url = server.url(L"/auth");
    settings.feeds.push_back(bad);
    std::vector<FeedStatus> updates;
    const auto result = buildCatalog(settings, true, {}, [&](std::size_t index, const FeedStatus &status) {
        EXPECT_EQ(index, updates.size());
        updates.push_back(status);
    });
    ASSERT_EQ(updates.size(), 2u);
    EXPECT_EQ(updates[0].title, L"Refreshed feed");
    EXPECT_TRUE(updates[0].message.starts_with(L"Available"));
    EXPECT_NE(updates[1].message.find(L"Authentication required"), std::wstring::npos);
    EXPECT_EQ(result.status.size(), updates.size());
    EXPECT_EQ(server.requests().size(), initialRequests + 2);
}
TEST_F(Integration, FeedStatusSeparatesCountsAndPreservesCacheTimeOn304)
{
    Server server;
    Settings settings;
    FeedConfig feed;
    feed.url = server.url(L"/feed");
    feed.policy.allowHttp = true;
    feed.policy.allowPrivateNetwork = true;
    settings.feeds = {feed};
    (void)buildCatalog(settings, true, {});
    const auto metadataPath = root / L"cache" / (digest(feed.url) + ".feed.json");
    auto bytes = readFile(metadataPath, 1024 * 1024);
    auto metadata = nlohmann::json::parse(reinterpret_cast<const char *>(bytes.data()),
                                          reinterpret_cast<const char *>(bytes.data()) + bytes.size());
    metadata["cached"] = 1700000000;
    metadata["checked"] = 1700000000;
    writeText(metadataPath, metadata.dump());
    const auto catalog = buildCatalog(settings, true, {});
    ASSERT_EQ(catalog.status.size(), 1u);
    const auto &status = catalog.status.front();
    EXPECT_EQ(status.total, 1u);
    EXPECT_NE(status.message.find(L"last success "), std::wstring::npos);
    EXPECT_NE(status.message.find(L"cached 2023-"), std::wstring::npos);
    EXPECT_TRUE(status.message.ends_with(L" (local)"));
    EXPECT_EQ(status.message.find(L"eligible"), std::wstring::npos);
    bytes = readFile(metadataPath, 1024 * 1024);
    metadata = nlohmann::json::parse(reinterpret_cast<const char *>(bytes.data()),
                                     reinterpret_cast<const char *>(bytes.data()) + bytes.size());
    EXPECT_EQ(metadata.at("cached"), 1700000000);
    EXPECT_GT(metadata.at("checked").get<std::int64_t>(), 1700000000);
}
TEST_F(Integration, JsonFeedUsesSharedCacheDecoderAndEligibilityRules)
{
    Server server;
    Settings settings;
    FeedConfig feed;
    feed.url = server.url(L"/json-feed");
    feed.policy.allowHttp = true;
    feed.policy.allowPrivateNetwork = true;
    feed.policy.overrideAge = true;
    feed.policy.age.entireFeed = true;
    settings.feeds = {feed};
    const auto catalog = buildCatalog(settings, true, {});
    ASSERT_EQ(catalog.items.size(), 1u);
    EXPECT_EQ(catalog.status[0].title, L"JSON Photos");
    const auto path = imageFile(catalog.items[0], settings.defaults, {});
    EXPECT_NO_THROW(decodeImage(path));
    const auto requests = server.requests().size();
    EXPECT_EQ(buildCatalog(settings, false, {}, {}, true).items.size(), 1u);
    EXPECT_EQ(server.requests().size(), requests);
    settings.defaults.excludeUnrated = true;
    EXPECT_TRUE(buildCatalog(settings, false, {}, {}, true).items.empty());
    settings.defaults.excludeUnrated = false;
    settings.feeds[0].policy.overrideAge = false;
    EXPECT_TRUE(buildCatalog(settings, false, {}, {}, true).items.empty());
    settings.defaults.age.includeUndated = true;
    EXPECT_EQ(buildCatalog(settings, false, {}, {}, true).items.size(), 1u);
    settings.defaults.excludedUrlText = {L"image.bmp"};
    EXPECT_TRUE(buildCatalog(settings, false, {}, {}, true).items.empty());
}
TEST_F(Integration, RedirectsAndExclusions)
{
    Server server;
    FeedPolicy p;
    p.allowHttp = true;
    p.allowPrivateNetwork = true;
    const auto r = download(server.url(L"/redirect"), p, {}, 1024 * 1024);
    EXPECT_TRUE(r.finalUrl.ends_with(L"/image.bmp"));
    EXPECT_EQ(identifyImage(r.body), ImageFormat::bmp);
    const std::vector<std::wstring> exclusions{L"image.bmp"};
    EXPECT_THROW(download(server.url(L"/redirect"), p, exclusions, 1024 * 1024), std::exception);
}
TEST_F(Integration, NoAuthenticationRetriesOrCookies)
{
    Server server;
    FeedPolicy p;
    p.allowHttp = true;
    p.allowPrivateNetwork = true;
    EXPECT_THROW(download(server.url(L"/auth"), p, {}, 100), std::exception);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].find("Authorization:"), std::string::npos);
    EXPECT_EQ(requests[0].find("Cookie:"), std::string::npos);
}
TEST_F(Integration, ConditionalRequestsAndDownloadLimit)
{
    Server server;
    FeedPolicy p;
    p.allowHttp = true;
    p.allowPrivateNetwork = true;
    EXPECT_EQ(download(server.url(L"/conditional"), p, {}, 100, {}, L"\"test\"").status, 304u);
    EXPECT_THROW(download(server.url(L"/image.bmp"), p, {}, 100), std::exception);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(download(server.url(L"/"), p, {}, 100, stop.get_token()), std::exception);
}
TEST_F(Integration, DecodeActualImageAndRejectExecutable)
{
    auto path = root / L"photo.img";
    atomicWrite(path, bitmap());
    const auto pixels = decodeImage(path);
    EXPECT_EQ(pixels.width, 640u);
    EXPECT_EQ(pixels.height, 360u);
    EXPECT_EQ(pixels.rgba.size(), 640u * 360u * 4u);
    writeText(path, "MZnot an image");
    EXPECT_THROW(decodeImage(path), std::exception);
}
TEST_F(Integration, SettingsBackupRecovery)
{
    Settings s;
    saveSettings(s);
    s.defaults.displaySeconds = 20;
    saveSettings(s);
    writeText(root / L"settings.json", "damaged");
    std::wstring warning;
    const auto restored = loadSettings(warning);
    EXPECT_EQ(restored.defaults.displaySeconds, 10u);
    EXPECT_FALSE(warning.empty());
}
TEST_F(Integration, OversizeSettingsCannotReplaceValidFile)
{
    Settings initial;
    saveSettings(initial);
    Settings large;
    for (unsigned i = 0; i < 500; ++i)
    {
        FeedConfig f;
        f.url = L"https://example.com/" + std::to_wstring(i) + std::wstring(10000, L'a');
        large.feeds.push_back(std::move(f));
    }
    EXPECT_THROW(saveSettings(large), std::exception);
    std::wstring warning;
    EXPECT_TRUE(loadSettings(warning).feeds.empty());
}
TEST_F(Integration, SeedPreviewData)
{
    Settings s;
    FeedConfig feed;
    feed.url = L"https://example.com/fixture";
    feed.title = L"Local regression fixture";
    feed.policy.age.entireFeed = true;
    feed.policy.overrideAge = true;
    s.feeds.push_back(feed);
    saveSettings(s);
    auto cache = root / L"cache";
    std::filesystem::create_directories(cache);
    std::string xml = "<rss version=\"2.0\" xmlns:m=\"http://search.yahoo.com/mrss/\"><channel><title>Local "
                      "regression fixture</title><m:rating>nonadult</m:rating>";
    for (unsigned i = 0; i < 3; ++i)
    {
        const auto url = L"https://example.com/image" + std::to_wstring(i) + L".bmp";
        const auto key = digest(url);
        atomicWrite(cache / (key + ".img"), bitmap(i));
        writeText(cache / (key + ".image.json"),
                  nlohmann::json{{"url", utf8(url)}, {"final", utf8(url)}, {"public", true}}.dump());
        xml += "<item><title>Test image " + std::to_string(i + 1) +
               "</title><m:content type=\"image/bmp\" url=\"" + utf8(url) + "\"/></item>";
    }
    xml += "</channel></rss>";
    writeText(cache / (digest(feed.url) + ".feed"), xml);
    writeText(cache / (digest(feed.url) + ".feed.json"),
              nlohmann::json{{"checked", std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count()}}
                  .dump());
    const auto catalog = buildCatalog(s, false, {});
    ASSERT_EQ(catalog.items.size(), 3u);
    for (const auto &item : catalog.items)
        EXPECT_NO_THROW(decodeImage(imageFile(item, s.defaults, {}, true)));
    s.defaults.excludedUrlText = {L"image1"};
    EXPECT_EQ(buildCatalog(s, false, {}).items.size(), 2u);
}
TEST_F(Integration, RenderAllTransitions)
{
    auto path = root / L"render.bmp";
    atomicWrite(path, bitmap());
    const auto pixels = decodeImage(path);
    HWND window = CreateWindowExW(0, L"STATIC", L"mRSS rendering test", WS_OVERLAPPED, 0, 0, 640, 360,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    try
    {
        Renderer renderer(window);
        renderer.setImage(pixels);
        renderer.setImage(pixels);
        Settings settings;
        for (auto effect : {Transition::fade, Transition::slide, Transition::crossFade, Transition::flip,
                            Transition::cube, Transition::collage, Transition::panZoom, Transition::none})
        {
            for (const bool fill : {false, true})
            {
                settings.fill = fill;
                for (const float progress : {0.f, .01f, .25f, .5f, .75f, .99f, 1.f})
                    renderer.draw(progress, effect, settings, progress);
            }
        }
        auto hdr = pixels;
        hdr.hdr = true;
        for (std::size_t i = 0; i < hdr.rgba.size(); i += 4)
        {
            hdr.rgba[i] *= 8;
            hdr.rgba[i + 1] *= 8;
            hdr.rgba[i + 2] *= 8;
        }
        renderer.setImage(hdr);
        renderer.draw(1, Transition::none, settings);
        MetadataOptions metadata;
        metadata.title = metadata.author = true;
        for (const bool top : {false, true})
            for (const unsigned points : {8u, 24u, 96u})
            {
                metadata.top = top;
                metadata.points = points;
                renderer.draw(1, Transition::none, settings, 0, L"Sunset over the hills\nPhotographer",
                              &metadata);
                renderer.draw(1, Transition::panZoom, settings, .5f, std::wstring(1024, L'W'), &metadata);
            }
        metadata.font = L"Missing font family for fallback test";
        renderer.draw(1, Transition::none, settings, 0, L"Unicode \u00e9 \u03a9 \u4e2d", &metadata);
        metadata.font = L"Segoe UI";
        for (const bool bold : {false, true})
            for (const bool italic : {false, true})
            {
                metadata.bold = bold;
                metadata.italic = italic;
                renderer.draw(1, Transition::none, settings, 0, L"Caption style\nPhotographer", &metadata);
            }
    }
    catch (...)
    {
        DestroyWindow(window);
        throw;
    }
    DestroyWindow(window);
}
TEST_F(Integration, CachedImagesCannotBypassCurrentPolicy)
{
    Item item;
    item.feed.url = L"https://example.com/feed";
    item.feed.policy.age.entireFeed = true;
    item.feed.policy.overrideAge = true;
    item.image.url = L"https://example.com/image.bmp";
    item.image.rating = Rating::adult;
    Defaults defaults;
    EXPECT_THROW(imageFile(item, defaults, {}, true), std::exception);
    defaults.allowMature = true;
    defaults.excludedUrlText = {L"image.bmp"};
    EXPECT_THROW(imageFile(item, defaults, {}, true), std::exception);
}
TEST_F(Integration, SharedCacheReferencesAndRemoval)
{
    const auto directory = root / L"cache";
    std::filesystem::create_directories(directory);
    const std::wstring image = L"https://example.com/shared.bmp";
    const auto key = digest(image);
    atomicWrite(directory / (key + ".img"), bitmap());
    writeText(directory / (key + ".image.json"),
              nlohmann::json{{"url", utf8(image)}, {"final", utf8(image)}, {"public", true}}.dump());
    Settings settings;
    FeedConfig a;
    a.url = L"https://example.com/a";
    FeedConfig b;
    b.url = L"https://example.com/b";
    const std::string xml = "<rss version=\"2.0\"><channel><item><enclosure type=\"image/bmp\" "
                            "url=\"https://example.com/shared.bmp\"/></item></channel></rss>";
    writeText(directory / (digest(a.url) + ".feed"), xml);
    writeText(directory / (digest(b.url) + ".feed"), xml);
    settings.feeds = {b};
    pruneRemovedFeeds(settings);
    EXPECT_TRUE(std::filesystem::exists(directory / (key + ".img")));
    settings.feeds.clear();
    pruneRemovedFeeds(settings);
    EXPECT_FALSE(std::filesystem::exists(directory / (key + ".img")));
}
TEST_F(Integration, DecodeSupportedBuiltInFormats)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> wic;
    ASSERT_TRUE(SUCCEEDED(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))));
    auto b = bitmap();
    ComPtr<IWICBitmap> source;
    ASSERT_TRUE(
        SUCCEEDED(wic->CreateBitmapFromMemory(640, 360, GUID_WICPixelFormat24bppBGR, 640 * 3, 640 * 360 * 3,
                                              reinterpret_cast<BYTE *>(b.data() + 54), &source)));
    const GUID formats[] = {GUID_ContainerFormatJpeg, GUID_ContainerFormatPng, GUID_ContainerFormatGif,
                            GUID_ContainerFormatTiff};
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto path = root / (L"encoded" + std::to_wstring(i) + L".img");
        ComPtr<IWICStream> stream;
        ASSERT_TRUE(SUCCEEDED(wic->CreateStream(&stream)));
        ASSERT_TRUE(SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)));
        ComPtr<IWICBitmapEncoder> encoder;
        ASSERT_TRUE(SUCCEEDED(wic->CreateEncoder(formats[i], nullptr, &encoder)));
        ASSERT_TRUE(SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)));
        ComPtr<IWICBitmapFrameEncode> frame;
        ASSERT_TRUE(SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)));
        ASSERT_TRUE(SUCCEEDED(frame->Initialize(nullptr)));
        ASSERT_TRUE(SUCCEEDED(frame->SetSize(640, 360)));
        auto format = GUID_WICPixelFormat24bppBGR;
        ASSERT_TRUE(SUCCEEDED(frame->SetPixelFormat(&format)));
        ComPtr<IWICPalette> palette;
        ASSERT_TRUE(SUCCEEDED(wic->CreatePalette(&palette)));
        ASSERT_TRUE(SUCCEEDED(palette->InitializeFromBitmap(source.Get(), 256, FALSE)));
        if (format == GUID_WICPixelFormat8bppIndexed)
            ASSERT_TRUE(SUCCEEDED(frame->SetPalette(palette.Get())));
        ComPtr<IWICFormatConverter> converter;
        ASSERT_TRUE(SUCCEEDED(wic->CreateFormatConverter(&converter)));
        ASSERT_TRUE(SUCCEEDED(converter->Initialize(source.Get(), format, WICBitmapDitherTypeNone,
                                                    palette.Get(), 0, WICBitmapPaletteTypeCustom)));
        ASSERT_TRUE(SUCCEEDED(frame->WriteSource(converter.Get(), nullptr)));
        ASSERT_TRUE(SUCCEEDED(frame->Commit()));
        ASSERT_TRUE(SUCCEEDED(encoder->Commit()));
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        const auto decoded = decodeImage(path);
        EXPECT_EQ(decoded.width, 640u);
        EXPECT_EQ(decoded.height, 360u);
    }
}
