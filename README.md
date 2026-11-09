# CRTube — youtube player for PS3 with focused on CRT

<p align="center">
  <img src="apps/yo-player/ICON0.PNG" alt="CRtube" width="180">
</p>

A PlayStation 3 YouTube client with optional Google/YouTube account sign-in and account-based feeds. Display layout follows the PS3's active video output. The configurable UI scale (1.0x or 1.5x) is available at every output resolution; the 4:3 aspect ratio additionally selects the CRT-oriented layout.

This repository is a fork of [vortigauntlet/yo-player-signin](https://github.com/vortigauntlet/yo-player-signin), which adds YouTube account support to the original [Yo! Player](https://github.com/mohasi/ps3-dev/tree/main/apps/yo-player). This fork continues that work with additional playback, display, and usability changes.

> **Project scope:** The application lives in [`apps/yo-player`](apps/yo-player). The repository also retains the shared PS3 libraries, build infrastructure, and tools from the upstream `ps3-dev` project.

## Highlights

- **Optional YouTube account sign-in:** Sign in using YouTube's device authorization flow. Approve the code on a phone or computer; no browser is required on the PS3.
- **Account and PS3-local libraries:** Browse Recommended, account Subscriptions / History / Watch Later / Playlists, and separate PS3-local Subscriptions / History / Watch Later lists. Open account playlists and browse their videos; Watch Later remains a separate feed. Playback itself remains on the anonymous player client.
- **Dynamic display layout:** The app follows the PS3's active video output. 16:9 keeps the standard widescreen UI; SD 4:3 activates a dedicated CRT layout, including a two-column video grid and adjusted navigation hints.
- **Adjustable UI scale:** Configure the interface scale (1.0x or 1.5x) through **UI Scaling** in the home Settings panel. It applies to the navigation interface at every output resolution, including 480p, 720p and 1080p.
- **Improved YouTube playback handling:** VOD and live playback use separate InnerTube client paths. VOD uses the VISIONOS client; live streams use the Android VR client to obtain the DASH segment data required by the live player.
- **VOD client fallback:** When the primary VISIONOS request explicitly reports an unplayable VOD, the extractor can retry supported alternate clients. Live streams are kept out of this fallback path.
- **Members-only identification:** Restricted videos can be identified and marked in the video grid instead of being mistaken for ordinary playable videos.
- **Streaming playback:** H.264 video and AAC audio are streamed during playback. The app includes the TEE H.264 decoder as an experimental option for supported 1080p50/60 streams, with Sony VDEC remaining the default.
- **Navigation and feeds:** A sidebar provides Search, Recommended, account feeds and Playlists, PS3-local lists, Trending, Live, Account, Settings and Exit. Playlists is a dedicated account-library page outside the L1/R1 category cycle. Trending has Gaming, Sports and Podcasts tabs; Live has Popular, Music, Gaming, Sports, Podcasts and News.
- **Playback and settings:** Search, channel browsing, resume, customizable subtitle styling, chapters, SponsorBlock Skip / Ask / Keep actions per category, autoplay, region selection and background downloads.

## What this fork adds

The fork is based on `vortigauntlet/yo-player-signin`. In addition to the account sign-in and feed functionality inherited from that project, this repository includes the following changes in the current source:

| Area | Changes in this fork |
|---|---|
| CRT / SD presentation | Dedicated 4:3 SD grid layout and SD-specific navigation hints; configurable UI scaling (1.0x/1.5x) is available at every output resolution through the Settings panel |
| YouTube extraction | VISIONOS-based VOD extraction and a separate Android VR request path for live streams |
| Playback resilience | A limited alternate-client retry for VODs explicitly rejected by the primary client; live playback is excluded from that retry |
| Live playback | DASH manifest parsing and rolling segment-sequence playback for live broadcasts |
| Restricted content | Members-only detection and a visible `*Members only` badge on affected grid items |
| Diagnostics | Additional boot-stage and playback/extraction logging to make failures easier to diagnose on real hardware |
| H.264 decoding | Optional TEE Decoder backend for supported 1080p50/60 H.264 streams, with automatic recovery to a lower rendition through Sony VDEC if TEE decoding fails |
| Account playlists | Authenticated YouTube TV playlist discovery, playlist video browsing and continuation-based pagination; Watch Later remains a separate account feed |
| Playlist thumbnails | Playlist cover images are resolved asynchronously from the first video. A single low-priority thumbnail request runs at a time; the queue is reprioritized for the current viewport and the playlist image cache is kept near the visible grid |
| Video-grid progress | A red progress strip along a thumbnail's bottom edge shows the saved playback position for VODs with a resume point; watched items without a saved position retain the existing dimming treatment |
| Subtitle styling | Independent subtitle text/background colors, background opacity, highlight effects and font size, with a live `Lorem Ipsum` preview in Playback Settings |
| PSL1GHT build | An open-toolchain PSL1GHT target and build documentation alongside the Sony SDK project |

The original account functionality remains optional. Without signing in, the app continues to provide its anonymous browsing and playback paths.

## User-facing features

### Navigation, account and feeds
- Open the sidebar with **CIRCLE**, or press **LEFT** while the grid selection is in its first column. While the sidebar is open, **LEFT**, **RIGHT** or **CIRCLE** closes it.
- The sidebar separates account Subscriptions / History / Watch Later / Playlists from the PS3-local versions of those lists. Playlists opens a dedicated account-library page and is not part of the L1/R1 category cycle. It also links to Recommended, Trending, Live, Account, Settings and Exit.
- Open **Account** in the sidebar to sign in or sign out. Enter the device code at the verification URL on another device.
- Access tokens are refreshed when needed. Signing out removes the locally saved token and attempts to revoke it.
- The Playlists page discovers the signed-in account's playlists, displays each playlist's name and video count, and opens its videos in the normal grid. Watch Later remains its own feed.
- Playlist cover thumbnails are fetched asynchronously from each playlist's first video. Only one thumbnail request is active at a time; pending work is reprioritized as the visible grid changes, and off-screen playlist images are evicted from the bounded cache.
- Account authorization is used for account feeds and playlists; video playback remains separate from the signed-in account.

### Browsing and playback
- Browse Trending (Gaming, Sports, Podcasts) and Live (Popular, Music, Gaming, Sports, Podcasts, News), search YouTube, and open channel pages.
- Sort search results by relevance or view count; sort channel videos by latest, popular, or oldest.
- Paste or type a YouTube URL into search to open the video directly.
- Stream H.264 video and AAC audio with hardware decoding.
- Use pause, seek, volume, subtitles, chapter navigation, description overlay, and autoplay controls.
- Resume videos from the last saved playback position. VODs with a saved playback position show a red progress strip along the bottom of their thumbnail; watched items without a saved position use the watched-item dimming treatment.
- Configure each SponsorBlock category as Skip, Ask or Keep.
- Open **Playback Settings** from playback Options to configure subtitle appearance independently of the application theme. Text color, background color, background opacity, highlight (None / Shadow / Outline) and font size are adjustable and saved between sessions. A live `Lorem Ipsum` preview remains available even when subtitles are turned off; text wrapping keeps larger subtitle sizes within the screen width.
- Opening playback Options also starts an asynchronous chapter lookup when description parsing did not find chapters. The chapter list itself opens only when selected.

### Personal library and downloads
- Subscribe to channels and browse their latest uploads.
- Save videos to the local Watch Later list.
- Keep local watch history and playback positions.
- Queue VOD downloads in the background. Live streams cannot be downloaded.
- Completed files are stored on the PS3 HDD. The Sony SDK build uses the application's content directory (`/dev_hdd0/game/YOPCRT001/USRDIR`); if the system content directory cannot be obtained, it falls back to `/dev_hdd0/tmp/crtube_sdk/downloads`. The PSL1GHT build stores downloads in `/dev_hdd0/tmp/crtube/downloads`.
- **XMB Video column integration is planned for a future release and is not available in this release.** Downloads must currently be accessed through the PS3 file system.

### CRT and SD display
- The app detects SD output from the active 480-line video mode (480i or 480p). The selected aspect ratio is handled separately: 4:3 uses the compact CRT-oriented layout, while 16:9 keeps the widescreen layout.
- **UI Scaling** in the home Settings panel offers **1.0x** and **1.5x** at every output resolution, including 480p, 720p and 1080p. The selected aspect ratio independently determines whether the app uses its 4:3 CRT-oriented layout or 16:9 widescreen layout.
- The video grid uses a two-column arrangement on 4:3 SD output to preserve readable titles and usable thumbnail sizes. In 16:9 it uses three columns.
- The UI scale affects display-target interface drawing, not video frames or offscreen rendering. Playback controls and overlays deliberately remain at 1.0x.
- Developer implementation rules and component-level guidance are documented in [`apps/yo-player/UI-SCALING.md`](apps/yo-player/UI-SCALING.md).

## Settings

Open **Settings** from the sidebar or press **START** on the home screen. The menu contains the following application-wide options:

- **UI Scaling** — select **1.0x** or **1.5x** at any output resolution. Change it here; there is no L2 scaling shortcut.
- **Overscan** — apply a uniform zoom adjustment to compensate for televisions that crop the edges of the image. The value ranges from `-10` to `+10` percent; `0` leaves the output unchanged.
- **Region** — select the two-letter country code used for regional YouTube content lists (default: `US`).
- **Autoplay** — enable or disable automatic playback of the next recommended video when the current video ends.
- **Enable 1080p60 (experimental)** — opt in to the TEE H.264 decoder path for supported 1080p50/60 streams. It is disabled by default; Sony VDEC remains the standard playback path.
- **SponsorBlock** — configure each supported segment category independently as **Skip**, **Ask** or **Keep**.

The home Settings menu controls application-wide preferences. Playback **Options** contains per-video controls such as Quality, Audio, Subtitles, Chapters and Crop to Fill, plus **Playback Settings**. Open **Playback Settings → Subtitle Style** to customize subtitle text color, background color and opacity, highlight effect, and font size. Changes are previewed immediately with `Lorem Ipsum`, including when subtitles are disabled, and are saved independently of the application theme.

## Controls

The controls below describe the main navigation and playback screens. Button hints shown
in the app take precedence if a screen has a context-specific action.

### Navigation: home, search, and channel lists

| Controller input | Action |
|---|---|
| D-pad | Move the selection through the video grid |
| **X** | Play the selected video |
| **Square** | Add the selected video to the PS3-local Watch Later list, or remove it if it is already saved |
| **R3** (right-stick click) | Queue the selected VOD for background download; live streams cannot be downloaded |
| **Triangle** | Open the selected video's channel; inside a channel listing, subscribe to or unsubscribe from that channel |
| **SELECT** | Open YouTube search; entering a YouTube URL opens that video directly |
| **START** | Open the home Settings panel; press **START** again or **CIRCLE** to close it. UI Scaling, Overscan, Region and other application preferences are configured there |
| **CIRCLE** | Open the sidebar; in a nested screen, go back. While the sidebar is open, **LEFT**, **RIGHT** or **CIRCLE** closes it |
| **LEFT** | From the first grid column, open the sidebar |
| **L1 / R1** | Switch home categories |
| **R2** | Refresh the current home category |
| **PS button** | Exit the application |

### Playback

| Controller input | Action |
|---|---|
| **X** | Pause or resume playback. Hold to restart a VOD from the beginning; live streams use a normal pause/resume press |
| **LEFT / RIGHT** | Seek backward or forward in a VOD; holding a direction accelerates the seek. Live streams do not support seeking |
| **UP / DOWN** | Decrease or increase the playback volume |
| **TRIANGLE** | Toggle subtitles |
| **SQUARE** | Skip the currently active SponsorBlock segment when its category is set to Ask |
| **L1 / R1** | Play the previous / next video. If no next item is already queued, R1 can request the next recommendation |
| **L2** | Show the full playback information HUD |
| **R2** | Toggle crop-to-fill mode |
| **Right Analog Stick** | While crop-to-fill is enabled, move the cropped picture horizontally and vertically to adjust which part of the image is visible |
| **R3** (right-stick click) | Toggle playback statistics |
| **SELECT** | Open the video description overlay; use **UP / DOWN** to scroll and **SELECT** or **CIRCLE** to close |
| **START** | Open playback Options, including Quality, Audio, Subtitles, Chapters, Crop to Fill and Autoplay; press **START** again or **CIRCLE** to close the panel. Playback Settings contains Subtitle Style |
| **CIRCLE** | Return to the video list |

Live streams have no seek bar and cannot be downloaded. The description and chapter overlays temporarily take control of navigation while open. Audio tracks, quality and chapters are selected from playback Options; Square is reserved for SponsorBlock Ask actions. Background downloads are saved to the HDD locations listed above; they are not added to the XMB Video column in this release.

## Build and installation

The application has two build paths:

- **Sony SDK project:** Uses the repository's existing PS3 development environment and shared build infrastructure.
- **PSL1GHT project:** An open-toolchain build intended for Linux, macOS, or the included GitHub Actions workflow.

For PSL1GHT dependencies, build commands, packaging, installation, and known platform differences, see [`apps/yo-player/BUILDING-PSL1GHT.md`](apps/yo-player/BUILDING-PSL1GHT.md).

For the application architecture, settings, storage files, playback behavior, and detailed controls, see [`apps/yo-player/README.md`](apps/yo-player/README.md).

## Known limitations

- YouTube changes its internal APIs and client restrictions frequently. Extraction compatibility may require future updates.
- Live streams use a rolling segment window. They start and exit differently from VOD and do not support normal seeking or background downloading.
- Completed background downloads remain in the application's HDD storage directory. Automatic registration in the XMB Video column is planned for a future release and is not part of the current release.
- The TEE Decoder is experimental and opt-in. It supports a defined subset of H.264 (8-bit 4:2:0, progressive, CABAC, one slice per picture, up to 1920×1088); unsupported streams continue through Sony VDEC where possible. If TEE decoding fails during playback, the player attempts to switch to a lower rendition using Sony VDEC.
- Account feeds require a working network connection and valid Google authorization. Access can be revoked from the Google account security page.

## Credits and upstream

This project builds on the work of:

- **mohasi** — original Yo! Player and the `ps3-dev` project, including the PS3 application foundation, shared libraries, rendering, and media playback infrastructure.
- **vortigauntlet** — the `yo-player-signin` fork and its YouTube account sign-in implementation.
- **openlewa** — the PSL1GHT Yo! Player port that forms part of the project's toolchain lineage.
- **TheErsysEnding** — author of the TEE Decoder integrated from [TEE-Vanced-PS3](https://github.com/TheErsysEnding/TEE-Vanced-PS3). The decoder is separately licensed under LGPL-2.1-or-later; see `libs/simple-lib-av/tee_decoder.LICENSE` and `NOTICE`.
- **PS3 homebrew and PSL1GHT contributors** — the open-source libraries and tools used by the project.

Upstream repositories:
- [This fork](https://github.com/patnog/yo-player-signin-crt)
- [Parent fork: yo-player-signin](https://github.com/vortigauntlet/yo-player-signin)
- [Original Yo! Player in ps3-dev](https://github.com/mohasi/ps3-dev/tree/main/apps/yo-player)
- [PSL1GHT Yo! Player project](https://github.com/openlewa/yo-player-psl1ght)

## License

The repository's original `ps3-dev`-derived code is licensed under Apache-2.0 (`LICENSE`). That license does **not** override component-specific licenses: the integrated TEE Decoder is LGPL-2.1-or-later (`libs/simple-lib-av/tee_decoder.LICENSE`), and its bundled runtime portions retain their own notices. See `NOTICE` before redistributing source or binary packages.
