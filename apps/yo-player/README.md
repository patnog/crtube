# yo-player

A native PlayStation 3 YouTube client. Watch YouTube on the console the way the official app should have:
fast, no PSN sign-in, no ads.

An open-toolchain PSL1GHT port is available alongside the original Sony SDK
project. See [BUILDING-PSL1GHT.md](BUILDING-PSL1GHT.md) for the toolchain,
build, package, installation, limitations, and hardware-test procedure.

## Why

The official PS3 YouTube app is sluggish, forces a PSN account, and is full of ads. yo-player talks to YouTube
directly and only pulls the video and audio, so there are no ads and no account gate — and it stays snappy.

It reuses what this repo already has:
- **`simple-lib-av`** — H.264/AAC decode and playback, the only codecs the PS3 decodes in hardware.
- **`simple-lib-app`** — the UI framework (screens, widgets, fonts) shared with `file-manager`.
- **`simple-lib-tls`** — a modern TLS stack so the app can reach YouTube's servers, which the console's own
  networking can no longer handshake with.

## Features

- **Browse & search** — a sidebar with Recommended, account Subscriptions / History / Watch Later / Playlists, separate PS3-local Subscriptions / History / Watch Later / Playlists, Trending and Live. Search has Relevance / Views sort orders; channel views have Latest / Popular / Oldest. Pasting a YouTube link into search opens it directly. Trending tabs are Gaming, Sports and Podcasts; Live tabs are Popular, Music, Gaming, Sports, Podcasts and News.
- **Playback with sound** — streamed as it plays (nothing is downloaded in full first), H.264 + AAC. Sony VDEC is the default decoder. An experimental setting can enable the integrated TEE Decoder for supported 1080p50/60 H.264 streams. A continuous monitor measures presented frame rate in eight-second windows; sustained slowdown can trigger up to three downward rendition changes, with a cooldown between switches. This per-playback adaptation does not modify global settings.
  Pause and seek with a scrub bar showing current / total time.
- **Live streams** — plays anything with a LIVE badge (sports, news), a few seconds behind the live edge. Startup waits for a small segment buffer; transient segment-fetch failures are retried with capped backoff, and a stalled or failed LIVE session can be renewed. A delayed/paused DVR stream can return to the moving live edge with R1.
- **Resume** — videos pick up from where you last stopped; already-watched videos are dimmed in the grid.
- **Subscriptions** — subscribe / unsubscribe from inside a channel; the Subscriptions feed gathers the latest
  uploads from every channel you follow, newest first.
- **Watch Later and PS3 playlists** — Watch Later remains its own list. The PS3-local Playlists page has Watch Later and PS3 tabs; create empty playlists, create a playlist while saving a video, add/remove videos, rename or delete playlists, and sort local playlists by Newest or A-Z.
- **Download** — combine a video's video + audio into one `.mp4` file in the background, with a queue and progress readout. LIVE videos are rejected before queueing and checked again by the worker. Download settings let you choose resolution, audio language/source, primary subtitles that follow the selected audio language, and an optional secondary subtitle language. The Downloads category displays local thumbnails and video durations; downloaded videos can be played locally or deleted from that category.
- **Volume** — adjust in the player with a pill meter; the level is remembered across videos.
- **Description** — read the full video description over the playing video, scrollable, without pausing.
- **Chapters** — jump around by chapter. Chapters come from the description's timestamps, falling back to
  YouTube's own chapter list when the description has none. Chapter boundaries are notched onto the seek bar.
- **Subtitles** — cycle through the video's available subtitle languages (off by default); the chosen language
  shows in the player, and lines are drawn above the seek bar. Downloaded subtitle tracks are stored with their video and remain available for local playback.
- **SponsorBlock** — community-marked segments are shown on the seek bar. Each category can be configured as Skip, Ask (Square skips the active segment) or Keep.
- **No PSN, no ads** — talks to YouTube directly; appears under the XMB **Video** column like the old app.

## TEE H.264 decoder

