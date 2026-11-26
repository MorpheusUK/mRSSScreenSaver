# Testing mRSS Screen Saver 1.0.0

Version 1.0.0 is unsigned because no signing certificate was supplied.
The installer does not alter antivirus or lock-on-resume settings.
If Windows or Bitdefender blocks it, report the exact message; do not disable
protection or add an exclusion to run it.

## Start

Install the x64 setup executable. Administrative permission is needed to place
the screen saver in Windows' native discovery directory. Making it current is
an unchecked installer option and applies to the original interactive account.
Alternatively, use the built `.scr` file's Configure action without installing.

Windows Screen Saver Settings should list **mRSS Screen Saver**. Select it and
click Settings. The default appearance is **Follow Windows**. Settings and cache
are under `%LOCALAPPDATA%\mRSSScreenSaver` and are retained by uninstall.

## Suggested checks

1. Add a public HTTPS feed. Check its fetched title and status; Preview it.
2. Set the global maximum image age; Entire feed disables its numeric/unit fields.
   New feeds inherit this setting. Enable Override global maximum image age in a
   feed's settings to use its own limit (including its own Entire feed option).
3. Test Start, Previous, Play/Pause, Next and End; manual navigation pauses.
4. Add another feed; change order and toggle its enabled checkbox.
5. Test sequential and random playback, timing overrides and transition effects.
6. Add a URL substring exclusion. Preview should omit matching image URLs.
7. Test Boolean/regex metadata rules using the filter Test field.
8. Export OPML and reimport it. Avoid sharing exports containing access tokens.
9. Test light/dark Windows appearance and explicit appearance overrides.
10. Use Windows' embedded preview and full-screen Preview. Fullscreen mode exits
    on input, and must not affect Windows lock-on-resume or monitor sleep.

New feeds and legacy OPML imports default to global age and display-time settings.
Local age overrides require an explicit ageOverride flag or checkbox selection;
the presence of legacy age fields alone does not enable an override. Explicit
overrides already saved by a user or an earlier release are retained. Uncheck
Override global maximum image age on an existing feed to inherit the global
limit. Apply or OK saves the change with the normal settings backup.

Version 1.0.0 fixes embedded-settings OPML imports that incorrectly reported
"Too many feeds" even for one feed. The actual 1000-feed limit remains. Test
Merge and Replace with the corrected 35-entry OPML: it should import 34 unique
feeds, with legacy overrides off unless explicitly supplied.

About should show version 1.0.0, © 2026 MorpheusUK. All Rights Reserved., and
GPL-3.0-only. The installer includes the full GPL license and third-party notices.

Check that an empty list disables Edit, Remove, Up, Down, Preview, Refresh and
Export. With a selected feed, Up/Down should respect its position in the list.
Codec status, colour selection and confirmation/error dialogs should follow the
selected appearance. Follow Windows remains the default.

Mature imagery is disabled globally by default. Feed labels/search terms do not
guarantee suitability. Unrated images are permitted by default; use Exclude
unrated / unknown if wanted. No pixel-based safety classification is performed.

Codec status reports HEIF/WebP decoder availability. The app uses allowlisted
Microsoft WIC decoders and will skip unsupported files. HEIC may need Microsoft
HEIF and HEVC extensions. Only still frames are shown. HDR output needs an HDR
monitor with Windows HDR enabled; use actual HDR photos to validate your display.
The current HDR path recognises floating-point decoder output. Manufacturer
gain maps and non-floating-point HDR10 representations are not yet verified and
may display as SDR. Actual HDR/HEIF/WebP samples remain an acceptance-test area.

The installer has been compiled, but has not been installed on this workstation
during development. Check picker discovery, optional selection and uninstall as
part of acceptance testing. When uninstall is elevated under a different
administrator account, per-user selection cleanup cannot be assumed for other
accounts; select another saver in the affected account before uninstalling.

Public internet connections are direct. Authenticated proxies, website logins,
browser cookies, host credentials and client certificates are not supported.
HTTP requires an explicit per-feed allowance. LAN feeds need a separate explicit
private-network allowance. Signed URLs already supplied by the feed are allowed.

