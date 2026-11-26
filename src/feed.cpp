// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include "mrss/feed.hpp"
#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <memory>
#include <sstream>

namespace mrss
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr auto mediaNs = L"http://search.yahoo.com/mrss/";
constexpr auto atomNs = L"http://www.w3.org/2005/Atom";
constexpr std::size_t maxXmlBytes = 8 * 1024 * 1024;
struct Node
{
    std::wstring name, ns, text;
    std::map<std::wstring, std::wstring> attrs;
    std::vector<std::unique_ptr<Node>> children;
};
void check(HRESULT result)
{
    if (FAILED(result))
        throw ParseError("Invalid or unsafe XML");
}
std::wstring readerName(IXmlReader *r, bool ns = false)
{
    const wchar_t *p = nullptr;
    UINT size = 0;
    check(ns ? r->GetNamespaceUri(&p, &size) : r->GetLocalName(&p, &size));
    return {p, size};
}
std::unique_ptr<Node> readXml(std::span<const std::byte> bytes)
{
    if (bytes.empty() || bytes.size() > maxXmlBytes)
        throw ParseError("XML exceeds size limit or is empty");
    ComPtr<IStream> stream;
    stream.Attach(
        SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.data()), static_cast<UINT>(bytes.size())));
    if (!stream)
        throw ParseError("Cannot allocate XML input");
    ComPtr<IXmlReader> reader;
    check(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void **>(reader.GetAddressOf()), nullptr));
    check(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    check(reader->SetProperty(XmlReaderProperty_XmlResolver, 0));
    check(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64));
    check(reader->SetProperty(XmlReaderProperty_MaxEntityExpansion, 0));
    check(reader->SetInput(stream.Get()));
    std::unique_ptr<Node> root;
    std::vector<Node *> stack;
    std::size_t nodeCount = 0, totalText = 0;
    XmlNodeType type{};
    HRESULT hr{};
    while ((hr = reader->Read(&type)) == S_OK)
    {
        if (type == XmlNodeType_Element)
        {
            if (++nodeCount > 100000 || stack.size() >= 64)
                throw ParseError("XML structure exceeds limits");
            auto n = std::make_unique<Node>();
            n->name = readerName(reader.Get());
            n->ns = readerName(reader.Get(), true);
            if (reader->MoveToFirstAttribute() == S_OK)
            {
                unsigned attributes = 0;
                do
                {
                    if (++attributes > 64)
                        throw ParseError("Too many XML attributes");
                    const wchar_t *value = nullptr;
                    UINT len = 0;
                    check(reader->GetValue(&value, &len));
                    if (len > 262144)
                        throw ParseError("XML attribute is too long");
                    // Namespace declarations and namespaced attributes cannot
                    // impersonate unqualified URL/type attributes.
                    const auto ns = readerName(reader.Get(), true);
                    if (ns.empty())
                        n->attrs.emplace(readerName(reader.Get()), std::wstring(value, len));
                    else if (ns == L"urn:mrss-screen-saver:1" && readerName(reader.Get()) == L"settings")
                        n->attrs.emplace(L"mrssSettings", std::wstring(value, len));
                } while (reader->MoveToNextAttribute() == S_OK);
                check(reader->MoveToElement());
            }
            Node *ptr = n.get();
            if (stack.empty())
            {
                if (root)
                    throw ParseError("Multiple XML roots");
                root = std::move(n);
            }
            else
                stack.back()->children.push_back(std::move(n));
            if (!reader->IsEmptyElement())
                stack.push_back(ptr);
        }
        else if (type == XmlNodeType_EndElement)
        {
            if (stack.empty())
                throw ParseError("Unbalanced XML");
            stack.pop_back();
        }
        else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)
        {
            const wchar_t *text = nullptr;
            UINT len = 0;
            check(reader->GetValue(&text, &len));
            totalText += len;
            if (totalText > maxXmlBytes || (!stack.empty() && stack.back()->text.size() + len > 1024 * 1024))
                throw ParseError("XML text exceeds limits");
            if (!stack.empty())
                stack.back()->text.append(text, len);
        }
        else if (type == XmlNodeType_DocumentType)
            throw ParseError("XML DTD is prohibited");
    }
    check(hr);
    if (!root || !stack.empty())
        throw ParseError("Incomplete XML");
    return root;
}
std::wstring trim(std::wstring_view v)
{
    const auto a = v.find_first_not_of(L" \t\r\n");
    if (a == v.npos)
        return {};
    return std::wstring(v.substr(a, v.find_last_not_of(L" \t\r\n") - a + 1));
}
std::wstring attr(const Node &n, const wchar_t *key)
{
    const auto i = n.attrs.find(key);
    return i == n.attrs.end() ? L"" : i->second;
}
const Node *child(const Node &n, const wchar_t *name, const wchar_t *ns = L"")
{
    for (const auto &c : n.children)
        if (c->name == name && c->ns == ns)
            return c.get();
    return nullptr;
}
std::wstring text(const Node &n, const wchar_t *name, const wchar_t *ns = L"")
{
    const auto *c = child(n, name, ns);
    return c ? trim(c->text) : L"";
}
Rating ratingOf(const Node &n, Rating inherited)
{
    // A conflicting declaration at one scope must not conceal a mature rating.
    for (const auto &c : n.children)
        if (c->ns == mediaNs && (c->name == L"rating" || c->name == L"adult"))
        {
            auto v = trim(c->text);
            for (auto &ch : v)
                if (ch >= L'A' && ch <= L'Z')
                    ch += L'a' - L'A';
            if ((c->name == L"adult" && (v == L"true" || v == L"1")) ||
                (c->name == L"rating" && (v == L"adult" || v == L"r" || v == L"nc-17" || v == L"x" ||
                                          v == L"tv-ma" || v == L"18" || v == L"r18")))
                return Rating::adult;
        }
    if (const auto *r = child(n, L"rating", mediaNs))
    {
        auto scheme = attr(*r, L"scheme");
        auto value = trim(r->text);
        auto lower = [](std::wstring &s) {
            for (auto &c : s)
                if (c >= L'A' && c <= L'Z')
                    c += L'a' - L'A';
        };
        lower(scheme);
        lower(value);
        if (scheme.empty() || scheme == L"urn:simple")
        {
            if (value == L"adult")
                return Rating::adult;
            if (value == L"nonadult")
                return Rating::unrestricted;
        }
        if (scheme == L"urn:mpaa")
        {
            if (value == L"r" || value == L"nc-17" || value == L"x")
                return Rating::adult;
            if (value == L"g" || value == L"pg" || value == L"pg-13")
                return Rating::unrestricted;
        }
        if (scheme == L"urn:v-chip" || scheme == L"urn:tv")
        {
            if (value == L"tv-ma")
                return Rating::adult;
            if (value == L"tv-y" || value == L"tv-y7" || value == L"tv-g" || value == L"tv-pg" ||
                value == L"tv-14")
                return Rating::unrestricted;
        }
        if (scheme == L"urn:bbfc")
        {
            if (value == L"18" || value == L"r18")
                return Rating::adult;
            if (value == L"u" || value == L"pg" || value == L"12" || value == L"12a" || value == L"15")
                return Rating::unrestricted;
        }
        return Rating::unknown;
    }
    if (const auto *r = child(n, L"adult", mediaNs))
    {
        const auto value = trim(r->text);
        if (value == L"true" || value == L"1")
            return Rating::adult;
        if (value == L"false" || value == L"0")
            return Rating::unrestricted;
        return Rating::unknown;
    }
    return inherited;
}
std::wstring resolve(std::wstring_view base, const std::wstring &relative)
{
    if (relative.empty())
        return {};
    std::vector<wchar_t> output(16385);
    DWORD length = static_cast<DWORD>(output.size());
    if (FAILED(UrlCombineW(std::wstring(base).c_str(), relative.c_str(), output.data(), &length, 0)))
        return {};
    const auto valid = validateUrl(std::wstring_view(output.data(), length), true);
    return valid.url ? valid.url->original : L"";
}
bool imageDeclaration(const Node &n)
{
    const auto medium = attr(n, L"medium"), type = attr(n, L"type");
    if (!medium.empty() && medium != L"image")
        return false;
    return type.empty() ? medium == L"image" : type.starts_with(L"image/");
}
std::wstring mediaCredit(const Node &node, const std::wstring &fallback)
{
    std::wstring result;
    for (const auto &c : node.children)
        if (c->ns == mediaNs && c->name == L"credit")
        {
            const auto name = trim(c->text);
            if (!name.empty())
                result += ((result.empty() ? L"" : L", ") + name).substr(0, 4096 - result.size());
            if (result.size() >= 4096)
                break;
        }
    return (result.empty() ? fallback : result).substr(0, 4096);
}
std::wstring standardAuthor(const Node &node, bool atom)
{
    if (atom)
    {
        const auto author = child(node, L"author", atomNs);
        return author ? text(*author, L"name", atomNs).substr(0, 4096) : L"";
    }
    auto author = text(node, L"author");
    if (author.empty())
        author = text(node, L"creator", L"http://purl.org/dc/elements/1.1/");
    return author.substr(0, 4096);
}
void appendMedia(const Node &node, Rating inherited, std::optional<Time> date, const std::wstring &title,
                 const std::wstring &description, const std::wstring &author, std::wstring_view base,
                 Feed &result)
{
    const auto rating = ratingOf(node, inherited);
    const auto ownTitle = text(node, L"title", mediaNs);
    const auto ownDescription = text(node, L"description", mediaNs);
    const auto ownCredit = mediaCredit(node, author);
    for (const auto &c : node.children)
    {
        if (c->ns != mediaNs)
            continue;
        if (c->name == L"group")
        {
            appendMedia(*c, rating, date, ownTitle.empty() ? title : ownTitle,
                        ownDescription.empty() ? description : ownDescription, ownCredit, base, result);
        }
        else if (c->name == L"content" && imageDeclaration(*c))
        {
            const auto url = resolve(base, attr(*c, L"url"));
            if (url.empty())
                continue;
            const auto imageTitle = text(*c, L"title", mediaNs),
                       imageDescription = text(*c, L"description", mediaNs);
            result.images.push_back(
                {url, imageTitle.empty() ? (ownTitle.empty() ? title : ownTitle) : imageTitle,
                 imageDescription.empty() ? (ownDescription.empty() ? description : ownDescription)
                                          : imageDescription,
                 ratingOf(*c, rating), date, mediaCredit(*c, ownCredit)});
        }
    }
}
std::wstring escape(std::wstring_view v)
{
    std::wstring s;
    for (const auto c : v)
    {
        switch (c)
        {
        case L'&':
            s += L"&amp;";
            break;
        case L'<':
            s += L"&lt;";
            break;
        case L'>':
            s += L"&gt;";
            break;
        case L'"':
            s += L"&quot;";
            break;
        default:
            if (c < 32 && c != L'\t' && c != L'\r' && c != L'\n')
                throw ParseError("Invalid OPML text");
            s += c;
        }
    }
    return s;
}
void outlines(const Node &n, std::vector<Outline> &result)
{
    for (const auto &c : n.children)
    {
        if (c->name != L"outline" || !c->ns.empty())
            continue;
        const auto url = attr(*c, L"xmlUrl");
        if (!url.empty())
        {
            if (!validateUrl(url, true).url)
                throw ParseError("OPML contains an invalid or credential-bearing URL");
            auto title = attr(*c, L"title");
            if (title.empty())
                title = attr(*c, L"text");
            std::string settings;
            const auto extension = attr(*c, L"mrssSettings");
            if (!extension.empty())
            {
                const auto size =
                    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, extension.data(),
                                        static_cast<int>(extension.size()), nullptr, 0, nullptr, nullptr);
                if (size <= 0)
                    throw ParseError("Invalid OPML extension");
                settings.resize(static_cast<std::size_t>(size));
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, extension.data(),
                                    static_cast<int>(extension.size()), settings.data(), size, nullptr,
                                    nullptr);
            }
            if (std::none_of(result.begin(), result.end(), [&](const auto &f) { return f.url == url; }))
                result.push_back({title, url, settings});
        }
        outlines(*c, result);
    }
}
} // namespace