This fork includes TheErsysEnding's TEE Decoder from
[TEE-Vanced-PS3](https://github.com/TheErsysEnding/TEE-Vanced-PS3), integrated into
`simple-lib-av`. It decodes H.264 on the PS3's SPEs and can reach 1080p60 on
supported streams. It is an additional backend; Sony's `cellVdec` remains the
default and handles streams outside the TEE decoder's supported format.

The decoder currently supports H.264 Main/High Profile, 8-bit 4:2:0, progressive
pictures, CABAC, one slice per picture, and coded dimensions up to 1920×1088.
It does not support CAVLC/Constrained Baseline, interlaced pictures, multiple
slices per picture, 10-bit video, or 4:2:2 chroma. The player only offers
50/60-fps streams to this backend when the experimental setting is enabled and
the decoder passes its availability check.

The option is **off by default**. Set `enable-1080p60=on` in `settings.txt`, or
enable **Enable 1080p60 (experimental)** in the home Settings panel. If the TEE
decoder reports a fatal playback error, the player attempts an automatic
handoff to a lower rendition decoded by Sony VDEC. This recovery is not a
guarantee that every stream will play.

**License:** the TEE Decoder is Copyright © 2026 TheErsysEnding and is licensed
under **GNU LGPL-2.1-or-later**, not Apache-2.0. Its full license is included
at [`../../libs/simple-lib-av/tee_decoder.LICENSE`](../../libs/simple-lib-av/tee_decoder.LICENSE);
additional attribution and bundled-runtime notices are in
[`../../NOTICE`](../../NOTICE).

## Controller support

The shared input layer supports the Sony BD Remote in addition to standard PS3 controllers. BD Remote key codes are translated into the same logical buttons used by the player: directional and face buttons, L1/R1/L2/R2, Start and Select retain their usual meanings. Enter, Play and Pause map to Cross; Return maps to Circle; Next and Scan Forward map to Right; Previous and Scan Reverse map to Left. The remote has no analogue axes, so features requiring an analogue stick or stick click (such as R3 download/statistics) require a standard controller.

## Display output and aspect ratios

The app follows the PS3's active system video output configuration. It does not force
the console into a fixed aspect ratio or permanently switch the interface to CRT mode.

- **UI Scaling:** The home **Settings** panel offers **1.0x** and **1.5x** at every output
  resolution, including 480p, 720p and 1080p. This applies to 4:3 SD and 16:9 SD/HD modes.
- **4:3 aspect ratio:** Selects the dedicated CRT-oriented layout, including a two-column
  video grid and SD-specific navigation hints.
- **16:9 aspect ratio:** Keeps the standard widescreen layout, including on 480-line SD output.

Aspect ratio and output resolution are detected independently. A 720x480 framebuffer can
represent either 4:3 or anamorphic 16:9; UI scaling is available independently of
resolution, while the configured aspect ratio selects the layout. The scale is changed in **Settings**, not with
an L2 shortcut. See [UI-SCALING.md](UI-SCALING.md) for developer guidance and limitations.
## YouTube account sign-in

Signing in is optional. From the home screen, press **SELECT** to open the account panel. The app displays a device code and verification URL; enter the code on a phone or computer and approve access there. No web browser is needed on the PS3.

When signed in, the home screen can show the account's:
- Recommended feed.
- Subscriptions feed.
- Watch History.
- Watch Later playlist.

The app uses the YouTube TV device authorization flow. It stores a refresh token locally in `/dev_hdd0/tmp/yo-player/account.txt` and refreshes short-lived access tokens when required. Press **SELECT** and choose **Sign out** to remove the saved token and attempt to revoke it with Google.

**Playback does not use the signed-in account.** Account authorization is used for account-specific feeds; video resolution and playback continue through the app's separate anonymous player client.

Treat `account.txt` as a credential: anyone who can read the file may be able to access the account feeds until the token is revoked. Do not share it in debug logs or support reports.

## How it works (high level)

1. On launch it brings up the network, the modern TLS transport, graphics, audio, fonts and the pad, then loads
   the home screen with its default category selected (Trending when signed out).
2. Feeds, searches and channel pages are fetched from YouTube on demand and shown as a grid of thumbnails. Only
   the tiles currently on screen hold a thumbnail image, so a feed can hold up to 500 videos at a flat memory
   cost, and each category is cached for the session so switching back to it is instant.
3. Playing a video pulls the picture and sound streams and decodes them in hardware through `simple-lib-av`,
   streaming as it goes rather than waiting for a full download. The picture and sound streams open in parallel
   so the video starts roughly twice as fast.
4. Downloads run on a background worker: it reads the same picture and sound streams and remuxes them (repackages
   them, no re-encoding) into a single `.mp4` file on disk, while you keep browsing.

## Screens & controls

The app boots into the home feed. The sidebar switches between account feeds, PS3-local lists and playlists, Trending and Live. The PS3-local Playlists page has separate Watch Later and PS3 tabs. Searching or opening a channel pushes a list screen; playing pushes the player. **Circle** opens the sidebar on the home screen and backs out of nested screens. Button hints along the bottom use the console's own controller glyphs and change with the active category or overlay; when a hint differs from a general control summary, follow the on-screen hint.

### Home / search / channel lists

- **D-pad** — move the highlight. Left / right wrap onto the previous / next row, so you can run through every
  tile with just left and right.
- **X** — play the highlighted video.
- **Square** — tap to add/remove the highlighted video from PS3-local **Watch Later**; hold to open the local-playlist picker and add it to a PS3 playlist.
- **L2** — cycle the sort order only on supported lists: Trending, PS3 Subscriptions, Live subcategories, account Recommended/Subscriptions, account Playlists, PS3-local Playlists (the active Watch Later or PS3 tab), and Downloads. Other categories do not have an L2 sort action.
- **L3** — on the PS3 tab of the PS3-local Playlists page, create an empty playlist.
- **R3 (right stick click) — **download** the highlighted video (see Downloads below). Live streams can't be
  downloaded.
- **Triangle** — open the highlighted video's **channel** (that channel's videos only). Inside a channel,
  Triangle instead **subscribes / unsubscribes** from that channel.
