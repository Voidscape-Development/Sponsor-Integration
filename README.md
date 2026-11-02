# Sponsor Integration

An OBS Studio plugin with tooling for running sponsor and advertising content on stream.

Currently it provides a single source type: **Ad Playlist**.

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

Successful builds produce downloadable artifacts. To create a release, push a semver tag (e.g.
`1.2.3`) to the default branch; a draft release is created with installer packages attached.

Codesigning and notarization on macOS are covered in the template's
[Codesigning wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS).