std::optional<Time> parseDate(std::wstring_view value)
{
    const auto s = trim(value);
    if (s.empty() || s.size() > 128)
        return {};
    using namespace std::chrono;
    // RFC3339: explicit UTC or numeric offset, optional fractional seconds.
    if (s.size() >= 20 && s[4] == L'-' && s[7] == L'-' && (s[10] == L'T' || s[10] == L't'))
    {
        auto number = [&](std::size_t start, std::size_t len) -> int {
            if (start + len > s.size())
                return -1;
            int n = 0;
            for (std::size_t i = start; i < start + len; ++i)
            {
                if (s[i] < L'0' || s[i] > L'9')
                    return -1;
                n = n * 10 + s[i] - L'0';
            }
            return n;
        };
        const int y = number(0, 4), m = number(5, 2), d = number(8, 2), h = number(11, 2),
                  min = number(14, 2), sec = number(17, 2);
        if (s[13] != L':' || s[16] != L':' || y < 1601 || m < 1 || d < 1 || h < 0 || h > 23 || min < 0 ||
            min > 59 || sec < 0 || sec > 59)
            return {};
        const year_month_day date{year(y), month(static_cast<unsigned>(m)), day(static_cast<unsigned>(d))};
        if (!date.ok())
            return {};
        std::size_t p = 19;
        if (s[p] == L'.')
        {
            ++p;
            const auto begin = p;
            while (p < s.size() && s[p] >= L'0' && s[p] <= L'9')
                ++p;
            if (p == begin)
                return {};
        }
        if (p >= s.size())
            return {};
        int offset = 0;
        if (s[p] == L'Z' || s[p] == L'z')
        {
            if (p + 1 != s.size())
                return {};
        }
        else if ((s[p] == L'+' || s[p] == L'-') && p + 6 == s.size() && s[p + 3] == L':')
        {
            const int oh = number(p + 1, 2), om = number(p + 4, 2);
            if (oh < 0 || oh > 23 || om < 0 || om > 59)
                return {};
            offset = (oh * 60 + om) * (s[p] == L'+' ? 1 : -1);
        }
        else
            return {};
        return sys_days(date) + hours(h) + minutes(min - offset) + seconds(sec);
    }
    // RSS/RFC822 timestamps need their numeric timezone applied explicitly;
    // WinHTTP's HTTP-date parser alone does not provide that guarantee.
    auto input = s;
    const auto comma = input.find(L',');
    if (comma != input.npos)
        input = input.substr(comma + 1);
    std::wistringstream in(input);
    in.imbue(std::locale::classic());
    int d = 0, y = 0;
    std::wstring monthName, time, zone, extra;
    if (!(in >> d >> monthName >> y >> time >> zone) || (in >> extra))
        return {};
    const std::wstring months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                   L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    unsigned m = 0;
    for (unsigned i = 0; i < 12; ++i)
        if (monthName == months[i])
            m = i + 1;
    if (y >= 0 && y < 100)
        y += y < 50 ? 2000 : 1900;
    if (y < 1601 || y > 9999 || !m || d < 1)
        return {};
    const year_month_day date{year(y), month(m), day(static_cast<unsigned>(d))};
    if (!date.ok())
        return {};
    auto digits = [](std::wstring_view v) -> int {
        if (v.empty())
            return -1;
        int n = 0;
        for (auto c : v)
        {
            if (c < L'0' || c > L'9')
                return -1;
            n = n * 10 + c - L'0';
        }
        return n;
    };
    if ((time.size() != 5 && time.size() != 8) || time[2] != L':' || (time.size() == 8 && time[5] != L':'))
        return {};
    const int h = digits(std::wstring_view(time).substr(0, 2)),
              min = digits(std::wstring_view(time).substr(3, 2)),
              sec = time.size() == 8 ? digits(std::wstring_view(time).substr(6, 2)) : 0;
    if (h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return {};
    int offset = 0;
    if (zone == L"GMT" || zone == L"UTC" || zone == L"UT" || zone == L"Z")
    {
    }
    else if (zone.size() == 5 && (zone[0] == L'+' || zone[0] == L'-'))
    {
        const auto oh = digits(std::wstring_view(zone).substr(1, 2)),
                   om = digits(std::wstring_view(zone).substr(3, 2));
        if (oh < 0 || oh > 23 || om < 0 || om > 59)
            return {};
        offset = (oh * 60 + om) * (zone[0] == L'+' ? 1 : -1);
    }
    else
    {
        const std::map<std::wstring, int> zones{{L"EST", -300}, {L"EDT", -240}, {L"CST", -360},
                                                {L"CDT", -300}, {L"MST", -420}, {L"MDT", -360},
                                                {L"PST", -480}, {L"PDT", -420}};
        const auto it = zones.find(zone);
        if (it == zones.end())
            return {};
        offset = it->second;
    }
    return sys_days(date) + hours(h) + minutes(min - offset) + seconds(sec);
}
Feed parseFeed(std::span<const std::byte> xml, std::wstring_view baseUrl)
{
    auto sniff = std::string_view(reinterpret_cast<const char *>(xml.data()), xml.size());
    if (sniff.starts_with("\xEF\xBB\xBF"))
        sniff.remove_prefix(3);
    const auto first = sniff.find_first_not_of(" \t\r\n");
    if (first != sniff.npos && (sniff[first] == '{' || sniff[first] == '['))
        return parseJsonImageFeed(xml, baseUrl);
    const auto root = readXml(xml);
    const bool atom = root->name == L"feed" && root->ns == atomNs;
    const Node *channel = atom ? root.get() : child(*root, L"channel");
    if (!atom && (root->name != L"rss" || !root->ns.empty() || attr(*root, L"version") != L"2.0"))
        throw ParseError("Only RSS 2.0 and Atom feeds are supported");
    if (!channel)
        throw ParseError("RSS channel is missing");
    Feed result;
    result.title = text(*channel, L"title", atom ? atomNs : L"");
    const auto inherited = ratingOf(*channel, Rating::unrated);
    const auto feedAuthor = mediaCredit(*channel, standardAuthor(*channel, atom));
    for (const auto &item : channel->children)
    {
        if (item->name != (atom ? L"entry" : L"item") || item->ns != (atom ? atomNs : L""))
            continue;
        const auto title = text(*item, L"title", atom ? atomNs : L"");
        const auto description = text(*item, atom ? L"summary" : L"description", atom ? atomNs : L"");
        auto date = parseDate(text(*item, atom ? L"published" : L"pubDate", atom ? atomNs : L""));
        if (!date && atom)
            date = parseDate(text(*item, L"updated", atomNs));
        const auto imageStart = result.images.size();
        const auto itemAuthor = standardAuthor(*item, atom);
        const auto author = mediaCredit(*item, itemAuthor.empty() ? feedAuthor : itemAuthor);
        appendMedia(*item, inherited, date, title, description, author, baseUrl, result);
        for (const auto &c : item->children)
        {
            const bool enclosure =
                atom ? (c->name == L"link" && c->ns == atomNs && attr(*c, L"rel") == L"enclosure")
                     : (c->name == L"enclosure" && c->ns.empty());
            if (!enclosure || !attr(*c, L"type").starts_with(L"image/"))
                continue;
            const auto url = resolve(baseUrl, attr(*c, atom ? L"href" : L"url"));
            if (!url.empty())
                result.images.push_back({url, title, description, ratingOf(*item, inherited), date});
        }
        std::wstring keywords = text(*item, L"keywords", mediaNs);
        for (const auto &c : item->children)
            if (c->name == L"category")
                keywords += L" " + (atom ? attr(*c, L"term") : c->text);
        for (auto i = imageStart; i < result.images.size(); ++i)
        {
            if (result.images[i].author.empty())
                result.images[i].author = author;
            result.images[i].keywords = keywords;
        }
    }
    return result;
}
std::vector<Outline> parseOpml(std::span<const std::byte> xml)
{
    const auto root = readXml(xml);
    if (root->name != L"opml" || !root->ns.empty())
        throw ParseError("Not an OPML document");
    const auto *body = child(*root, L"body");
    if (!body)
        throw ParseError("OPML body is missing");
    std::vector<Outline> result;
    outlines(*body, result);
    return result;
}
std::string exportOpml(std::span<const Outline> feeds)
{
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\" "
        L"xmlns:mrss=\"urn:mrss-screen-saver:1\"><head><title>mRSS Screen Saver feeds</title></head><body>\n";
    for (const auto &f : feeds)
    {
        if (!validateUrl(f.url, true).url)
            throw ParseError("Cannot export invalid URL");
        std::wstring extension;
        if (!f.settings.empty())
        {
            const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f.settings.data(),
                                               static_cast<int>(f.settings.size()), nullptr, 0);
            if (!n)
                throw ParseError("Invalid OPML extension");
            extension.resize(static_cast<std::size_t>(n));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, f.settings.data(),
                                static_cast<int>(f.settings.size()), extension.data(), n);
        }
        xml += L"<outline type=\"rss\" text=\"" + escape(f.title) + L"\" title=\"" + escape(f.title) +
               L"\" xmlUrl=\"" + escape(f.url) + L"\"" +
               (extension.empty() ? L"" : L" mrss:settings=\"" + escape(extension) + L"\"") + L"/>\n";
    }
    xml += L"</body></opml>\n";
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(),
                                           static_cast<int>(xml.size()), nullptr, 0, nullptr, nullptr);
    if (!count)
        throw ParseError("Invalid Unicode in OPML");
    std::string output(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, xml.data(), static_cast<int>(xml.size()),
                             output.data(), count, nullptr, nullptr))
        throw ParseError("Cannot encode OPML");
    return output;
}
} // namespace mrss
