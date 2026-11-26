// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/settings.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <shlobj.h>
#include <sstream>

namespace mrss
{
namespace
{
struct Lock
{
    HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\mRSSScreenSaver.Data.v1");
    Lock()
    {
        if (!h)
            throw std::runtime_error("Cannot open data lock");
        const auto r = WaitForSingleObject(h, 10000);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED)
        {
            CloseHandle(h);
            h = nullptr;
            throw std::runtime_error("Settings are busy");
        }
    }
    ~Lock()
    {
        if (h)
        {
            ReleaseMutex(h);
            CloseHandle(h);
        }
    }
};
using json = nlohmann::json;
std::filesystem::path dataRoot;
json strings(const std::vector<std::wstring> &list)
{
    json a = json::array();
    for (const auto &s : list)
        a.push_back(utf8(s));
    return a;
}
std::vector<std::wstring> strings(const json &a)
{
    if (!a.is_array() || a.size() > 256)
        throw std::runtime_error("Invalid exclusions");
    std::vector<std::wstring> r;
    for (const auto &s : a)
    {
        auto v = wide(s.get<std::string>());
        if (v.size() > 1024)
            throw std::runtime_error("Exclusion is too long");
        if (!v.empty())
            r.push_back(v);
    }
    return r;
}
unsigned bounded(const json &j, const char *key, unsigned fallback, unsigned min, unsigned max)
{
    const auto v = j.value(key, fallback);
    if (v < min || v > max)
        throw std::runtime_error("Setting outside allowed range");
    return v;
}
json encodeMetadata(const MetadataOptions &m)
{
    return {{"title", m.title},   {"author", m.author},   {"top", m.top},       {"bold", m.bold},
            {"italic", m.italic}, {"seconds", m.seconds}, {"points", m.points}, {"font", utf8(m.font)}};
}
MetadataOptions decodeMetadata(const json &j)
{
    if (!j.is_object())
        throw std::runtime_error("Invalid metadata overlay settings");
    MetadataOptions m;
    m.title = j.value("title", false);
    m.author = j.value("author", false);
    m.top = j.value("top", false);
    m.bold = j.value("bold", false);
    m.italic = j.value("italic", false);
    m.seconds = bounded(j, "seconds", 5, 1, 86400);
    m.points = bounded(j, "points", 24, 8, 96);
    m.font = wide(j.value("font", std::string("Segoe UI")));
    if (m.font.empty() || m.font.size() > 128 ||
        std::any_of(m.font.begin(), m.font.end(), [](wchar_t c) { return c < 32; }))
        throw std::runtime_error("Invalid metadata font family");
    return m;
}
} // namespace
const MetadataOptions &effectiveMetadata(const Settings &settings, const FeedConfig &feed)
{
    return feed.metadata ? *feed.metadata : settings.metadata;
}
unsigned effectiveMetadataSeconds(const Settings &settings, const FeedConfig &feed)
{
    return (std::min)(effectiveMetadata(settings, feed).seconds,
                      effectiveDisplaySeconds(settings.defaults, feed.policy));
}
std::wstring metadataText(const MediaImage &image, const MetadataOptions &options)
{
    auto clean = [](std::wstring_view input) {
        std::wstring text;
        for (auto c : input.substr(0, 1024))
            text.push_back(c < 32 || c == 127 ? L' ' : c);
        const auto first = text.find_first_not_of(L' ');
        return first == text.npos ? std::wstring{}
                                  : text.substr(first, text.find_last_not_of(L' ') - first + 1);
    };
    auto text = options.title ? clean(image.title) : L"";
    const auto author = options.author ? clean(image.author) : L"";
    if (!author.empty())
        text += (text.empty() ? L"" : L"\n") + author;
    return text;
}
std::string utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    if (value.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Text too long");
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode");
    std::string r(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), r.data(),
                        n, nullptr, nullptr);
    return r;
}
std::wstring wide(std::string_view value)
{
    if (value.empty())
        return {};
    if (value.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Text too long");
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
    if (!n)
        throw std::runtime_error("Invalid UTF-8");
    std::wstring r(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), r.data(),
                        n);
    return r;
}
std::filesystem::path dataDirectory()
{
    if (!dataRoot.empty())
    {
        std::filesystem::create_directories(dataRoot);
        return dataRoot;
    }
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        throw std::runtime_error("Local application data is unavailable");
    std::filesystem::path r(p);
    CoTaskMemFree(p);
    r /= L"mRSSScreenSaver";
    std::filesystem::create_directories(r);
    return r;
}
void setDataDirectory(std::filesystem::path path)
{
    if (!path.is_absolute())
        throw std::runtime_error("Data directory must be absolute");
    dataRoot = std::move(path);
}
std::vector<std::byte> readFile(const std::filesystem::path &path, std::size_t limit)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Cannot read file");
    const auto size = f.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > limit)
        throw std::runtime_error("File exceeds limit");
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size())))
        throw std::runtime_error("Cannot read complete file");
    return data;
}
void atomicWrite(const std::filesystem::path &path, std::span<const std::byte> bytes, bool backup)
{
    Lock lock;
    std::filesystem::create_directories(path.parent_path());
    auto tmp = path;
    tmp += L".tmp";
    HANDLE file =
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot write file");
    DWORD written = 0;
    const bool ok = bytes.size() <= MAXDWORD &&
                    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok)
    {
        DeleteFileW(tmp.c_str());
        throw std::runtime_error("Cannot complete save");
    }
    if (backup && std::filesystem::exists(path))
    {
        auto bak = path;
        bak += L".bak";
        if (!ReplaceFileW(path.c_str(), tmp.c_str(), bak.c_str(), 0, nullptr, nullptr))
        {
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Cannot replace settings");
        }
    }
    else if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        throw std::runtime_error("Cannot commit file");
    }
}
void writeText(const std::filesystem::path &path, std::string_view value, bool backup)
{
    atomicWrite(path, {reinterpret_cast<const std::byte *>(value.data()), value.size()}, backup);
}
std::string digest(std::wstring_view value)
{
    auto input = utf8(value);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Hash provider unavailable");
    std::array<unsigned char, 32> result{};
    auto status = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0)
        status =
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0);
    if (status >= 0)
        status = BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0);
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (status < 0)
        throw std::runtime_error("Hash failed");
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (auto b : result)
        s << std::setw(2) << static_cast<unsigned>(b);
    return s.str();
}
json encodeSettings(const Settings &s)
{
    const auto &d = s.defaults;
    json j = {{"version", 2},
              {"maximumAge",
               {{"entire", d.age.entireFeed},
                {"value", d.age.value},
                {"unit", static_cast<int>(d.age.unit)},
                {"undated", d.age.includeUndated}}},
              {"displaySeconds", d.displaySeconds},
              {"transitionSeconds", d.transitionSeconds},
              {"refreshHours", d.refreshHours},
              {"cacheMiB", d.cacheBytes / (1024 * 1024)},
              {"downloads", d.simultaneousDownloads},
              {"allowMature", d.allowMature},
              {"excludeUnrated", d.excludeUnrated},
              {"transition", static_cast<int>(d.transition)},
              {"excludedUrls", strings(d.excludedUrlText)},
              {"random", s.random},
              {"balanceFeeds", s.balanceFeeds},
              {"fill", s.fill},
              {"theme", s.theme},
              {"randomEffects", s.randomEffects},
              {"background", s.background},
              {"hdrNits", s.hdrNits},
              {"metadataOverlay", encodeMetadata(s.metadata)},
              {"feeds", json::array()}};
    for (const auto &f : s.feeds)
    {
        const auto &p = f.policy;
        const auto &a = p.age;
        const auto &q = f.filter;
        j["feeds"].push_back({{"title", utf8(f.title)},
                              {"url", utf8(f.url)},
                              {"enabled", p.enabled},
                              {"http", p.allowHttp},
                              {"lan", p.allowPrivateNetwork},
                              {"ageOverride", p.overrideAge},
                              {"entire", a.entireFeed},
                              {"age", a.value},
                              {"unit", static_cast<int>(a.unit)},
                              {"undated", a.includeUndated},
                              {"duration", p.displaySeconds.value_or(0)},
                              {"metadataOverlay", f.metadata ? encodeMetadata(*f.metadata) : json(nullptr)},
                              {"excludedUrls", strings(p.excludedUrlText)},
                              {"filter",
                               {{"mode", static_cast<int>(q.mode)},
                                {"expression", utf8(q.expression)},
                                {"exclude", utf8(q.exclude)},
                                {"case", q.caseSensitive},
                                {"fields", q.fields}}}});
    }
    return j;
}
json parseJson(std::string_view text)
{
    if (text.size() > 4 * 1024 * 1024)
        throw std::runtime_error("JSON exceeds size limit");
    return json::parse(text, [&](int depth, json::parse_event_t, json &) {
        if (depth > 64)
            throw std::runtime_error("JSON nesting exceeds limit");
        return true;
    });
}
Settings decodeSettings(const json &j)
{
    const auto version = j.value("version", 0);
    if (!j.is_object() || (version != 1 && version != 2))
        throw std::runtime_error("Unsupported settings version");
    Settings s;
    auto &d = s.defaults;
    if (j.contains("maximumAge"))
    {
        const auto &age = j.at("maximumAge");
        if (!age.is_object())
            throw std::runtime_error("Invalid global maximum image age");
        d.age.entireFeed = age.value("entire", false);
        d.age.value = bounded(age, "value", 7, 1, 1200);
        d.age.unit = static_cast<AgeUnit>(bounded(age, "unit", 2, 0, 4));
        d.age.includeUndated = age.value("undated", false);
    }
    d.displaySeconds = bounded(j, "displaySeconds", 10, 1, 86400);
    d.transitionSeconds = bounded(j, "transitionSeconds", 2, 0, 30);
    d.refreshHours = bounded(j, "refreshHours", 24, 1, 8760);
    d.cacheBytes = static_cast<std::uint64_t>(bounded(j, "cacheMiB", 50, 10, 4096)) * 1024 * 1024;
    d.simultaneousDownloads = bounded(j, "downloads", 4, 1, 4);
    d.allowMature = j.value("allowMature", false);
    d.excludeUnrated = j.value("excludeUnrated", false);
    d.transition = static_cast<Transition>(bounded(j, "transition", 2, 0, 8));
    d.excludedUrlText = strings(j.value("excludedUrls", json::array()));
    s.random = j.value("random", false);
    s.balanceFeeds = j.value("balanceFeeds", false);
    s.fill = j.value("fill", false);
    s.theme = bounded(j, "theme", 0, 0, 2);
    s.randomEffects = bounded(j, "randomEffects", 127, 1, 127);
    s.background = bounded(j, "background", 0, 0, 0xffffff);
    s.hdrNits = bounded(j, "hdrNits", 250, 80, 1000);
    if (j.contains("metadataOverlay"))
        s.metadata = decodeMetadata(j.at("metadataOverlay"));
    const auto &feeds = j.at("feeds");
    if (!feeds.is_array())
        throw std::runtime_error("Settings feed list must be a JSON array");
    if (feeds.size() > 1000)
        throw std::runtime_error("Feed limit exceeded (maximum 1000 feeds)");
    for (const auto &v : feeds)
    {
        FeedConfig f;
        f.title = wide(v.value("title", std::string{}));
        f.url = wide(v.at("url").get<std::string>());
        auto &p = f.policy;
        if (!validateUrl(f.url, true).url)
            throw std::runtime_error("Invalid feed URL");
        p.enabled = v.value("enabled", true);
        p.allowHttp = v.value("http", false);
        p.allowPrivateNetwork = v.value("lan", false);
        p.age.entireFeed = v.value("entire", false);
        p.age.value = bounded(v, "age", 7, 1, 1200);
        p.age.unit = static_cast<AgeUnit>(bounded(v, "unit", 2, 0, 4));
        p.age.includeUndated = v.value("undated", false);
        // Only an explicit override flag opts a feed out of global defaults.
        p.overrideAge = v.value("ageOverride", false);
        const auto duration = bounded(v, "duration", 0, 0, 86400);
        if (duration)
            p.displaySeconds = duration;
        p.excludedUrlText = strings(v.value("excludedUrls", json::array()));
        if (v.contains("filter"))
        {
            const auto &q = v.at("filter");
            f.filter.mode = static_cast<FilterMode>(bounded(q, "mode", 0, 0, 2));
            f.filter.expression = wide(q.value("expression", std::string{}));
            f.filter.exclude = wide(q.value("exclude", std::string{}));
            f.filter.caseSensitive = q.value("case", false);
            f.filter.fields = bounded(q, "fields", 3, 1, 31);
            validateFilter(f.filter);
        }
        if (v.contains("metadataOverlay") && !v.at("metadataOverlay").is_null())
            f.metadata = decodeMetadata(v.at("metadataOverlay"));
        s.feeds.push_back(std::move(f));
    }
    return s;
}
FeedConfig decodeOutline(const Outline &outline)
{
    FeedConfig result;
    result.title = outline.title;
    result.url = outline.url;
    if (!outline.settings.empty())
    {
        auto extension = parseJson(outline.settings);
        if (!extension.is_object())
            throw std::runtime_error("OPML per-feed settings must be a JSON object");
        if (!extension.contains("ageOverride"))
            extension["ageOverride"] = false;
        extension["url"] = utf8(outline.url);
        auto wrapper = encodeSettings(Settings{});
        wrapper["feeds"] = json::array();
        wrapper["feeds"].push_back(extension);
        result = decodeSettings(wrapper).feeds.at(0);
    }
    if (!outline.title.empty())
        result.title = outline.title;
    result.policy.allowPrivateNetwork = false;
    return result;
}
Settings loadSettings(std::wstring &recovery)
{
    const auto path = dataDirectory() / L"settings.json";
    if (!std::filesystem::exists(path))
        return {};
    auto read = [&](const auto &p) {
        auto b = readFile(p, 4 * 1024 * 1024);
        int depth = 0;
        auto j = json::parse(b.begin(), b.end(), [&](int d, json::parse_event_t, json &) {
            depth = (std::max)(depth, d);
            if (depth > 64)
                throw std::runtime_error("Settings nesting limit");
            return true;
        });
        return decodeSettings(j);
    };
    try
    {
        return read(path);
    }
    catch (...)
    {
        auto bak = path;
        bak += L".bak";
        try
        {
            auto s = read(bak);
            recovery = L"Settings were damaged. Recovered the last saved backup. Review before applying.";
            return s;
        }
        catch (...)
        {
            throw std::runtime_error("Settings and backup could not be read. Original files have been "
                                     "preserved; use Restore Defaults only if you intend to replace them.");
        }
    }
}
void saveSettings(const Settings &s)
{
    Lock lock;
    const auto j = encodeSettings(s);
    const auto serialized = j.dump(2);
    if (serialized.size() > 4 * 1024 * 1024)
        throw std::runtime_error("Settings exceed the 4 MiB limit; reduce the feed list before saving");
    (void)decodeSettings(j);
    const auto path = dataDirectory() / L"settings.json";
    bool valid = false;
    if (std::filesystem::exists(path))
        try
        {
            const auto b = readFile(path, 4 * 1024 * 1024);
            (void)decodeSettings(json::parse(b.begin(), b.end()));
            valid = true;
        }
        catch (...)
        {
            auto damaged = path;
            damaged += L".damaged";
            std::filesystem::copy_file(path, damaged, std::filesystem::copy_options::overwrite_existing);
        }
    writeText(path, serialized, valid);
}
void withDataLock(const std::function<void()> &action)
{
    Lock lock;
    action();
}
std::wstring filterText(const MediaImage &image, unsigned fields)
{
    std::wstring t;
    if (fields & 1)
        t += image.title + L"\n";
    if (fields & 2)
        t += image.description + L"\n";
    if (fields & 4)
        t += image.url + L"\n";
    if (fields & 8)
        t += image.author + L"\n";
    if (fields & 16)
        t += image.keywords;
    return t;
}
} // namespace mrss
