# Sponsor Integration

An OBS Studio plugin with tooling for running sponsor and advertising content on stream.

It provides two source types: **Ad Playlist** and **Picture Gallery**.

## Ad Playlist

A playlist source in the spirit of Image Slide Show or VLC Video Source, except it cycles through
**videos and images in the same list** and transitions between them.

* Videos always play their full roll; images hold for a configurable amount of time.
* A configurable transition runs between items, driven either by the end of a video or by the image
  hold timer.
* Looping, shuffling, manual next/previous, and OBS media controls.

### Playlist

Add files with the **Playlist** list. Anything OBS can decode works — `.mp4`, `.mov`, `.mkv`,
`.webm`, `.ts`, `.avi`, `.flv`, audio-only files, and images (`.png`, `.jpg`, `.gif`, `.webp`, …).
Adding a directory pulls in every supported file inside it, sorted by name so the running order is
the same on every machine and every launch. A directory is read when the playlist is edited and when
the source is loaded, not on every settings change, so files dropped into it while OBS is running
are picked up the next time the list is touched.

Items are recognised by extension: known image extensions are loaded as image sources, everything
else is loaded as a media source.

### Playback

| Setting | Description |
|---|---|
| **Image hold time** | How long a still image stays on screen. Videos ignore this and play to their end. |
| **Loop playlist** | Start over after the last item. With looping off the playlist stops on the last item. |
| **Shuffle order** | Shuffles the running order rather than picking at random, so every spot plays exactly once per pass. The order is reshuffled each pass, never repeating an item across the loop point. |
| **Hide when the playlist ends** | With looping off, transition out to nothing at the end instead of holding the last frame. |
| **Restart playlist when the source becomes visible** | Start from the first item every time the source is shown. With this off, showing the source resumes at the item it was on. |
| **Keep media files loaded** | Keeps each video open and its first frame decoded so items start instantly. Turn it off for very large playlists to save memory, at the cost of a short load when each item comes up. |
| **Use hardware decoding** | Passed through to the underlying media sources. |

The playlist only advances while the source is visible. A hidden Ad Playlist holds its position
instead of burning through the rotation off screen.

### Audio

The Ad Playlist is a full audio source: it shows up in the OBS **Audio Mixer** with its own fader,
mute, meter, monitoring, audio filters and track routing, just like a Media Source. The items'
audio is mixed by the playlist itself and follows the transition, so a fade crossfades the audio,
Fade to Color dips through silence at its switch point, and a Stinger fades the outgoing item out by
its transition point and the incoming one back in (its own sound plays on top).

| Setting | Description |
|---|---|
| **Volume** | Level of the whole playlist, applied before the mixer fader. Takes effect immediately without interrupting the item on screen. |
| **Even out loudness between items** | *(on by default)* Measures each file's loudness the way broadcast does (ITU-R BS.1770 / EBU R128 — K-weighted and gated, so silence and quiet pauses don't count) and brings it toward the target: quiet spots are turned up, loud spots are turned down. |
| **Target loudness** | The level every item is brought toward, in LUFS. Default −18. |
| **Maximum boost** | How far a quiet file may be turned up, in dB. Default 12. Loud files can always be turned down (up to 30 dB). Boosting is held back wherever it would push a file into clipping. |

A file is measured while it plays and its level eases in over its first couple of seconds. The
measurement is remembered for the rest of the session, so from its second time around a file starts
at exactly the right level. Each file's measured loudness and the gain applied are written to the
OBS log when it leaves the screen.


Choose between **Cut**, **Fade**, **Swipe**, **Slide**, **Fade to Color** and **Stinger**. The
options for the selected transition (direction, color, stinger video and transition point, …) appear
underneath it.

**Transition timing** decides when the transition fires:

* **Overlap the end of the item** *(default)* — the transition starts early enough to land exactly
  on the item's last frame, so a video's tail plays underneath the transition and an image is on
  screen for exactly its hold time. For a stinger, "early enough" means the stinger's transition
  point, so the incoming item is swapped in behind the stinger at the right moment.
* **Start after the item finishes** — the item plays out first and the transition runs afterwards.
  Each cycle then takes the item's duration *plus* the transition.

### Appearance

**Bounding size / aspect ratio** is either `Automatic` (the source grows to fit the largest item in
the playlist), a fixed resolution such as `1920x1080`, or an aspect ratio such as `16:9`. Items are
placed inside those bounds using the **Scale mode** and **Alignment** settings.

