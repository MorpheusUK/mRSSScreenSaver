# mRSS Screen Saver

Windows 11 x64 C++20/Win32 screensaver, with darkmodelib settings and an Inno
Setup 7 dynamic light/dark installer. Version 1.0.0.

- Installer: `build/dist/mRSSScreenSaver-1.0.0-x64-setup.exe`
- Screensaver: `build/bin/Release/mRSSScreenSaver.scr`
- [Testing guide](docs/TESTING.md)
- [Agreed specification](docs/SPECIFICATION.md)

© 2026 MorpheusUK. All Rights Reserved. Original project code is licensed under
[GNU GPL version 3.0 only](LICENSE) (GPL-3.0-only); third-party dependencies keep
their own licenses. See [NOTICE.md](NOTICE.md).

## Build and test

Requirements: Visual Studio 2022 Desktop development with C++, Windows SDK,
bundled CMake. Open this folder in Visual Studio, or use its CMake executable:

```
cmake --preset vs2022-x64
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target installer
```

Configuration generates `build/mRSSScreenSaver.sln`. Dependencies are pinned:
GoogleTest 1.17.0, PCRE2 10.46 and nlohmann/json 3.12.0. Existing `third_party`
checkouts are used; otherwise CMake fetches the recorded commits. Regression
tests use local fixtures and loopback networking, not internet feed content.

`DARKMODELIB_ROOT` defaults to the supplied `I:/darkmodelib` checkout and can be
overridden during configuration. External source directories are not modified.
Inno Setup is detected at `C:/Program Files/Inno Setup 7`; set `INNO_COMPILER` if
needed. Build artifacts stay in `build`. Tests do not select a screensaver or
change Windows security settings.

## Current implementation

- Native C++ policy library: defaults, explicit timing overrides, HTTPS/HTTP URL
  validation, credential rejection, URL redaction and literal exclusions.
- Global maximum image age with explicit per-feed overrides, calendar-month
  calculations, date eligibility and global rating policy.
- Allowlisted image signature identification (a first gate, not a decoder).
- Deterministic no-repeat shuffle cycles.
- Bounded namespace-aware RSS 2.0/Atom parsing, mRSS rating inheritance and
  image-enclosure extraction; DTDs and external entities prohibited.
- Automatic JSON Feed 1.0/1.1 and Peapix Spotlight/Bing parsing, using explicit
  image fields and image attachments with the same filtering/cache/decoder path.
- Standard nested OPML import/export with XML escaping and duplicate detection.
- GoogleTest regression coverage and Visual Studio x64 build/test presets.
- Themed native settings, feed CRUD/order/status, duration overrides, OPML
  settings extensions and full settings backups.
- WinHTTP downloads, redirect/destination checks, credential suppression,
  conditional requests, cancellation and bounded per-user caching.
- Microsoft WIC still-image decoding and Direct2D/DXGI rendering, transitions,
  Fit/Fill, a brightness-limited HDR path and multi-monitor mirroring.
- Feed slideshow preview with Start/Previous/Play-Pause/Next/End, sequential and
  shuffled playback, plus optional feed balancing.
- Native `.scr` lifecycle and an installer with optional current-user selection.
- Themed codec/confirmation/error dialogs and colour picker; feed actions and
  dependent settings are disabled when they do not apply.
- Themed About dialog with the build version, author/copyright and license.

New feeds and legacy imports inherit global age and display-time settings unless
an override is explicitly enabled. OPML imports with embedded per-feed JSON now
use an explicit array and report malformed settings separately from feed limits.

The default transition duration is two seconds. Existing saved durations are
preserved. The installer offers a Start menu folder choice and installs shortcuts
for all users. The app and installer share an original landscape/RSS icon.
Backup/Restore saves and loads the complete configuration, including feeds,
exclusions and advanced filters, without the downloaded image cache.

Transitions also include a perspective rotating cube, a six-piece collage
assembly, and pan & zoom with a cross-fade. Pan & zoom continues gently during
the image display interval and pauses with slideshow playback. Random mode has
individual checkboxes for all seven effects; existing saved selections remain
unchanged. Windows' disabled-animation preference still takes precedence.
None is listed first without changing stored transition IDs. Settings performs
an explicit cache-aware startup check; imports and Refresh / Retry force checks.
The feed list has a separate Eligible (eligible/total) column before Status.
The URL column is narrow by default to leave room for availability, last-success
and cached-body timestamps in local time. Columns remain resizable, with
horizontal scrolling for longer text. A successful 304 check advances last
success while preserving the cached body's timestamp.

Developer diagnostic: `build/Release/mrss_probe.exe --opml <file>` validates the
actual parser/import path offline without saving settings or fetching URLs.

The default appearance follows Windows. JPEG, PNG, GIF, BMP, TIFF, WebP and
HEIC/HEIF are allowlisted; optional formats depend on installed Microsoft codecs.
No HTML scraping, videos, documents, background services or host credentials.

The global Metadata button configures optional image title and/or author/credit
captions. Each feed can explicitly override those options (off by default).
Choose an installed font family, 8–96 point size, top/bottom centre and a display
period; the period is capped to that feed's effective image display time. Captions
start after transitions and pause their countdown with playback. White text
uses a translucent black panel; missing fields are omitted and long lines are
trimmed. Options are included in backups and per-feed OPML settings extensions.
Bold and Italic are independent options, both off by default; older settings
retain regular text. Styles follow the same global/per-feed override rules.
mRSS media:credit inheritance and Dublin Core creator fields supply author data
alongside RSS/Atom and JSON Feed authors; Peapix supplies its credit text.

Peapix feeds can be added directly:
`https://peapix.com/spotlight/feed` and
`https://peapix.com/bing/feed?country=gb`. Spotlight currently omits dates, so
enable Include undated items (or Entire feed). Bing's calendar dates are treated
as UTC midnight. JSON Feed uses date_published; date_modified does not make an
older image new. These formats have no standard maturity rating, so images are
unrated and are excluded if Exclude unrated / unknown is enabled. Peapix prefers
fullUrl, then imageUrl; it never uses thumbnails as additional slideshow items.
JSON Feed uses item.image and image attachments, without scraping content_html,
following next_url, or treating decorative banner_image as a slideshow image.

Release builds use MSVC 19.44 and Windows SDK 10.0.26100.0. The installer compiles
with Inno Setup 7.1.1. Visual settings/preview checks use isolated workspace data.

Clean-machine installation, upgrade/uninstall, optional codecs, mixed-DPI monitors
and actual HDR photographs remain acceptance checks. This build is unsigned
because no signing certificate was supplied. See the testing guide for HDR scope
and current live-feed limitations.
