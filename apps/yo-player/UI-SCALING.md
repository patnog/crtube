# UI Scaling: Developer Guide

This document describes the UI scaling behavior implemented by the graphics library and
Yo! Player. It is intended for anyone adding or modifying screens, widgets, labels, icons,
or overlays. The key rule is to treat **output resolution** and **display aspect ratio** as
separate properties.

## 1. When scaling is available

The graphics layer sets `sdVideoOutput` when the active video output resolution is 480 lines
(`res.height == 480`). This covers 480i and 480p, regardless of whether the PS3 output aspect
is 4:3 or anamorphic 16:9.

| PS3 output | UI Scaling setting | Effective scale | Layout |
|---|---|---:|---|
| 480i/480p, 4:3 | Available: 1.0x / 1.5x | Selected value | CRT-oriented 4:3 layout |
| 480i/480p, 16:9 | Available: 1.0x / 1.5x | Selected value | Standard widescreen layout |
| 720p/1080p, 16:9 | Unavailable | 1.0x | Standard widescreen layout |

The aspect ratio is read separately from `videoState.displayMode.aspect` and selects the
virtual canvas width. A 720x480 framebuffer does **not** imply 4:3: it can represent
anamorphic 16:9. Do not use `isGfxSdVideoOutput()` as an aspect-ratio test.

The user changes the value through **Home → Settings → UI Scaling**. The available values
are `1.0f` and `1.5f`; the first-launch default is 1.5x. The selection is saved separately
from the other settings:

- PSL1GHT: `/dev_hdd0/tmp/crtube/sd-ui-scale.txt`
- Sony SDK: `/dev_hdd0/tmp/crtube_sdk/sd-ui-scale.txt`

There is no L2 shortcut for changing the scale. On HD output, Settings displays the option
as unavailable and the effective scale remains 1.0x.

## 2. Graphics-layer behavior

Relevant implementation: `libs/simple-lib-app/src/gfx.c` and `libs/simple-lib-app/include/gfx.h`.

- `isGfxSdVideoOutput()` reports whether the active output is 480 lines.
- `getGfxSdUiScale()` returns the stored selection, even when the current output is HD.
- `getGfxUiScaleFactor()` returns the selected scale on 480-line output and 1.0 otherwise.
- `setGfxSdUiScale()` accepts values from 1.0 through 1.5. The app's Settings UI currently
  exposes only 1.0x and 1.5x.
- The internal `uiScale()` transform is active only when drawing to the display target and
  when the output is 480 lines. It scales UI geometry around the center of the current
  logical target (`targetW / 2`, `targetH / 2`).
- Offscreen render targets do not receive this geometry transform. Video frames drawn by
  `drawGfxYuvFrame()` are not enlarged by the UI scale.
- Overscan is a separate output-viewport transform and can affect the final presentation
  of both UI and video. Do not confuse it with UI Scaling.

The scale is not a global multiplier for every application value. It is a graphics-layer
transform plus explicit opt-ins in UI components. A component can therefore be only partly
scaled if its font, texture, dimensions, or custom drawing code does not participate.

## 3. Labels and text

Relevant implementation: `libs/simple-lib-app/src/ui/label.c` and
`libs/simple-lib-app/include/ui/label.h`.

Labels do not automatically opt in. Call `setLabelScaleWithUi(&label, 1)` for labels that
should follow the selected SD UI scale. The label renderer then re-rasterises the font at
the selected factor. In the current label implementation, an opted-in label is drawn with
`drawGfxTextureFixedPosition()`: its anchor is kept at the supplied `x`/`y`, and its texture
uses the dimensions produced by the larger/smaller font raster. This avoids applying the
geometry transform a second time to a texture whose raster dimensions already changed.
`refreshLabelScale()` can refresh an opted-in label when the selected factor changes; the
normal label drawing path also checks for scale changes.

Important details:

- `initLabel()` / `initLabelRaw()` alone do not enable UI scaling.
- `fixedRenderScale` overrides the selected factor for font rasterisation. Avoid setting
  it on labels that are expected to follow UI Scaling.
- `scalePositionWithUi` is a separate choice used by the fixed-render-scale helper path:
  it can move the label anchor with the UI transform while keeping the rasterised texture
  dimensions unchanged.