Automatic sizing never shrinks during a session, because a video only reports its dimensions once it
has been opened, and a scene item that resizes itself mid-rotation is worse than one that is
slightly too large. For playlists mixing several resolutions, set a fixed bounding size.

### Controls

The source is a controllable media source, so it appears in the **Media Controls** dock with
play/pause, restart, stop, next and previous, and the timeline shows the progress of the current
item. The same five actions are available as assignable hotkeys.

### Automation

The source exposes procedures and signals for scripts and `obs-websocket`:

| | |
|---|---|
| proc `current_index(out int current_index)` | Index of the current item in the playlist, or `-1`. |
| proc `current_path(out string current_path)` | File path of the current item. |
| proc `total_items(out int total_items)` | Number of items in the playlist. |
| signal `ad_item_changed(int index, int position, string path, bool is_video)` | An item started. `index` is its place in the playlist, `position` its place in the (possibly shuffled) running order. |
| signal `ad_playlist_ended()` | A non-looping playlist reached its end. |

## Picture Gallery

An image rotation in the spirit of Image Slide Show, where **every image comes in with a Show
transition and goes out with a Hide transition**, with a Normal and a Loop wrap transition alongside
and an optional **gap** between images.

### Images

Add files or directories with the **Images** list. Any image OBS can load works — `.png`, `.jpg`,
`.gif`, `.webp`, `.bmp`, `.tga`, `.psd`, `.tif`, … Directories are expanded and sorted by name, and
are read when the list is edited and when the source is loaded. Animated GIFs/WebPs loop for as long
as they are on screen.

### Playback

| Setting | Description |
|---|---|
| **Hold time** | How long each image is fully on screen. Transitions come on top of this. |
| **Gap between images** | `0` *(default)*: the next image comes in with the Show transition while the last one goes out with the Hide transition, both at once. Above `0`, each image goes out with Hide, the gallery stays empty (transparent) for the gap, and the next image comes in with Show. |
| **Loop gallery** | Start over after the last image. |
| **Shuffle order** | Shuffles the running order so every image comes up exactly once per pass, without repeating across the loop point. |
| **Hide when the gallery ends** | With looping off, go out with the Hide transition at the end instead of holding the last image. |
| **Restart gallery when the source becomes visible** | Start from the first image whenever the source is shown. With this off, it picks up at the image it was on. |
| **Use Show/Hide only when the gallery comes in or goes out** | Off by default. On, images change with the Normal transition and Show/Hide only play when the gallery comes in from or goes out to nothing (starting, the Show/Hide controls, Stop, the end, and around a gap) — the way the Source Switcher plugin uses them. |

The gallery only advances while the source is visible.

### Transitions

Each of the four directions has its own transition, duration and options:

| Direction | Plays when |
|---|---|
| **Show** (→ image) | Every image coming in: as it replaces the last one, when the source becomes visible, when the gallery is started, restarted or shown again with the Show control, and after a gap. |
| **Hide** (image →) | Every image going out: as the next one replaces it (drawn on top of the incoming image), going into a gap, Stop, the Hide control, and the end of a gallery set to hide when it ends. |
| **Normal** (image → image) | One image straight to the next when Show and Hide are both *None* — or always, with *Use Show/Hide only when the gallery comes in or goes out* on. |
| **Loop wrap** (→ first image) | A looping gallery goes from its last image back to its first: it brings the first image in in place of Show (or Normal, with Show/Hide only at the edges). *None* by default. |

Any direction can be set to **None** to leave it unused, and the gallery works out what to play
instead:

| Show | Hide | Image → image (no gap) |
|---|---|---|
| set | set | The old image goes out with Hide on top while the new one comes in with Show. |
| set | None | Show plays straight from the old image to the new one. |
| None | set | The new image is there underneath while the old one goes out with Hide. |
| None | None | The Normal transition plays from one image to the other. |

When the gallery comes in from or goes out to nothing (starting, a gap, Stop, the Show/Hide
controls) a Show or Hide set to *None* uses the Normal transition. Normal set to *None* is a cut.
Loop wrap set to *None* means the loop point is treated like any other change.

