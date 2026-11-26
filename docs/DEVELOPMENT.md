# Developer guide

This document keeps the implementation, diagnostics and build-environment notes
separate from the [main README](../README.md). The current release is 1.0.0.

## Build configuration

The `vs2022-x64` CMake preset generates `build/mRSSScreenSaver.sln`. The `release`
and `debug` build presets select the corresponding Visual Studio configuration.
Build output stays under `build/`.

Set `DARKMODELIB_ROOT` to an external darkmodelib source checkout. Its historical
default is `I:/darkmodelib`, so override it on other machines. The application
does not modify that checkout. The tested revision is recorded in
[DEPENDENCIES.md](DEPENDENCIES.md).

GoogleTest, PCRE2 and nlohmann/json are pinned to recorded commits. Existing
sources under `third_party/` are used when present; otherwise CMake fetches them.
These downloaded source folders are excluded from Git. GoogleTest is used only
by the tests. PCRE2 uses its 16-bit static build with JIT disabled.

The installer target is available when CMake finds Inno Setup. Discovery includes
`C:/Program Files/Inno Setup 7`; set `INNO_COMPILER` explicitly for another path:

```powershell
cmake --preset vs2022-x64 -DDARKMODELIB_ROOT="C:/src/darkmodelib" -DINNO_COMPILER="C:/Program Files/Inno Setup 7/ISCC.exe"
cmake --build --preset release --target installer
```

The development environment used MSVC 19.44, Windows SDK 10.0.26100.0 and Inno
Setup 7.1.1. These are the tested versions, not a claim that every other toolchain
combination has been validated. Windows system libraries provide networking,
image decoding, graphics and text rendering.

## Tests and diagnostics

Version 1.0.0 passes 63 automated tests. Regression tests use deterministic
fixtures and a local loopback HTTP server rather than live publisher feeds.
They cover policies, parser input limits, URL/authentication rules, dates,
filters, settings recovery, OPML, caching, decoding and rendering. The installer
target depends on the test target when `BUILD_TESTING` is enabled, as it is in
the supplied preset.

The native probe uses the same downloader and feed parser as the application:

```powershell
build/Release/mrss_probe.exe "https://peapix.com/spotlight/feed"
build/Release/mrss_probe.exe --opml "C:/feeds/example.opml"
```

The first command fetches and reports feed metadata. The `--opml` command is an
offline parser/import check: it does not save settings or fetch feed URLs.
Probe diagnostics redact URL paths and query strings to avoid exposing tokens.

For UI checks, use an isolated settings/cache directory:

```powershell
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = (Resolve-Path "./build/bin/Release/mRSSScreenSaver.scr").Path
$startInfo.Arguments = "/c /data:C:/temp/mrss-ui-test"
$startInfo.UseShellExecute = $false
[System.Diagnostics.Process]::Start($startInfo) | Out-Null
```

The `/data:` argument must be an absolute directory. It prevents a development
run from altering normal user settings or cache data. Tests do not select the
screensaver as the Windows default or change Windows security settings.

See [TESTING.md](TESTING.md) for the manual acceptance checklist. Clean-machine
installation, upgrade/uninstall, optional codec availability, mixed-DPI monitors
and actual HDR photographs remain important acceptance checks. The release is
unsigned because no signing certificate was supplied.

## Implementation overview

| Area | Main files |
| --- | --- |
| Windows dialogs, preview and screensaver lifecycle | [main.cpp](../src/main.cpp) |
| RSS/Atom/mRSS and OPML parsing | [feed.cpp](../src/feed.cpp) |
| JSON Feed and Peapix parsing | [json_feed.cpp](../src/json_feed.cpp) |
| Network and destination validation | [network.cpp](../src/network.cpp) |
| Eligibility, catalogues and cache handling | [engine.cpp](../src/engine.cpp) |
| Dates, URLs, image signatures and playlist policies | [policy.cpp](../src/policy.cpp) |
| Boolean and bounded regex filtering | [filter.cpp](../src/filter.cpp) |
| Settings, recovery, overrides and serialization | [settings.cpp](../src/settings.cpp) |
| WIC decoding, Direct2D/DXGI rendering and DirectWrite captions | [graphics.cpp](../src/graphics.cpp) |
| Windows version/icon resources | [app.rc](../src/app.rc), [app.manifest](../src/app.manifest) |
| Inno Setup packaging | [mRSSScreenSaver.iss](../installer/mRSSScreenSaver.iss) |

The default theme follows Windows. darkmodelib themes settings, auxiliary
dialogs and the colour picker. Feed actions and dependent controls are disabled
when they do not apply. The installer offers a Start menu folder, all-user
shortcuts and optional selection as the current user's screensaver.

