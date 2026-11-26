# mRSS Screen Saver

A Windows 11 screensaver that turns online image feeds into a photo slideshow.
Add your favourite public feeds, choose how the pictures are displayed, and let
the screensaver do the rest.

It supports Media RSS, RSS and Atom image feeds, JSON Feed, and Peapix's Windows
Spotlight and Bing image feeds. Images are cached locally for reuse.

**Version 1.0.0 · Windows 11 x64 · C++/Win32 · GPL-3.0-only**

## Features

- **Manage multiple feeds:** add, edit, remove and reorder feeds, with titles,
  eligible-image counts, availability and local check/cache timestamps.
- **Choose your playback:** sequential or random images, with an option to
  balance the selection between feeds.
- **Control the presentation:** fit or fill the screen, choose a background
  colour, and set image and transition timings.
- **Pick a transition:** none, fade, slide, cross-fade, flip, rotating cube,
  collage, or pan & zoom. Random mode lets you choose which effects to include.
- **Show image information:** optional title and author/credit captions, with
  font, size, bold/italic, top/bottom positioning and a timed display.
- **Filter your images:** maximum image age, mature/unrated content settings,
  simple URL exclusions, and advanced Boolean or regular-expression filters.
- **Use global settings or per-feed overrides** for image duration, maximum age
  and metadata captions. New feeds inherit the global settings by default.
- **Preview a feed** with Start, Previous, Play/Pause, Next and End controls.
- **Import and export OPML feed lists**, or back up and restore all settings.
- **Follow the Windows theme**, or select light or dark mode yourself.
- **Show the same slideshow on multiple monitors**, with HDR brightness controls
  for supported images and displays.

## Install and get started