- **Select** — open YouTube search. Typing a YouTube link opens it directly.
- **Start** — open the home Settings panel.
- **L1 / R1** — context-sensitive navigation:
  - On the home feed ring, move to the previous / next category. The ring order differs when signed in because account feeds are included.
  - On the Live page, cycle its six subcategories: Popular, Music, Gaming, Sports, Podcasts and News.
  - On the PS3-local Playlists page, switch between the Watch Later and PS3 tabs.
  - They do not switch categories inside playlist contents, on the account Playlists page, or in Downloads.
- **R2** (home only) — refresh the current category.
- **Circle** — open the sidebar; in nested screens, back out one level.
- **Left** — from the first grid column, open the sidebar.
- **PS button** — exit.

### In the player

- **Up / Down** — volume (a pill meter shows briefly, remembered across videos).
- **Left / Right** — seek back / forward in VOD. Holding a direction accelerates the seek; live streams cannot
  be seeked.
- **X** — pause / resume. Hold to restart a VOD from the beginning; live streams use a normal pause/resume press.
- **Square** — skip the active SponsorBlock segment when its category is set to Ask.
- **L1** — play the previous video when one is available. This action is unavailable during local downloaded-video playback.
- **R1** — context-sensitive:
  - On a Live stream that is paused or behind the live edge, return playback to the current live point.
  - When the Live stream is already at the live edge, continue to the next queued video or request the next recommendation.
  - On VOD, play the next queued video or request the next recommendation when the queue is empty.
  - Previous/next navigation is unavailable for locally downloaded-video playback.
- **L2** — show the full playback information HUD.
- **R2** — toggle Crop-to-Fill mode.
- **Right Analog Stick** — while Crop-to-Fill is enabled, move the cropped picture horizontally and vertically to adjust which part of the image is visible.
- **R3** (right-stick click) — toggle playback statistics.
- **Select** — show the video information panel over the video (playback carries on); scroll with up / down, close
  with Select or Circle. It includes the description and metadata such as view count,
  publication date and like count. Metadata is fetched only when the panel is opened,
  so it does not delay playback startup.
- **Triangle** — toggle **subtitles**.
- **Start** — open playback Options. Quality, Audio, Subtitles, Chapters, Crop to Fill and Autoplay are selected there. Playback Settings provides UI Scaling, Overscan, Subtitle Style, Subtitle Offset and SponsorBlock preferences.
- In **Playback Settings → Subtitle Offset**, use **Left / Right** to move subtitles down / up one virtual screen pixel. Hold a direction to repeat the adjustment; positive values move subtitles upward.
- **Circle** — back to the list.

