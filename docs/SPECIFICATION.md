# mRSS Screen Saver specification

Status: agreed requirements; implementation in progress. This document records
the discussion. It does not claim the features below are already implemented.

## Platform and delivery

- Native C++20, Win32 and Windows SDK, Windows 11 x64.
- Visual Studio 2022 Community and CMake-generated Visual Studio solution.
- darkmodelib settings UI: follow system, light, dark; high contrast, keyboard
  access, resize and per-monitor DPI support.
- Development dependency: I:/darkmodelib, revision
  e8ec95ba631462f01e238e63f7d06c1e6829a7a1. Dependency path must be configurable.
- Inno Setup 7 at C:/Program Files/Inno Setup 7; WizardStyle=modern dynamic.
- Native screen-saver discovery and /s, /c, /p argument forms, including colon
  forms and 64-bit parent HWNDs. Settings/embedded preview/feed previews may
  coexist; one full-screen instance per user/session.
- Install with protected executable/DLL locations. Offer optional current-user
  selection and opening Windows Screen Saver Settings. Preserve timeout,
  lock-on-resume and policy settings. Never weaken Windows security or sleep.
- Upgrades preserve data; uninstall only changes selection if still ours.
- No background service, scheduled task, startup task or resident downloader.
- Code-sign release artifacts when a certificate is available. Include licenses
  and source availability for applicable dependency licenses.

## Defaults and efficiency

| Setting | Default |
| --- | --- |
| Fully visible image duration | 10 seconds |
| Transition duration | 2 seconds |
| Feed refresh interval | 24 hours |
| Maximum image age | 7 days |
| Disk image cache | 50 MiB |
| Simultaneous downloads | at most 4, at most 2 per server |
| Prefetch | next 2 images |
| Mature content | off, global only |
| Transport | HTTPS only; per-feed Allow HTTP override |

Use conditional requests, retry backoff, bounded buffers, resolution-appropriate
variants and LRU eviction. Separate decoded-memory and per-download limits from
the disk-cache limit. Static images need no continuous render loop. Suspend
downloads/rendering when displays are off; cancel work promptly on exit.
Refresh only while Settings, Preview or the screensaver is running, and provide
manual Refresh. Avoid duplicate concurrent refreshes.

## Feeds and UI

- Ordered, enabled/disabled feed list with title and URL. Add/edit/remove,
  move up/down, refresh, preview, OPML import/export.
- Asynchronous validation; a temporarily unavailable feed may still be saved.
- Status icons plus text: available, invalid URL, unreachable, invalid feed,
  authentication required, offline, checking. Last check/success, details, Retry.
  Failed requests alone do not establish an offline computer.
- Global maximum image age with an explicit per-feed override switch; new feeds
  inherit the global setting. Age value and minutes/hours/days/weeks/months. Entire feed disables the value
  and unit. Entire feed means the items supplied, not an archive crawl.
- Calendar-month arithmetic clamps month-end dates. UTC comparisons; reject
  implausibly future dates (five-minute skew allowed). Missing dates excluded
  under age limits unless Include undated is enabled.
- Global defaults apply unless an explicit per-feed override is provided.
  Image duration supports a Use global checkbox; maximum age supports an explicit
  local override checkbox; mature content is global only.
  Mandatory security boundaries are never overridden.
- Global and per-feed additive URL substring exclusions, one literal per line,
  case-insensitive by default; blur_ is an example, not a built-in rule.
  Apply before requests and to redirects; never rewrite publisher URLs.
- Advanced filters: Boolean AND/OR/NOT, quoted phrases and parentheses;
  precedence NOT > AND > OR. Separate bounded regex mode, includes/excludes,
  case option. Search selected metadata fields, not pixels. Test matches and
  rejection reasons. No unbounded std::regex execution on untrusted input.
- Apply/OK/Cancel; Restore Defaults; versioned atomic settings, last-known-good
  backup, migrations and visible recovery reports without losing the feed list.
- Local overrides default off, including legacy OPML records without explicit
  override flags. An About dialog shows version, MorpheusUK copyright and GPL-3.0.
- Clear Cache. Removing/disabling feeds immediately removes eligible queued
  images; remove cache objects without other feed references. Recheck all
  eligibility gates before every display, including cached images.

## Accepted content and parser

- RSS 2.0 and Atom with mRSS, plus image enclosures in those feeds.
- JSON Feed 1.0/1.1 explicit item images and image attachments. Retain title,
  content metadata, author, tags and date_published; no HTML scraping or automatic
  pagination. Missing/invalid published dates remain undated. Unrated by default.
- Peapix arrays only at peapix.com/spotlight/feed and peapix.com/bing/feed (query
  parameters retained). Prefer fullUrl then imageUrl; retain title/copyright.
  Bing calendar dates use UTC midnight; undated Spotlight items remain undated.
  Apply identical networking, ratings, age, filters, caching and decoder limits.
- JSON input: 8 MiB, depth 64, 100000 parse events, 1 MiB strings, 10000 items/
  images and 16 MiB extracted text. Reject duplicate keys, unknown JSON schemas
  and unsupported JSON Feed versions; sanitise parser diagnostics.
- No scraping HTML descriptions or linked pages. Treat metadata as plain text,
  never executable HTML. No browser engine or active content.
- Allowlist: JPEG, PNG, GIF first frame, BMP, TIFF first page, WebP first frame,
  HEIC/HEIF primary still image using approved installed codecs.
- No video/audio/documents/executables/SVG/AVIF or arbitrary installed codecs.
  An HEVC still-image decoder does not enable video playback.
- Validate signatures and actual decoder/container output; suffix/MIME alone
  are insufficient. Apply size, dimension, decoded-memory and processing limits.
