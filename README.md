# Yo! Player — Sign-In & CRT Edition

A PlayStation 3 YouTube client with optional Google/YouTube account sign-in and account-based feeds. Display layout follows the PS3's active video output: standard widescreen UI for 16:9, with additional CRT-oriented layout and scaling adjustments for SD 4:3.

This repository is a fork of [vortigauntlet/yo-player-signin](https://github.com/vortigauntlet/yo-player-signin), which adds YouTube account support to the original [Yo! Player](https://github.com/mohasi/ps3-dev/tree/main/apps/yo-player). This fork continues that work with additional playback, display, and usability changes.

> **Project scope:** The application lives in [`apps/yo-player`](apps/yo-player). The repository also retains the shared PS3 libraries, build infrastructure, and tools from the upstream `ps3-dev` project.

## Highlights

- **Optional YouTube account sign-in:** Sign in using YouTube's device authorization flow. Approve the code on a phone or computer; no browser is required on the PS3.
- **Personal account feeds:** Browse Recommended, Subscriptions, History, and Watch Later feeds from the signed-in account. Playback itself remains on the anonymous player client.
- **Dynamic display layout:** The app follows the PS3's active video output. 16:9 keeps the standard widescreen UI; SD 4:3 activates a dedicated CRT layout, including a two-column video grid and adjusted navigation hints.
- **Adjustable SD UI scale:** Press **L2** on the home screen or channel view to switch between the supported SD interface scales. The selection is saved separately from the normal settings file.
- **Improved YouTube playback handling:** VOD and live playback use separate InnerTube client paths. VOD uses the VISIONOS client; live streams use the Android VR client to obtain the DASH segment data required by the live player.
- **VOD client fallback:** When the primary VISIONOS request explicitly reports an unplayable VOD, the extractor can retry supported alternate clients. Live streams are kept out of this fallback path.
- **Members-only identification:** Restricted videos can be identified and marked in the video grid instead of being mistaken for ordinary playable videos.
- **Streaming playback:** H.264 video and AAC audio are streamed and decoded by the PS3; the app does not need to download an entire video before playback.
- **User features:** Search, channel browsing, subscriptions, Watch Later, watch history, resume playback, subtitles, chapters, SponsorBlock, background downloads, and XMB video export.

## What this fork adds

The fork is based on `vortigauntlet/yo-player-signin`. In addition to the account sign-in and feed functionality inherited from that project, this repository includes the following changes in the current source:

| Area | Changes in this fork |
|---|---|
| CRT / SD presentation | Dedicated 4:3 SD grid layout, SD-specific navigation hints, and a persistent 1.0x / 1.5x UI scale toggle |
| YouTube extraction | VISIONOS-based VOD extraction and a separate Android VR request path for live streams |
| Playback resilience | A limited alternate-client retry for VODs explicitly rejected by the primary client; live playback is excluded from that retry |
| Live playback | DASH manifest parsing and rolling segment-sequence playback for live broadcasts |
| Restricted content | Members-only detection and a visible `*Members only` badge on affected grid items |
| Diagnostics | Additional boot-stage and playback/extraction logging to make failures easier to diagnose on real hardware |
| PSL1GHT build | An open-toolchain PSL1GHT target and build documentation alongside the Sony SDK project |

The original account functionality remains optional. Without signing in, the app continues to provide its anonymous browsing and playback paths.

## User-facing features

### Account and feeds
- Sign in or sign out from the home screen using **SELECT**.
- Enter the device code at the verification URL shown by the app on another device.
- Browse the account's Recommended, Subscriptions, History, and Watch Later feeds.
- Access tokens are refreshed when needed. Signing out removes the locally saved token and attempts to revoke it.
- Account authentication is used for account feeds; video playback remains separate from the signed-in account.

### Browsing and playback
- Browse trending categories, search YouTube, and open channel pages.
- Sort search results by relevance or view count; sort channel videos by latest, popular, or oldest.
- Paste or type a YouTube URL into search to open the video directly.
- Stream H.264 video and AAC audio with hardware decoding.
- Use pause, seek, volume, subtitles, chapter navigation, description overlay, and autoplay controls.
- Resume videos from the last saved playback position.
- Skip community-submitted SponsorBlock segments according to the selected setting.

### Personal library and downloads
- Subscribe to channels and browse their latest uploads.
- Save videos to the local Watch Later list.
- Keep local watch history and playback positions.
- Queue video downloads in the background.
- Export completed downloads to the PS3 XMB Video column.

### CRT and SD display
- On SD output, the application uses a layout designed around a 4:3 screen.
- Press **L2** on the home screen or channel view to cycle the available SD UI scales.
- The scale choice is persisted in `/dev_hdd0/tmp/yo-player/sd-ui-scale.txt`.
- The video grid uses a two-column arrangement on 4:3 SD output to preserve readable titles and usable thumbnail sizes.

## Controls

The controls below describe the main navigation and playback screens. Button hints shown
in the app take precedence if a screen has a context-specific action.

### Navigation: home, search, and channel lists

| Controller input | Action |
|---|---|
| D-pad | Move the selection through the video grid |
| **X** | Play the selected video |
| **Square** | Add the selected video to Watch Later, or remove it if it is already saved |
| **R3** (right-stick click) | Queue the selected VOD for background download; live streams cannot be downloaded |
| **Triangle** | Open the selected video's channel; inside a channel listing, subscribe to or unsubscribe from that channel |
| **START** | Open YouTube search; entering a YouTube URL opens that video directly |
| **SELECT** | On the home screen, open the account panel; in search results or a channel listing, change the sort order |
| **L1 / R1** | Switch home categories |
| **L2** | On SD 4:3 output, cycle the additional UI scale on the home screen or channel listing |
| **R2** | Refresh the current home category |
| **CIRCLE** | Go back to the previous screen |
| **PS button** | Exit the application |

### Playback

| Controller input | Action |
|---|---|
| **X** | Pause or resume playback. Hold to restart a VOD from the beginning; live streams use a normal pause/resume press |
| **LEFT / RIGHT** | Seek backward or forward in a VOD; holding a direction accelerates the seek. Live streams do not support seeking |
| **UP / DOWN** | Decrease or increase the playback volume |
| **TRIANGLE** | Cycle through available subtitle tracks |
| **SQUARE** | Switch to another available video resolution |
| **L3** (left-stick click) | Cycle through available audio tracks when alternatives exist |
| **L1 / R1** | Play the previous / next video. If no next item is already queued, R1 can request the next recommendation |
| **L2** | Show the full playback information HUD |
| **R2** | Toggle crop-to-fill mode |
| **Right Analog Stick** | While crop-to-fill is enabled, move the cropped picture horizontally and vertically to adjust which part of the image is visible |
| **R3** (right-stick click) | Toggle playback statistics |
| **SELECT** | Open the video description overlay; use **UP / DOWN** to scroll and **SELECT** or **CIRCLE** to close |
| **START** | Open the chapter list; use **UP / DOWN** to select a chapter and **X** to jump to it. **START** or **CIRCLE** closes the list |
| **CIRCLE** | Return to the video list |

Live streams have no seek bar and cannot be downloaded. The description and chapter overlays temporarily take control of navigation while open.

## Build and installation

The application has two build paths:

- **Sony SDK project:** Uses the repository's existing PS3 development environment and shared build infrastructure.
- **PSL1GHT project:** An open-toolchain build intended for Linux, macOS, or the included GitHub Actions workflow.

For PSL1GHT dependencies, build commands, packaging, installation, and known platform differences, see [`apps/yo-player/BUILDING-PSL1GHT.md`](apps/yo-player/BUILDING-PSL1GHT.md).

For the application architecture, settings, storage files, playback behavior, and detailed controls, see [`apps/yo-player/README.md`](apps/yo-player/README.md).

## Known limitations

- YouTube changes its internal APIs and client restrictions frequently. Extraction compatibility may require future updates.
- Live streams use a rolling segment window. They start and exit differently from VOD and do not support normal seeking or background downloading.
- 1080p60 may fall back to 720p60 because of the PS3's video-decoding performance limits.
- Account feeds require a working network connection and valid Google authorization. Access can be revoked from the Google account security page.
- The SD scale toggle is intended for SD/CRT output; it is not a general display zoom control.

## Credits and upstream

This project builds on the work of:

- **mohasi** — original Yo! Player and the `ps3-dev` project, including the PS3 application foundation, shared libraries, rendering, and media playback infrastructure.
- **vortigauntlet** — the `yo-player-signin` fork and its YouTube account sign-in implementation.
- **openlewa** — the PSL1GHT Yo! Player port that forms part of the project's toolchain lineage.
- **PS3 homebrew and PSL1GHT contributors** — the open-source libraries and tools used by the project.

Upstream repositories:
- [This fork](https://github.com/patnog/yo-player-signin-crt)
- [Parent fork: yo-player-signin](https://github.com/vortigauntlet/yo-player-signin)
- [Original Yo! Player in ps3-dev](https://github.com/mohasi/ps3-dev/tree/main/apps/yo-player)
- [PSL1GHT Yo! Player project](https://github.com/openlewa/yo-player-psl1ght)

## License

See [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). The project retains the applicable upstream licensing and third-party notices. Check the individual component notices before redistributing bundled dependencies.