## Candidate feeds supplied during planning

- https://backend.deviantart.com/rss.xml?type=deviation&q=photography+architecture+-cartoon+-ai+sort%3Atime
- https://backend.deviantart.com/rss.xml?type=deviation&q=photography+portrait+glamour+-cartoon+-drawing+-sketch+-adoptable+-oc+-ai+sort%3Atime
- https://api.flickr.com/services/feeds/photos_public.gne?tags=monument&format=rss2
- https://www.bing.com/HPImageArchive.aspx?format=rss&idx=0&n=1&mkt=en-US

Availability, ratings and direct image support depend on the publisher. Plain
RSS with HTML-only images will show no eligible images; the app does not scrape
HTML. A single-image feed will not repeatedly transition to the same image.
Live development checks: Flickr returned usable image entries; DeviantArt
returned HTTP 403; Bing returned valid RSS with no supported image entries.
These results can change. Begin with Flickr rather than the other candidates.

## Report problems

Version 1.0.0 adds independent Bold and Italic checkboxes to Metadata globally
and per feed. Verify all four combinations, including after changing styles on
the same text, and that both controls grey out when neither field is enabled.
Settings/OPML tests cover style preservation and legacy regular-weight defaults;
renderer checks cover all four combinations and cached layout invalidation.

Version 1.0.0 adds Metadata globally and an unchecked-by-default per-feed
override. Test title-only, author-only, both, neither, missing fields, installed
and unavailable fonts, 8/24/96 point sizes, long text and Unicode. Verify top and
bottom centre, expiry without changing the image, paused countdown and duration
capping to a shorter per-feed image interval. Check static and moving effects,
HDR/SDR and multiple monitors. Options must round-trip settings backups and OPML.
Isolated GUI checks exercised the themed dialogs, override greying, bottom-centre
Segoe UI captions, top-centre Arial captions, and a two-second caption expiring
while the same image remained displayed. Renderer tests cover both positions,
font sizes, overflow, Unicode and missing-font fallback.

Version 1.0.0 adds JSON Feed 1.0/1.1 and Peapix Spotlight/Bing JSON. Test both
Peapix URLs listed in README. Spotlight requires Include undated or Entire feed;
both formats are unrated and respect Exclude unrated / unknown. JSON Feed tests
cover explicit images/attachments, author inheritance, dates, tags, unsafe URLs,
non-image media, missing IDs, depth/BOM/duplicate keys and shared cache/filter/
decoder behavior. OPML preserves these feed URLs; format detection is automatic.

Version 1.0.0 separates Eligible (eligible/total) from Status, narrows the default
URL column, and shows last-success and cached-body timestamps in local time.
Check these fit the default window and that both timestamps remain accurate
after a 304 response or failed refresh using cached data. The cache timestamp
for older metadata is recovered from the cached feed file's modification time.

Version 1.0.0 adds Rotating cube, Collage (six tiles assemble into the next
photo), and Pan & zoom. Exercise each in preview and fullscreen, both Fit and
Fill, and through Random with only that effect selected. Pan & zoom should move
through the display interval and hold still when playback is paused. Existing
transition and Random choices must survive upgrade. Include undated items stays
beside the age settings. Rendering tests cover transition endpoints and interior
frames for both image layouts.

For version 1.0.0, verify that Setup offers the Start menu folder page and that
shortcuts are visible from a standard user account. Check the landscape/RSS icon
on Setup, the screensaver file and settings window. New settings and Restore
Defaults should use a two-second transition; saved values remain unchanged.

Include the feed's public URL (remove tokens), displayed status, relevant settings,
Windows version, monitor arrangement and codec status. For a visual problem,
include whether Windows HDR was enabled and which transition/layout was used.

## Developer checks

Use the VS2022 x64 CMake preset, then the release build and test presets. Tests use
isolated workspace data; live requests are separate from deterministic tests.
The developer `/data:<absolute-directory>` argument isolates all app settings and
cache when exercising the UI without changing normal user data.