1. Run `mRSSScreenSaver-1.0.0-x64-setup.exe`. If you are building from source,
   see [Building](#building) below for how to create the installer.
2. Choose a Start menu folder. You can also choose to make mRSS Screen Saver
   your current screensaver during installation.
3. Open **mRSS Screen Saver → Screen Saver Settings** from the Start menu.
4. Click **Add…** to enter a feed URL, or **Import OPML…** to load a feed list.
5. Select a feed and click **Preview…** to check its images.
6. Adjust the global settings, then click **Apply** or **OK** to save.

The installer registers the screensaver in Windows' Screen Saver Settings.
Administrator permission is required for installation. The installed executable
is `C:\Windows\System32\mRSSScreenSaver.scr`.

## Feed and image support

| Feed format | Supported image sources |
| --- | --- |
| RSS 2.0 and Atom | Media RSS image entries and image enclosures |
| JSON Feed 1.0 / 1.1 | Item images and image attachments |
| Peapix JSON | Windows Spotlight and Bing image feeds |

Only still images are displayed: **JPEG, PNG, GIF, BMP, TIFF, WebP and
HEIC/HEIF**. WebP and HEIC/HEIF depend on compatible Windows image codecs being
installed; **Codec status** shows which optional decoders are available.
Animated and multi-page files use their first frame, first page or primary still
image, as appropriate. Videos, documents and executables are not supported.

The app uses image links supplied by the feed. It does not scrape images from
HTML descriptions or linked web pages. Feeds requiring a login are not supported:
the app does not supply browser cookies or Windows credentials. URLs containing
access tokens are supported. HTTPS is the default; HTTP can be enabled per feed.

### Example feeds

- [Flickr: photos tagged monument](https://api.flickr.com/services/feeds/photos_public.gne?tags=monument&format=rss2)
- [Peapix: Windows Spotlight](https://peapix.com/spotlight/feed)
- [Peapix: Bing image of the day, United Kingdom](https://peapix.com/bing/feed?country=gb)

Peapix Spotlight currently supplies no publication dates, so enable **Include
undated items** or **Entire feed** for it. Peapix's feeds are treated as unrated;
leave **Exclude unrated / unknown** off if you want their images displayed.
Feed availability and contents are controlled by their publishers.

## Settings worth knowing

**Feed checks and caching.** Feeds are checked when the app opens, using the cache
where it is still fresh. Imports and added/edited feeds trigger a check, and
**Refresh / Retry** forces a new check. The refresh interval defaults to 24 hours
while the app is open. No background checks run after it closes. The status shows
the last successful check and when the cached feed contents were downloaded;
those times can differ if the server reports that nothing has changed.

**Image age and content filters.** The default maximum age is seven days.
**Entire feed** removes the age restriction; otherwise **Include undated items**
decides whether images without dates are eligible. Mature images are disabled by
default, and the mature-content setting is global. Mature and unrated filtering
depend on the ratings supplied by the feed.

**Metadata captions.** Click **Metadata…** beside Global settings to show the
title, author/credit, or both. Captions start after the transition, and their
display period is capped to the image's effective duration. Text is white on a
translucent black panel, with regular text as the default. To customise one feed,
open **Edit…**, enable **Override global metadata**, then click **Metadata…**.

**OPML versus settings backups.** OPML is for moving or sharing feed lists.
**Backup… / Restore…** includes the complete configuration, including global
settings, per-feed overrides, exclusions and advanced filters. Cached images are
not included. After restoring a backup, click **Apply** or **OK** to save it.

## Building

### Requirements

- Windows with **Visual Studio 2022**, including the **Desktop development with
  C++** workload and a Windows SDK.
- **CMake 3.24 or later** and **Git**, available on your command line.
- A separate [darkmodelib](https://github.com/ozone10/darkmodelib) source checkout.
- **Inno Setup 7** if you want to build the installer.

See [Dependency revisions](docs/DEPENDENCIES.md) for the tested dependency
versions and licenses. CMake downloads the pinned GoogleTest, PCRE2 and
nlohmann/json sources if they are not already present.

### Compile and test

From the project folder in a Visual Studio 2022 Developer PowerShell, configure
the x64 build with the path to your darkmodelib checkout:

```powershell
cmake --preset vs2022-x64 -DDARKMODELIB_ROOT="C:/src/darkmodelib"
cmake --build --preset release
ctest --preset release
```

Replace `C:/src/darkmodelib` with your actual path. The screensaver is written to
`build/bin/Release/mRSSScreenSaver.scr`.

To create the installer:

```powershell
cmake --build --preset release --target installer
```

The installer is written to
`build/dist/mRSSScreenSaver-1.0.0-x64-setup.exe`. This target also runs the tests.
If Inno Setup is not detected, set `INNO_COMPILER` to the full path of `ISCC.exe`
when configuring. See the [Developer guide](docs/DEVELOPMENT.md) for more build
options, diagnostics and implementation details.

## Troubleshooting

- **Available, but no eligible images?** Check the age limit, undated setting,
  ratings, URL exclusions and advanced filters. The Eligible column shows how
  many images pass these rules out of the total found in the feed.
- **A feed works in a browser but fails in the app?** A browser may be using a
  logged-in session. A publisher may also refuse requests from particular
  networks or VPN connections. Check the exact status message and retry.
- **Configuring VPN split tunnelling?** The installed app is
  `C:\Windows\System32\mRSSScreenSaver.scr`; `.scr` is the Windows screensaver
  executable extension.
- **An image format will not display?** Check **Codec status** and the
  [Testing guide](docs/TESTING.md) for decoder requirements and known limitations.

When reporting a problem, include the app version, Windows version, exact status
message, and a public feed URL with any private tokens removed. For display
problems, include the selected effect, monitor scaling and whether HDR is enabled.

## More information

- [Developer guide](docs/DEVELOPMENT.md)
- [Testing guide and acceptance checks](docs/TESTING.md)
- [Detailed specification](docs/SPECIFICATION.md)
- [Dependency revisions and licenses](docs/DEPENDENCIES.md)

## Disclaimer

mRSS Screen Saver is provided **“as is”, without warranty**, to the extent
permitted by applicable law. Unless required by law or agreed in writing, the
copyright holders and contributors are not liable for loss of data, loss of
use, or other damages arising from using or being unable to use the software.

Images and feed content come from third parties and are not controlled or
endorsed by this project. Feed availability, accuracy and suitability are not
guaranteed. Content filters depend on publisher metadata and cannot guarantee
that all unwanted content will be excluded.

The warranty and liability provisions in sections 15–17 of the [GPL v3
license](LICENSE) apply. This notice does not restrict your rights under that
license or exclude liability that cannot lawfully be excluded.

## License

© 2026 MorpheusUK. All Rights Reserved.

Original project code is licensed under the [GNU General Public License v3.0
only](LICENSE). Third-party dependencies retain their own licenses; see
[NOTICE.md](NOTICE.md) and [Dependency revisions](docs/DEPENDENCIES.md).