## Defaults and compatibility

| Setting | Default |
| --- | --- |
| Image display time | 10 seconds |
| Transition duration | 2 seconds |
| Transition | Cross-fade |
| Feed refresh interval | 24 hours while running |
| Maximum image age | 7 days |
| Cache limit | 50 MiB |
| Maximum simultaneous downloads | 4 |
| Mature content | Off; global only |
| Metadata captions | Off; regular text |
| Per-feed overrides | Off unless explicitly enabled |

Global maximum age uses calendar-month calculations where appropriate. A feed
may override image duration, age options or the complete metadata-caption style.
Existing explicitly saved overrides are retained. Legacy imports without an
override flag inherit global settings.

Transition presentation order is separate from persisted enum values, so moving
None to the top of the list did not change saved selections. Random effects
have independent selection bits; older saved selections remain valid. Pan &
zoom runs through the image display interval, and a single-image collection does
not repeatedly transition to the same image. Windows' disabled-animation
preference takes precedence.
Unbalanced random playback uses no-repeat shuffle cycles and avoids an immediate
repeat at cycle boundaries.

## Parser and network behaviour

- RSS/Atom parsing is namespace-aware. XML DTDs and external entities are
  prohibited, with bounds on input size, depth, node count and text.
- JSON Feed supports explicit item images and image attachments. It does not
  scrape `content_html`, use decorative `banner_image` fields, or follow
  `next_url` pagination automatically.
- Peapix arrays are recognised only at its known Spotlight/Bing endpoints.
  `fullUrl` is preferred, falling back to `imageUrl`; thumbnails are not added
  as separate slideshow items. Copyright text supplies the credit field.
- JSON `date_published` determines age; `date_modified` does not make an older
  image new. Peapix Bing calendar dates use UTC midnight. Missing Spotlight
  dates stay missing rather than being replaced with the download time.
- JSON Feed and Peapix have no standard maturity rating, so their images are
  treated as unrated. mRSS ratings and credits follow their metadata scopes;
  Dublin Core creator and standard RSS/Atom authors provide author fallbacks.
- Image signatures are an initial gate. Actual decoder/container validation
  still applies; MIME types and file extensions alone are not trusted. Only
  approved still-image decoders are used.
- Network requests suppress authentication, cookies and host credentials.
  Redirects and resolved destinations are validated, including private-network
  restrictions. Literal URL exclusions also apply to redirected destinations.
  WinHTTP uses direct connections rather than automatic system proxy settings.
- Conditional requests, cancellation and download limits reduce repeat work.
  Successful checks and cached-body timestamps are recorded separately. A 304
  response updates the former without replacing the latter. Older metadata
  recovers cache time from the cached feed file's modification time.

The shared parser/eligibility path is used by settings checks, playback and
preview. Settings starts an explicit cache-aware check; imports, feed edits and
Refresh / Retry force a check. There is no background refresh service.

## Settings, OPML and metadata captions

Settings backups include the full feed URLs, global settings, per-feed options,
exclusions and advanced filters. They do not include downloaded images. Settings
are written atomically, with recovery from the last saved backup where possible.

OPML supports nested feed lists, duplicate URL detection and a namespaced
per-feed settings extension. Other OPML readers may discard that extension, so
use a full settings backup when preserving the complete configuration matters.
Import preserves feed URLs and detects their formats when fetched. Private/LAN
allowances require explicit review after import.

An earlier importer incorrectly reported “Too many feeds” because embedded JSON
was wrapped in the wrong shape. Regression coverage now distinguishes malformed
feed arrays from the actual 1,000-feed limit.

Metadata captions use the effective global or per-feed style. The duration is
capped to the effective image display time and starts after a transition.
Playback pauses freeze the caption countdown. Missing fields are omitted;
control characters, text length and layout overflow are handled before drawing.
DirectWrite layouts are cached and invalidated when text, font, size or style
changes. The white text and translucent black panel provide consistent contrast
without analysing image pixels each frame.

## Live-feed observations

Live feeds are useful smoke tests, not reliable regression fixtures. Flickr and
both Peapix endpoints returned usable image entries during development. The
original Bing RSS endpoint returned RSS without supported image entries.
DeviantArt results varied: the installed app fetched feeds with the VPN disabled,
while separate probe requests returned 403. A 403 alone
does not establish that a feed requires authentication or contains mature images.

For detailed requirements and processing limits, see
[SPECIFICATION.md](SPECIFICATION.md). For dependency source revisions and notices,
see [DEPENDENCIES.md](DEPENDENCIES.md).