- Select suitable resolution among alternatives; play distinct image attachments
  and avoid duplicate thumbnails/full-resolution variants where identifiable.
- Preserve URLs including signed query parameters; do not strip them for
  deduplication. Retain provenance if an image is shared by multiple feeds.
- Apply mRSS metadata inheritance: media:content > media:group > item > channel.
  Recognise documented rating schemes and legacy media:adult. Missing/unknown
  ratings are distinct and subject to Exclude unrated. Ratings and URL strings
  cannot guarantee suitability. Never download rejected content for thumbnails.
- Safe processing sequence: metadata inheritance, age/rating/metadata gates,
  URL exclusions, network destination policy, bounded download, safe decode.

## Playback and preview

- Sequential: feed-list order, then feed-published item order.
- Random: shuffled eligible images across feeds, no repeat within a cycle or
  boundary repeat when more than one image exists. Optional equal-feed weighting.
- Refresh playlist at image boundaries, preserving shuffle progress where possible.
- Transitions: fade through background, slide, cross-fade, flip, random from
  selected effects, none. Honour reduced-animation preferences.
- Fit default, Fill/Crop optional, orientation/profile/transparency support and
  configurable background. Initially mirror image across monitors.
- Preserve supported HDR/wide-gamut metadata; HDR per monitor when enabled,
  tone-map for SDR; bounded HDR brightness and avoid abrupt brightness changes.
  Detect codecs and unsupported variants; diagnostics in Settings only.
- Empty eligible collections use a quiet fallback and backoff; never relax rules.
- Feed Preview is resizable, optional fullscreen, and uses unsaved settings:
  Start/Previous/Play-Pause/Next/End, counter, title/source, exclusion summary,
  keyboard shortcuts, loading/error/empty states. Stable order; manual navigation
  pauses. Start/End means first/last eligible image. Fetch on demand, cancel on
  close; do not download the entire feed. Do not save or select the screensaver.
- Actual screensaver exits promptly on input; embedded Windows preview uses
  cache and avoids repeated network requests. Handle sleep/resume, display
  changes, Remote Desktop and graphics-device loss.

## Network and local security

- Public feeds and images only. Embedded URL tokens/signatures allowed.
- Never provide host credentials: no explicit logins, Windows auth, browser
  cookies, credential store, OAuth, client certs or username:password@host URLs.
- HTTPS default; explicit per-feed Allow HTTP applies to feed and media URLs
  and is required for HTTPS-to-HTTP redirects. Never ignore TLS validation.
- Revalidate every redirect and resolved destination. Block loopback, private,
  link-local and other special-use networks by default; explicit per-feed LAN
  allowance only. Bound redirects and defend against resolution/address changes.
- No cross-origin credential/header forwarding. Tokens stay on their supplied
  URL; do not copy query parameters onto other requests.
- Redact sensitive URLs in logs and warn about tokens in OPML exports. Store
  private data per user, safe generated cache names, protected executables and
  DLL search policy. No DLL/plugin loading from cache/current directory.
- Prohibit XML DTDs/entities and bound feed/OPML size, nesting and text lengths.
  Use bounded regex execution; consider restricted decoding worker.
- Build with applicable compiler/linker exploit protections.
- Bitdefender is present: ordinary native build/test tooling only; no encoded
  scripts, antivirus bypasses, exclusions or security-setting changes. Report
  blocks and investigate without evasion.

## OPML and tests

- Standard OPML URLs/titles/nesting; preserve order, detect duplicates, report
  invalid entries. Import preview, Merge/Replace. Namespaced settings extensions;
  separate full-settings backup because other readers may discard extensions.
- GoogleTest pinned to v1.17.0, commit
  52eb8108c5bdec04579160ae17225d66034bd723. Deterministic clocks/random seeds,
  local fixtures and mocked networking; no internet required for unit tests.
- Test parser inheritance, ratings, filters, ages, URLs/redirect/auth policy,
  settings/migration/recovery, OPML, cache references/eviction and playlists.
- Integration: local HTTP server fixtures, real decoding, missing codecs,
  malformed/large content and per-format/HDR fixtures. Manual: Windows lifecycle,
  installation/upgrade/uninstall, accessibility, DPI, HDR and multi-monitor.
- Separate CPU/memory/download performance checks. Release builds run tests.

Additional transitions: perspective rotating cube; a six-piece tile collage
assembly; pan & zoom during the display interval with a cross-fade between
images. All participate in the selectable Random effect set. Preserve existing
numeric transition IDs and saved Random selections. Undated inclusion remains
an age option, independent of content ratings.

Metadata captions: global settings with explicit optional per-feed overrides,
default off. Select title, author/credit or both; font family, size 8–96 points,
top/bottom centre and duration 1–86400 seconds clamped to effective image duration.
Use white text with a translucent black background and safe margins; omit
missing fields, bound text and trim overflow. Captions begin after transitions
and their countdown pauses with preview playback. Cache DirectWrite layouts;
redraw static images only when caption visibility changes. Persist global and
per-feed options in backups and per-feed OPML extensions. Resolve media:credit
from content/group/item/channel, falling back to standard authors/DC creator.
Independent Bold and Italic options default off and participate in global/per-feed
inheritance, settings backup and OPML. Existing metadata options stay regular.

Live candidate feeds (structure/access/content unverified until fetched):

1. DeviantArt glamour: potentially adult; photography portrait glamour query.
2. DeviantArt architecture: expected SFW; photography architecture query.
3. Flickr monument RSS2: HTTP policy and another publisher.
4. Bing HPImageArchive RSS, n=1: expected single-item collection.

windows10spotlight.com/feed is unverified and excluded from confirmed fixtures.
Live results are not deterministic; save sanitised metadata fixtures separately.

