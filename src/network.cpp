// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 MorpheusUK. All Rights Reserved.

#include <winsock2.h>
#include <ws2tcpip.h>
#include "mrss/network.hpp"
#include "mrss/version.hpp"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <shlwapi.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>

namespace mrss
{
namespace
{
struct Winsock
{
    Winsock()
    {
        WSADATA d{};
        if (WSAStartup(MAKEWORD(2, 2), &d))
            throw std::runtime_error("Networking unavailable");
    }
    ~Winsock()
    {
        WSACleanup();
    }
};
struct Handle
{
    std::atomic<HINTERNET> value;
    explicit Handle(HINTERNET h) : value(h)
    {
        if (!h)
            throw std::runtime_error("Cannot create network request");
    }
    ~Handle()
    {
        close();
    }
    void close()
    {
        if (auto h = value.exchange(nullptr))
            WinHttpCloseHandle(h);
    }
    operator HINTERNET() const
    {
        return value.load();
    }
};
void cancelled(std::stop_token s)
{
    if (s.stop_requested())
        throw std::runtime_error("Cancelled");
}
void checked(BOOL ok)
{
    if (!ok)
    {
        const auto e = GetLastError();
        if (e == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED || e == ERROR_WINHTTP_LOGIN_FAILURE)
            throw std::runtime_error("Authentication required; authenticated feeds are unsupported");
        throw std::runtime_error("Network error " + std::to_string(e) + " (DNS, connection or TLS)");
    }
}
std::mutex gateMutex;
std::condition_variable_any gateCondition;
unsigned active = 0;
std::map<std::wstring, unsigned> hosts;
struct Gate
{
    std::wstring host;
    Gate(std::wstring h, std::stop_token stop, unsigned maximum) : host(std::move(h))
    {
        std::unique_lock lock(gateMutex);
        if (!gateCondition.wait(lock, stop, [&] { return active < maximum && hosts[host] < 2; }))
            throw std::runtime_error("Cancelled");
        ++active;
        ++hosts[host];
    }
    ~Gate()
    {
        std::lock_guard lock(gateMutex);
        --active;
        if (!--hosts[host])
            hosts.erase(host);
        gateCondition.notify_all();
    }
};
std::wstring header(HINTERNET h, DWORD id)
{
    DWORD bytes = 0;
    WinHttpQueryHeaders(h, id, nullptr, nullptr, &bytes, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes > 32768)
        return {};
    std::wstring r(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(h, id, nullptr, r.data(), &bytes, nullptr))
        return {};
    r.resize(bytes / sizeof(wchar_t));
    while (!r.empty() && r.back() == 0)
        r.pop_back();
    return r;
}
std::wstring pinnedAddress(std::wstring host, bool allowPrivate, std::stop_token stop)
{
    if (host.starts_with(L"[") && host.ends_with(L"]"))
        host = host.substr(1, host.size() - 2);
    ADDRINFOEXW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOEXW *raw = nullptr;
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent)
        throw std::runtime_error("Cannot start DNS lookup");
    HANDLE cancelHandle = nullptr;
    timeval timeout{8, 0};
    auto result = GetAddrInfoExW(host.c_str(), nullptr, NS_DNS, nullptr, &hints, &raw, &timeout, &operation,
                                 nullptr, &cancelHandle);
    if (result == WSA_IO_PENDING)
    {
        while (WaitForSingleObject(operation.hEvent, 50) == WAIT_TIMEOUT)
        {
            if (stop.stop_requested())
                GetAddrInfoExCancel(&cancelHandle);
        }
        result = GetAddrInfoExOverlappedResult(&operation);
    }
    CloseHandle(operation.hEvent);
    std::unique_ptr<ADDRINFOEXW, decltype(&FreeAddrInfoExW)> addresses(raw, FreeAddrInfoExW);
    cancelled(stop);
    if (result)
        throw std::runtime_error("Unreachable: DNS lookup failed");
    std::wstring chosen;
    for (auto *a = raw; a; a = a->ai_next)
    {
        wchar_t ip[NI_MAXHOST]{};
        if (GetNameInfoW(a->ai_addr, static_cast<socklen_t>(a->ai_addrlen), ip, NI_MAXHOST, nullptr, 0,
                         NI_NUMERICHOST))
            continue;
        if (!allowPrivate && !publicAddress(ip))
            throw std::runtime_error(
                "Blocked private or special-use destination; enable LAN access only for a trusted feed");
        if (chosen.empty())
            chosen = ip;
    }
    if (chosen.empty())
        throw std::runtime_error("No usable server address");
    return chosen;
}
} // namespace
bool offlineHint()
{
    NL_NETWORK_CONNECTIVITY_HINT hint{};
    return GetNetworkConnectivityHint(&hint) == NO_ERROR &&
           (hint.ConnectivityLevel == NetworkConnectivityLevelHintNone ||
            hint.ConnectivityLevel == NetworkConnectivityLevelHintLocalAccess);
}
bool publicAddress(std::wstring_view address)
{
    IN_ADDR a4{};
    const std::wstring s(address);
    if (InetPtonW(AF_INET, s.c_str(), &a4) == 1)
    {
        const auto n = ntohl(a4.S_un.S_addr);
        const auto a = n >> 24, b = (n >> 16) & 255, c = (n >> 8) & 255;
        return a != 0 && a != 10 && a != 127 && a < 224 && !(a == 100 && b >= 64 && b <= 127) &&
               !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
               !(a == 192 && (b == 168 || (b == 0) || (b == 2))) &&
               !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
               !(a == 203 && b == 0 && c == 113);
    }
    IN6_ADDR a6{};
    if (InetPtonW(AF_INET6, s.c_str(), &a6) != 1)
        return false;
    const auto *p = a6.u.Byte;
    if (IN6_IS_ADDR_V4MAPPED(&a6))
    {
        wchar_t out[64]{};
        InetNtopW(AF_INET, const_cast<UCHAR *>(p + 12), out, 64);
        return publicAddress(out);
    }
    // Global-unicast only, excluding documentation, transition and special ranges.
    return (p[0] & 0xe0) == 0x20 &&
           !(p[0] == 0x20 && p[1] == 0x01 && (p[2] < 2 || (p[2] == 0x0d && p[3] == 0xb8))) &&
           !(p[0] == 0x20 && p[1] == 0x02);
}
Response download(std::wstring url, const FeedPolicy &policy, std::span<const std::wstring> exclusions,
                  std::size_t limit, std::stop_token stop, std::wstring_view etag, std::wstring_view modified,
                  unsigned maxConcurrent)
{
    static Winsock winsock;
    (void)winsock;
    const auto origin = validateUrl(url, policy.allowHttp);
    if (!origin.url)
        throw std::runtime_error(utf8(origin.error));
    for (unsigned redirect = 0; redirect <= 5; ++redirect)
    {
        cancelled(stop);
        const auto parsed = validateUrl(url, policy.allowHttp);
        if (!parsed.url)
            throw std::runtime_error(utf8(parsed.error));
        if (excludedUrl(url, exclusions))
            throw std::runtime_error("URL excluded by substring rule");
        Gate gate(parsed.url->host, stop, (std::clamp)(maxConcurrent, 1u, 4u));
        const auto ip = pinnedAddress(parsed.url->host, policy.allowPrivateNetwork, stop);
        cancelled(stop);
        // Direct connections avoid implicit proxy authentication and ensure that
        // the pinned, validated address is the endpoint actually contacted.
        Handle session(WinHttpOpen(L"mRSSScreenSaver/" MRSS_APP_VERSION_W, WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   nullptr, nullptr, 0));
        checked(WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000));
        Handle connection(WinHttpConnect(session, parsed.url->host.c_str(), parsed.url->port, 0));
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        checked(WinHttpCrackUrl(url.c_str(), 0, 0, &parts));
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (path.empty())
            path = L"/";
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Handle request(WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          parsed.url->secure ? WINHTTP_FLAG_SECURE : 0));
        const DWORD disabled =
            WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
        checked(WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, const_cast<DWORD *>(&disabled),
                                 sizeof(disabled)));
        DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        checked(WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth)));
        checked(WinHttpSetOption(request, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                                 const_cast<wchar_t *>(ip.c_str()),
                                 static_cast<DWORD>((ip.size() + 1) * sizeof(wchar_t))));
        std::stop_callback cancel(stop, [&] { request.close(); });
        std::wstring headers = L"Accept: application/rss+xml, application/atom+xml, application/feed+json, "
                               L"application/json, application/xml, "
                               L"image/*;q=0.9, */*;q=0.1\r\n";
        auto safeHeader = [](std::wstring_view v) {
            return v.size() < 4096 && v.find_first_of(L"\r\n") == v.npos;
        };
        if (url == origin.url->original)
        {
            if (!etag.empty() && safeHeader(etag))
                headers += L"If-None-Match: " + std::wstring(etag) + L"\r\n";
            if (!modified.empty() && safeHeader(modified))
                headers += L"If-Modified-Since: " + std::wstring(modified) + L"\r\n";
        }
        checked(WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), nullptr, 0,
                                   0, 0));
        checked(WinHttpReceiveResponse(request, nullptr));
        cancelled(stop);
        DWORD status = 0, size = sizeof(status);
        checked(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                    &status, &size, nullptr));
        if (status == 401 || status == 407)
            throw std::runtime_error("Authentication required; authenticated feeds are unsupported");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            auto location = header(request, WINHTTP_QUERY_LOCATION);
            if (location.empty())
                throw std::runtime_error("Redirect has no destination");
            std::vector<wchar_t> combined(16385);
            DWORD length = static_cast<DWORD>(combined.size());
            if (FAILED(UrlCombineW(url.c_str(), location.c_str(), combined.data(), &length, 0)))
                throw std::runtime_error("Invalid redirect");
            url.assign(combined.data(), length);
            continue;
        }
        Response r;
        r.status = status;
        r.finalUrl = url;
        r.etag = header(request, WINHTTP_QUERY_ETAG);
        r.modified = header(request, WINHTTP_QUERY_LAST_MODIFIED);
        if (status == 304)
            return r;
        if (status != 200)
            throw std::runtime_error("Server returned HTTP " + std::to_string(status));
        const auto length = header(request, WINHTTP_QUERY_CONTENT_LENGTH);
        if (!length.empty())
        {
            try
            {
                if (std::stoull(length) > limit)
                    throw std::runtime_error("Download exceeds size limit");
            }
            catch (const std::invalid_argument &)
            {
                throw std::runtime_error("Invalid content length");
            }
        }
        std::array<std::byte, 32768> buffer{};
        DWORD got = 0;
        const auto start = std::chrono::steady_clock::now();
        for (;;)
        {
            cancelled(stop);
            checked(WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &got));
            if (!got)
                break;
            if (r.body.size() + got > limit)
                throw std::runtime_error("Download exceeds size limit");
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(45))
                throw std::runtime_error("Download took too long");
            r.body.insert(r.body.end(), buffer.begin(), buffer.begin() + got);
        }
        return r;
    }
    throw std::runtime_error("Too many redirects");
}
} // namespace mrss