Community **SponsorBlock** segments are marked on the seek bar in their category colours. Categories set to Ask can be skipped with Square. Live streams have no seek bar (there's nothing to scrub).

## UI scaling

The UI scale can be selected in the home **Settings → UI Scaling** menu. It offers **1.0x**
and **1.5x** at every output resolution, including 480i/480p, 720p and 1080p. The selected
value is applied to the navigation interface in SD and HD modes. The choice is saved and
restored on the next launch:

- PSL1GHT: `/dev_hdd0/tmp/crtube/sd-ui-scale.txt`
- Sony SDK: `/dev_hdd0/tmp/crtube_sdk/sd-ui-scale.txt`

The 4:3 aspect ratio selects the CRT-oriented layout; 16:9 retains the widescreen layout.
Playback uses the selected value for text rasterisation, while video geometry and HUD
placement remain unchanged. A separate **Playback Settings → UI Scaling** row can switch
the playback text scale between 1.0x and 1.5x. Video frames are not enlarged by either
setting. Developer details are in
[UI-SCALING.md](UI-SCALING.md).

## Downloads

Pressing **R3** on a video queues a download. The app rejects videos identified as currently LIVE, and the background worker performs a second LIVE check before fetching media. The picture and sound streams are combined into one `.mp4` file. Before downloading, configure **Settings → Download settings**: choose Auto / 480p / 720p / 1080p, the audio language (or Original upload), whether auto-dubbed audio is allowed for a translated language, whether primary subtitles should automatically match the selected audio language, and an optional secondary subtitle language. These preferences affect downloads, not streaming playback.
Downloads run in the background so you can keep browsing, queue up if you start several, and show progress in
the top-right corner. Live streams can't be downloaded.

The Downloads category keeps a local library entry with the thumbnail, title, description and duration; older MP4 files can have their duration recovered from the MP4 movie header. Select a completed item and press **X** to play it directly from local storage. Hold **Square** to open the delete confirmation. Downloaded subtitle files are kept with the video and can be used during local playback. That is what makes it appear in the XMB's Video column, named
after the video, next to anything you would normally copy over from a USB stick. Copying a file into
`/dev_hdd0/video` by hand does not work: the XMB lists videos from that database, not from the folder.

Two constraints came out of getting that working, and both shape the code. The export only accepts files from
the directory the game content utility hands back, so downloads are written straight into the app's own
`USRDIR`. And it rejects filenames containing spaces or brackets, so the file is staged under the video id;
the name you actually see comes from the title passed to the export.

## Updates

The **Updates & About** panel checks the latest published GitHub release for
`patnog/crtube`. The check is performed by a background worker, so the interface
remains responsive while the request is in progress.

The checker requests:

`https://api.github.com/repos/patnog/crtube/releases/latest`

It reads the release tag, title, release notes and the asset named exactly
`crtube.pkg`. Version tags must contain three numeric components
(`major.minor.patch`), optionally prefixed with `v`. The installed version comes
from the build's `CRTUBE_VERSION` definition. A release is reported as available
only when its parsed version is newer than the installed version; otherwise the
status is up to date. Network, HTTP, JSON or version-tag failures produce a
failed-check state.

Automatic checks can be disabled in the update panel. An update can also be
ignored by tag; ignoring a release suppresses its notification without changing
the installed version or deleting downloaded files. A manual check remains
available from the panel.

### Downloading an update

When a release is available, the panel can download its `crtube.pkg` asset.
The download runs on a background worker and reports byte progress. The file is
first written as a `.part` file and is renamed to its final name only after the
stream has completed and the received byte count matches the HTTP content
length when one is provided. Failed or incomplete downloads are discarded.

The retained download is stored at:

`/dev_hdd0/tmp/crtube/update/crtube-<version>.pkg`

The application also copies the completed package to:

`/dev_hdd0/packages/crtube-<version>.pkg`

This second copy makes the package available to an external PS3 package
installer. **CRTube does not install or launch the PKG installer itself.**
Install the downloaded package using the console's package-installation
workflow or another compatible package installer.

If the same release package is already present and recorded as the last
download, the downloader reuses it and refreshes the `/dev_hdd0/packages`
copy rather than downloading it again. The overwrite action removes the
previously recorded update package and the currently advertised package from
both locations before starting a fresh download.

### Update implementation

The update flow is isolated in `src/updater.c` and exposed through
`include/updater.h`:

- `startUpdateCheck` starts the asynchronous GitHub release check.
- `getUpdateCheckState`, `isUpdateAvailable` and the release getters expose
  the result to the UI.
- `ignoreCurrentUpdate` and `disableAutomaticUpdateChecks` persist the user's
  notification preferences through the settings layer.
- `startUpdateDownload` and `startUpdateDownloadOverwrite` manage package
  downloads; the download getters expose state, byte counts and the saved path.
- `shutdownUpdater` cancels outstanding HTTP work and joins update/download
  workers during shutdown.

The checker and downloader depend on the shared HTTP, JSON, settings, thread,
debug/logging and VFS helpers. They do not invoke webMAN or any in-app package
installation command. The release endpoint, exact asset filename and storage
locations are implementation constants in `src/updater.c`.

## Settings

User settings live in `settings.txt` under the build's `CRTUBE_DATA_DIR` (PSL1GHT: `/dev_hdd0/tmp/crtube/`;
official SDK: `/dev_hdd0/tmp/crtube_sdk/`), created with defaults on first launch. It's a plain `key=value`
text file you can edit over FTP; file edits apply on the next launch, while Settings-panel changes such as
overscan are saved immediately. A bad value quietly falls back to its default.