Every direction can use **Cut**, **Fade**, **Swipe**, **Slide**, **Fade to Color** or **Stinger**, or
**OBS scene transition** to use one of the transitions set up in the Scene Transitions dock (Move and
other third-party transitions included). The plugin finds the OBS frontend at runtime rather than
linking it, so it also loads in builds of OBS without one; there, the scene transition option is
simply not offered. The gallery plays its own copy of a scene transition, with
the same settings, and follows any change made to them. If a chosen scene transition is renamed or
removed, that direction falls back to a fade (with a note in the log) until one by that name exists
again.

OBS hides a scene item instantly, so the eye icon cannot play the Hide transition; hiding and
showing the source that way cuts out, and the Show transition plays when it comes back. To animate
the gallery out on demand use the **Hide Picture Gallery** hotkey (or the `hide_gallery` proc), or
set OBS's own Show/Hide transition on the scene item.

A stinger's own sound plays through the scene the gallery is in.

### Appearance

**Bounding size / aspect ratio**, **Scale mode** and **Alignment** work as they do for the Ad
Playlist.

### Controls

The gallery appears in the **Media Controls** dock: play/pause (freezes the hold timer), restart,
stop (goes out with Hide), next and previous (the old image goes out with Hide as the next comes in
with Show, as on any other change — or with the Normal transition with Show/Hide only at the edges;
a manual skip never waits out the gap). Hotkeys are available for all of those plus **Show**
and **Hide**, which take the gallery out with its Hide transition and bring it back with its Show
transition, keeping its place.

Only **Show** brings back a gallery taken out with **Hide**. Restart, play after stop, and the source
becoming visible again with *Restart gallery when the source becomes visible* on all move it back to
the first image, but keep it out until it is shown.

### Automation

| | |
|---|---|
| proc `current_index(out int current_index)` | Index of the current image in the list, or `-1`. |
| proc `current_path(out string current_path)` | File path of the current image. During a gap, the image coming up next. |
| proc `total_items(out int total_items)` | Number of images. |
| proc `show_gallery()` | Bring the gallery back after `hide_gallery`. |
| proc `hide_gallery()` | Take the gallery out with its Hide transition and keep it out. |
| proc `is_hidden(out bool hidden)` | Whether the gallery was taken out with `hide_gallery`. |
| signal `gallery_image_changed(int index, int position, string path)` | An image started coming in. |
| signal `gallery_ended()` | A non-looping gallery reached its end. |

## Building

This project uses the OBS plugin template build system.

| Platform | Tool |
|-----------|--------|
| Windows | Visual Studio 17 2022 |
| macOS | Xcode 16.0 |
| Windows, macOS | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3, `ninja-build`, `pkg-config`, `build-essential` |

```sh
cmake --preset ubuntu-x86_64   # or macos / windows-x64
cmake --build build_x86_64
```

### Runtime tests

`tests/` holds tests that run both sources in real libobs, headless under Xvfb, with the stock image,
transition and media plugins, and check what they signal and what they actually render. They cover
the gallery's transitions, gap, loop wrap and end; its controls; the Hide control surviving a restart;
changing settings while it runs; OBS scene transitions (the test stands in for the OBS frontend);
a stinger longer than any timeout; the Ad Playlist's rotation; and relinking missing files in both
sources. Every scenario also fails on any leaked allocation.

On Ubuntu with OBS Studio, `xvfb`, `python3` and (for the stinger test) `ffmpeg` installed:

```sh
cmake --preset ubuntu-x86_64 -DENABLE_RUNTIME_TESTS=ON
cmake --build build_x86_64
tests/run-runtime-tests.sh build_x86_64            # or name scenarios: ... build_x86_64 gap controls
```

They also run on every pull request and push (`runtime-tests` workflow).

Build documentation for the underlying template lives in the
[Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki):
[Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started),
[Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements),
[Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options).

### GitHub Actions & CI

Default workflows are provided for the following repository actions:

* `push`: commits or tags pushed to `master` or `main`.
* `pr-pull`: a pull request has been pushed or synchronized.
* `dispatch`: triggered from GitHub's workflow dispatch UI.
* `build-project`: builds the project, triggered by the other workflows.
* `check-format`: checks CMake and source formatting, triggered by the other workflows.
* `runtime-tests`: builds with the runtime tests and runs them, triggered by `pr-pull` and `push`.

Successful builds produce downloadable artifacts. To create a release, push a semver tag (e.g.
`1.2.3`) to the default branch; a draft release is created with installer packages attached.

Codesigning and notarization on macOS are covered in the template's
[Codesigning wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS).
