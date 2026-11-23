# Asteroid Fusion

Mobile-first video compositor with a Fusion-style node graph, a CapCut/KineMaster-style touch UI and Lua scripting.
Native C (raylib + Lua 5.4 + NanoSVG), no Java, no Gradle. Formerly **HMY AFX**.

> Indonesia: editor video & motion graphics untuk Android (portrait diutamakan, landscape didukung), dengan
> graf node ala DaVinci Fusion dan scripting Lua. Lihat bagian *Status* untuk apa yang sudah diuji.

## What it does

- **Clips and layers**: video, image, SVG, text, shapes, solids, asteroid, starfield and adjustment layers on a timeline,
  every property keyframeable (ease in/out/hold) or driven by a Lua expression.
- **Fusion node graph** per clip: `Source -> effects / transforms / merge -> Output`. Generators (video, image, shape,
  text...) can be stacked on top with a Merge node (opacity, offset, blend: normal/add/multiply/screen).
  The renderer memoizes per node and reuses buffers, so only what changed is recomputed.
- **Video import** through a proxy frame sequence (JPEG, short side 540 px by default) with a background prefetch thread
  and a small frame cache, so scrubbing and playback stay smooth. Desktop uses ffmpeg/ffprobe, Android uses the NDK
  `MediaExtractor` + `MediaCodec`.
- **Touch UI**: preview on top, timeline in the middle, contextual toolbar and bottom sheets. Portrait first;
  landscape switches to a side panel. One responsive layout, one code base.
- **Scripting**: a Lua 5.4 API with a time limit, usable from the Script tool or from the project file itself.
- **Export**: GIF, PNG/JPG frames, and MP4/WebM/AVI.
  Desktop: ffmpeg, with the audio of video clips mixed in (trim, speed and volume are honoured).
  Android: H.264 MP4 through `MediaCodec` + `MediaMuxer` (video only for now, see *Status*).

## Status (read this first)

| Area | State |
| --- | --- |
| Desktop (Linux) build, UI, Lua API, graph render, video import, MP4 export with audio | built and exercised in a headless test session |
| Project save/load of graphs (`graph={...}` round trip) | tested: save -> load -> save is byte-stable and renders identically |
| Android APK | compiles, links, signs and passes `apksigner`/16 KB `zipalign` checks; **not run on a physical device yet** |
| Android `MediaCodec` import, `MediaCodec`/`MediaMuxer` export, media permission prompt | implemented, **untested on hardware**: please report device + Android version when something fails |
| Android export audio | not implemented (video track only) |
| Audio preview while editing | not implemented (audio is mixed at export time only) |
| Saving exports to the gallery | Android 10+: MediaStore into `Movies/AsteroidFusion` (videos) and `Pictures/AsteroidFusion` (GIF/PNG/JPG); Android 8-9: direct copy. Written from reports of the first device test, still unverified |

## Build

### Desktop (Linux)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target afx
./build/afx                      # editor
./build/afx --render project.lua out.png --frame 10
./build/afx --render project.lua out.mp4
./build/afx --convert in.lua out.lua   # load, normalize, save
```

Needs a C compiler, CMake, X11/GL development packages for GLFW, and `ffmpeg` + `ffprobe` in `PATH` for video import and
video export.

### Android (pure native APK)

```sh
# Android SDK with NDK 28.1.13356709, build-tools 36.0.0, platform android-34 (override with ANDROID_HOME)
ABIS="arm64-v8a" android/build.sh   # one ABI, fast
android/build.sh                    # arm64-v8a, armeabi-v7a, x86_64
```

The script uses NDK clang -> `aapt2` -> `zipalign` (16 KB pages) -> `apksigner`. A signing key is generated on the first
run into `android/release.keystore` (never committed). **Releases of this project are signed with the maintainer's key;
a build you make yourself carries a different signature and cannot be installed over an official one.**

## Lua scripting

```lua
afx.comp{ name="demo", width=1080, height=1920, fps=30, duration=6, bg={8,10,22,255} }

local clip = afx.video("/sdcard/Movies/trip.mp4")      -- path, fit="fill"|"fit", trim=, speed=, volume=
clip.speed = 1.5

local g = clip:graph()                                  -- {source=id, output=id}
local fx  = clip:node("glow", { input = g.source })     -- effect name, "transform", "merge" or a layer type
local top = clip:node("shape", { shape="star", fill="#ff9a2e" })        -- generator node
local mg  = clip:node("merge", { input = fx, input2 = top, blend = "add", opacity = 90 })
clip:link(mg, g.output)

local title = afx.text{ text = "Asteroid Fusion", fill = "#ffffff", y = -600 }
title:key("opacity", 0, 0); title:key("opacity", 0.5, 100)
title.expr = { rot = "wiggle(2, 5)" }
```

Node API on a layer: `graph()`, `node(kind, props)`, `nodeset(id, props)`, `link(from, to[, slot])`, `unlink(to[, slot])`,
`unnode(id)`, `ungraph()`. Layer constructors: `solid shape text svg image video asteroid stars adjust`.
Type `help()` in the Script tool for the full list.

Project files are plain Lua. Layers and graphs are written above the `--@user` marker; anything below it is your own script.

## Layout

```
src/        engine (core, render, graph, media, export, script) and UI (ui_core, ui_sheets, ui_fusion, ui_main)
android/    manifest, icon resources, build.sh
assets/     fonts and sample SVGs
vendor/     raylib, Lua 5.4, NanoSVG, stb
```

## Third-party code

raylib (zlib), Lua (MIT), NanoSVG (zlib), stb (public domain / MIT), DejaVu fonts (Bitstream Vera license).
Their license files stay in `vendor/` and `assets/fonts/`.