- **`autoplay`** — `on` (default) plays the next recommendation when a video ends; `off` returns to the library.
- **`enable-1080p60`** — `off` (default) or `on`; enables experimental TEE Decoder selection for supported H.264 50/60-fps streams.
- **`subtitle-language`** — preferred language code (default `en`); creator-provided tracks are tried before English auto-generated captions.
- **`download-resolution`** — `0` (Auto, default), `1` (480p), `2` (720p) or `3` (1080p).
- **`download-audio-language`** — `original` (default) or a supported language code for downloaded audio.
- **`download-audio-auto-dub`** — `on` (default) allows YouTube auto-dubbed tracks when a translated audio language is selected; `off` prefers uploaded tracks only. This option is not applicable to Original upload.
- **`download-primary-subtitles`** — `on` (default) automatically downloads a primary subtitle track matching the selected audio language; `off` disables it.
- **`download-secondary-subtitle`** — `off` (default) or a supported language code for an optional second subtitle track.
- **`region`** — two-letter country code for content lists (default `US`); selectable in Home Settings → Content Region.
- **`screensaver-timeout`** — `0` (off) or `30`–`300` seconds in 30-second steps (default `30`).
- **`overscan`** — uniform output zoom from `-15` to `+15` percent (default `0`), used in normal video mode.
- **`overscan-crop-to-fill`** — independent video zoom from `-15` to `+15` percent (default `0`), used only while Crop to Fill is enabled. It is configured separately from `overscan`, so normal video output and Crop-to-Fill framing can be adjusted independently.
- **`subtitle-offset`** — subtitle vertical offset in virtual screen pixels, `-150` to `+150` (default `0`; positive moves up).
- **`subtitle-text-color`**, **`subtitle-background-color`**, **`subtitle-background-opacity`**, **`subtitle-highlight`**, **`subtitle-font-size`** — subtitle appearance indexes with ranges `0–5`, `0–4`, `0–4`, `0–2` and `0–5`.
- **`theme-custom-bg`** and **`theme-custom-accent`** — custom theme background (`dark`/`light`) and accent colour (`#RRGGBB`).
- **`sponsorblock-mode`** — legacy global mode (`off`, `ads`, `all`) used to migrate older settings files; per-category settings take precedence.
- **`sponsorblock-sponsor`, `sponsorblock-selfpromo`, `sponsorblock-interaction`, `sponsorblock-intro`, `sponsorblock-outro`, `sponsorblock-music-offtopic`, `sponsorblock-filler`** — each accepts `skip`, `ask` or `keep`.
- **`theme`** — colour theme to start in: `crtube` (default), `blue`, `green`, `light`, `monochrome` or `custom`; registered theme slugs are also supported.

## Themes