- Font rasterisation uses the selected scale, but the label's logical `width` and `height`
  remain the dimensions passed by the caller. Review wrapping, truncation and available
  space at both 1.0x and 1.5x.
- When labels are reused or updated dynamically, ensure their scale opt-in is not lost when
  the label is reinitialised.

## 4. Geometry, textures, and custom widgets

For ordinary display-target geometry, the graphics layer transforms drawing coordinates
around the center of the logical canvas. Do not manually multiply every x/y coordinate by
the UI factor as well; doing so can double-scale an element.

However, not every drawing helper treats texture dimensions identically. The graphics API
contains helpers for fixed-size textures and for fixed-size textures whose anchor position
is scaled. Choose deliberately:

- Use normal drawing for elements whose position and dimensions should follow the display
  UI transform.
- Use fixed-size drawing when the texture's raster dimensions and anchor must remain
  unchanged (the current `scaleWithUi` label path uses this behavior after re-rasterising
  its font).
- Use fixed-size-with-scaled-position drawing when only the anchor should follow the UI
  transform.

Inspect the helper implementation in `gfx.c` before selecting a mode. Do not assume that a
texture's position, width, height, and source raster are all scaled in the same way.

For custom widgets, check all of the following independently:

1. Position and anchor coordinates.
2. Width, height, padding, margins and corner radii.
3. Font raster size and text wrapping/truncation.
4. Icon and texture dimensions.
5. Hit-testing, focus/selection geometry and controller navigation bounds.
6. Clipping and safe-area margins near the screen edges.

A visually scaled element with unscaled hit-testing (or the reverse) is still a UI bug.

## 5. Thumbnails, video, and offscreen rendering

The UI scale is not intended to enlarge video content. Video frames and thumbnail source
images should retain their intended dimensions unless a screen-specific layout explicitly
changes their destination rectangle. Offscreen rendering is also intentionally excluded
from the global display-target transform. If an offscreen texture is later composited to the
screen, its final on-screen placement is subject to the normal display drawing rules, but
its contents are not retroactively re-rasterised.

Avoid applying the UI factor to thumbnail dimensions merely because labels beside the
thumbnail scale. Keep the image, badge, duration, title and metadata behavior consistent
with the existing `video-grid.c` implementation.

## 6. Playback is intentionally 1.0x

`apps/yo-player/src/screens/play.c` temporarily sets the selected UI scale to 1.0x while
rendering the playback screen, then restores the user's selected value. This keeps the seek
bar, playback HUD, subtitles, menus and overlays at their designed geometry. Do not remove
this override or assume the library/navigation scale should affect playback controls.

If a future feature intentionally changes playback scaling, it must be designed and tested
as a separate behavior rather than relying on the library UI scale.

## 7. Application-level examples

Existing opt-in points provide examples of the intended pattern:

- `video-grid.c`: title, metadata, duration and badges adapt to the selected scale.
- `sidebar.c`: sidebar labels and layout account for the scale.
- `screens/home.c`: home-screen labels and settings use the shared scale state.
- `screens/search.c`: search labels opt into scale-aware rendering.
- `downloads.c`: the download overlay label opts into scale-aware rendering.

These are examples, not a guarantee that every element in every screen scales automatically.

## 8. Developer checklist

Before merging a UI change, test on real hardware or representative output modes at both
available scales and across the relevant aspect ratios.

- [ ] 480i/480p 4:3 at 1.0x.
- [ ] 480i/480p 4:3 at 1.5x.
- [ ] 480i/480p anamorphic 16:9 at 1.0x.
- [ ] 480i/480p anamorphic 16:9 at 1.5x.
- [ ] HD 16:9 output, where the effective scale must remain 1.0x.
- [ ] No clipped text, unexpected wrapping, overlap or off-screen controls.
- [ ] Icons, badges, thumbnails and labels remain visually coherent.
- [ ] Focus/highlight and controller hit areas still match the visible controls.
- [ ] Playback HUD, subtitles, seek bar and playback menus remain at 1.0x.
- [ ] Overscan behavior remains independent and has not been mistaken for UI Scaling.

When in doubt, follow the existing component implementation and inspect the corresponding
rendering helper rather than adding manual scale multipliers.