Every colour the app draws comes from one theme, and the shipped one is YouTube's own dark palette:
`#0F0F0F` background, `#212121` surfaces, `#AAAAAA` secondary text, YouTube red for the seek bar, LIVE
badges and the volume meter, and a white ring around the selected thumbnail. Everything is drawn flat
from plain rectangles and text — there are no sprite sheets.

`themes.txt` is stored in the toolchain-specific data directory. The home
**Settings → Themes** panel provides CRTube, Blue, Green, Light and Monochrome
presets, plus a Custom editor for dark/light background and accent colour. Custom
choices are persisted in `settings.txt` as `theme=custom`, `theme-custom-bg` and
`theme-custom-accent`.

The registry also supports custom `[Name]` blocks for FTP editing. A custom block
inherits the built-in CRTube palette unless overridden. Colours are `#RRGGBB` or
`#RRGGBBAA` (alpha last). The `theme` setting selects a preset or registered theme
slug. Changes made in the theme panel are saved immediately.

## Screensaver

The inactivity screensaver is enabled by default with a 30-second timeout. Change it
in **Home → Settings → Screensaver timeout**; available values are Off and
30–300 seconds in 30-second increments. It displays the packaged bouncing CRTube
logo on a black background. The logo is decoded asynchronously and uploaded to the
GPU on the main thread.

Any controller activity dismisses the screensaver. It can activate on the home screen
or while playback is paused; it does not interrupt actively playing video. During paused
playback, pressing **X** while the screensaver is visible also resumes the video. The
dismissal input is prevented from reaching the underlying screen. The underlying screen
and its input handlers remain untouched while the overlay is active.

## Subtitle appearance

Playback **Options → Playback Settings → Subtitle Style** exposes five preferences:
text colour, background colour, background opacity, text highlight and font size.
They are stored as `subtitle-text-color`, `subtitle-background-color`,
`subtitle-background-opacity`, `subtitle-highlight` and `subtitle-font-size`.
Their accepted ranges are respectively 0–5, 0–4, 0–4, 0–2 and 0–5.
**Subtitle Offset** is separate (`subtitle-offset`, -150 to +150 virtual screen
pixels); positive values move subtitles upward.

## Content region

**Home → Settings → Content Region** selects the country code (`region`) used for
YouTube browse and search requests. The default is `US`. Changing the region
invalidates regional feed caches so affected lists are fetched again. The available
countries are defined by the application; this is not an unrestricted country-code
field.

## Storage

Persistent application data is stored under a toolchain-specific directory — no account binding, so copy
the corresponding folder to back it up or move it to another console:

- PSL1GHT: `/dev_hdd0/tmp/crtube/`
- Sony SDK: `/dev_hdd0/tmp/crtube_sdk/`

PS3-local playlists and their index are stored alongside the other local library data:

- `settings.txt` — user settings (above).
- `subscriptions.txt` — subscribed channel ids, one per line (seeded with a few defaults on first run; editable
  by hand). Subscriptions are also toggled with Triangle inside a channel.
- `watchlater.txt` — the Watch Later queue. It keeps full video details so the list opens instantly without
  re-fetching.
- PS3-local playlist index and per-playlist files — playlist names and video entries for the PS3 tab.
- watch history and last-position files — used to dim already-watched tiles and to resume playback.

## Build & deploy

Both targets produce an NPDRM `.pkg`. The PSL1GHT build produces `crtube.pkg`
(and `crtube.gnpdrm.pkg`); the original Sony-SDK build produces
`out/crtube-sdk.pkg` through the **ps3 MCP tool** (or its build VM):

1. `mcp__ps3__list` — confirm the app name.
2. `mcp__ps3__build` with kind `apps`, name `yo-player` — returns a job id.
3. `mcp__ps3__poll` the job id until it finishes; exit code 0 means success.

Install the resulting `.pkg` with any PS3 package installer. It appears on the XMB under **Video**. Note: after
editing any shared library it links against, do a full **Rebuild** of the app, since an incremental build can
keep the old library code.

`TITLE_ID` is `YOPLAYER1`.

The open-source target uses native PSL1GHT Makefiles and can be built on Linux,
macOS, or the included GitHub Actions runner without a local VM. Its side-by-side
title ID is `YOPSL0001`; see [BUILDING-PSL1GHT.md](BUILDING-PSL1GHT.md) for the
exact command, package filename, and platform-specific limitations.

## YouTube extraction and playback notes

VOD and live streams use separate extraction paths because YouTube does not return the same usable media formats for both:

- **VOD:** The player request identifies as the VISIONOS client. The extractor can retry supported alternate clients only when the primary VOD request explicitly returns an unplayable response.
- **Live:** The extractor uses the Android VR client to obtain the DASH manifest, then follows the rolling live segment sequence. Live requests are not sent through the VOD fallback path.
- **Members-only videos:** The grid checks restricted videos in the background and marks detected items with a `*Members only` badge. A restricted item may not provide a playable stream to an anonymous session.

These client identifiers and YouTube endpoints are implementation details, not stable APIs. YouTube can change or block them without notice.


## Playback recovery and network failure handling

Playback distinguishes media-source failures from decoder/demuxer failures so a damaged or unsupported stream is not mistaken for a temporary network outage.

- **VOD session recovery:** transport or HTTP source failures can trigger a fresh playback-session extraction and player replacement. The player makes at most five renewal attempts; after that it stops with an explicit failure state instead of looping indefinitely.
- **LIVE segment recovery:** individual DASH segment fetches are retried with exponential delays starting at 500 ms and capped at 5 seconds. A source stops after five consecutive fetch failures. Initial playback primes up to three roughly two-second segments (about six seconds) before releasing the reader, reducing startup starvation.
- **LIVE session recovery:** a failed LIVE source can trigger a renewed extraction/session, with a bounded retry budget. If no new frame is presented for 20 seconds, the player treats the stream as stalled and begins recovery rather than waiting forever.
- **Adaptive quality:** presented frame rate is monitored continuously in eight-second windows. Sustained slowdown can trigger up to three downward rendition changes, with a cooldown. It can first move from high frame rate to 30 FPS and then reduce resolution. These changes apply only to the current playback.
- **HTTP stream recovery:** the shared streaming reader can reopen a failed source and retry stalled reads a bounded number of times. Consecutive transport failures are tracked separately from ordinary empty-buffer waits; exhausted retries become a terminal source error propagated to the player.
- **TLS/network time bounds:** the BearSSL transport bounds DNS and TCP connection waits to five seconds, socket I/O waits to five seconds, and response-header waits to ten seconds. This limits hangs caused by stalled network operations; it cannot guarantee that a remote server will recover.

These are bounded recovery mechanisms, not a guarantee that every video or network condition can be recovered. Decoder failures remain distinct and are not blindly retried as network failures.

## Known limitations

- **Live streams** use a different delivery than normal videos (a rolling sequence of segments rather than one
  file), so they take a little longer to start and to exit, and can't be downloaded or seeked. Otherwise they
  play the same, a few seconds behind the live edge.
- **TEE Decoder support** is experimental and limited to H.264 Main/High, 8-bit 4:2:0, progressive, CABAC, one slice per picture, up to 1920×1088. CAVLC, interlaced, multi-slice, 10-bit and 4:2:2 streams are not supported by TEE; Sony VDEC remains the fallback path where supported. Separately, continuous presented-frame-rate monitoring can step down to a 30 FPS rendition and then lower resolutions when the current stream cannot sustain its target cadence. This is a per-playback recovery and does not change saved quality or other global settings.
- The Subscriptions feed is a merged list, so it can't page past the first batch it gathers from each channel.

## Credits and component licenses

- **Original application and shared PS3 libraries:** mohasi / `ps3-dev` contributors (Apache-2.0).
- **YouTube account sign-in lineage:** vortigauntlet / `yo-player-signin`.
- **TEE H.264 decoder:** TheErsysEnding, integrated from
  [TEE-Vanced-PS3](https://github.com/TheErsysEnding/TEE-Vanced-PS3).
  The decoder is LGPL-2.1-or-later; see
  [`../../libs/simple-lib-av/tee_decoder.LICENSE`](../../libs/simple-lib-av/tee_decoder.LICENSE)
  and [`../../NOTICE`](../../NOTICE).

The repository-level Apache-2.0 license applies to the Apache-licensed project
code; it does not relicense the TEE Decoder or the other third-party components.
