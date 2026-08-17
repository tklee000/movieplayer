# MoviePlayer 0.6

MoviePlayer is a native Windows x64 MP4/MKV/AVI/MPEG-TS player written in
C++17. Its
first-party media layer was implemented directly in C/C++ for this project.
The repository contains the container
parsers, sample indexing and seeking, codec interfaces, HEVC bitstream and
DXVA submission code, AAC-LC, FLAC, and AC-3 decoders, libopus-backed Opus
playback, channel mixer, subtitle parsers, and subtitle-audio resampler.

![MoviePlayer first screen](docs/movieplayer-first-screen.png)

## Download and run

- [Open the latest GitHub Release](https://github.com/tklee000/movieplayer/releases/latest)

GitHub Releases publish the portable ZIP and `SHA256SUMS.txt`. Extract the
complete ZIP and run `setup.exe` once for the most convenient first-time setup:
it registers the supported video extensions for the current user, downloads
and verifies the standard Whisper and M2M100 AI models, and opens Windows
Default Apps so the user can confirm the associations. The separately licensed
Japanese-to-Korean model is off by default and is offered only after a license
notice with explicit **Accept** consent. `MoviePlayer.exe` can also be run
directly without setup. Required VC142 runtime DLLs are placed
beside the application, so a system-wide Visual C++ Redistributable install is
not required.

## Highlights

| Area | MoviePlayer 0.6 |
|---|---|
| Platform | Native Windows 10/11 x64, Per-monitor V2 DPI, Unicode, and long-path awareness |
| Playback | Play/pause/stop, frame step, seek, loop, 0.5x-2.0x speed, volume/mute, fullscreen, always-on-top, and conservative next-episode matching |
| Windows integration | Drag and drop, command-line file open, and setup.exe/in-app per-user association registration for supported extensions |
| Rendering | Shared-device D3D11 video processing, optional NVIDIA 2× FRUC followed by RTX Video VSR, aspect-ratio-preserving presentation, and WARP fallback where supported |
| Audio clock | XAudio2 audio-master synchronization with an external-clock fallback when audio is absent or fails |
| UI languages | English, Japanese, Korean, French, German, Simplified Chinese, Traditional Chinese, Spanish, Portuguese, Hindi, Indonesian, and Arabic |
| AI subtitles | Optional native whisper.cpp recognition and CTranslate2/SentencePiece translation; model downloads are pinned and verified |

## Implementation boundary

| Layer | MoviePlayer uses it for |
|---|---|
| First-party C/C++ | MP4/MKV/AVI/MPEG-TS parsing, indexing and seeking, codec interfaces, HEVC syntax parsing and DXVA submission, AAC-LC, FLAC, and AC-3 decoding, channel mixing, resampling, subtitle handling, playback scheduling, and the Win32 UI |
| libopus | Matroska mono/stereo Opus decoding to 48 kHz float PCM |
| Windows Media Foundation | H.264, MPEG-4 Part 2, MPEG-2, WMV3, and Microsoft MPEG-4 v3 video decoding, plus MP3 audio decoding |
| External DirectShow audio decoder | E-AC-3 and DTS decoding through a compatible decoder registered on the system; external codec binaries are not linked or distributed |
| D3D11, DXVA, DXGI, and XAudio2 | Hardware video decode services, GPU video processing and presentation, software-rendering fallback, and audio output |
| Optional NVIDIA and native AI components | Optical Flow FRUC, RTX Video VSR, speech recognition, and translation |

The decoder and renderer share one D3D11 device. H.264 requests DXVA through
the Windows transform and can fall back to its software path. HEVC Main and
Main10 use MoviePlayer's bitstream parser and require the GPU's matching D3D11
Main/NV12 or Main10/P010 decode profile; there is no current software HEVC
fallback. NVIDIA frame interpolation and RTX Video VSR are optional; failures
return to the preceding D3D11 path without stopping playback.

See [the technical guide](docs/MoviePlayer-Wiki.md) for the full architecture,
implementation ownership, acceleration requirements, and fallback matrix.
The investigation and validation history for the H.264 A/V synchronization
fix is recorded in
[H.264 A/V Sync and Video Corruption Fix History](docs/H264-AV-Sync-Fix-History.md).

## Current playback scope

- Containers: non-fragmented MP4 with `moov`, `stbl`, 32/64-bit chunk offsets,
  decode/composition timing, sync samples, `avcC`, `hvcC`, and `esds`; plus
  focused Matroska/MKV playback with `SeekHead`, `Cues`, clusters, block groups,
  and fixed/Xiph/EBML lacing for H.264/HEVC,
  AAC/Opus/FLAC/AC-3/E-AC-3/DTS, and
  text/bitmap subtitle tracks; RIFF AVI with classic `idx1` indexing or a
  bounded `movi` scan fallback; and 188/192/204-byte MPEG transport streams
  with PAT/PMT/PES discovery. The container reader is selected from the file
  signature first, so supported MP4, Matroska, AVI, or MPEG-TS content can open
  even when its filename has an incorrect extension.
- Video: H.264 from supported MP4 (`avc1`/`avc3`), AVI, and MPEG-TS inputs
  through the Windows Media Foundation decoder, including B-frame MP4 streams
  without composition offsets and the tested High Profile Level 4.2 title;
  and HEVC Main/Main10 4:2:0 streams matching the supplied x265 test title.
  Windows decoders also handle MPEG-4 Part 2, MPEG-2 video, WMV3, and Microsoft
  MPEG-4 v3 for the supported MP4, AVI, and MPEG-TS paths. Media Foundation
  NV12 output is preferred; supported YUY2 fallback output is normalized to
  NV12 before presentation through the D3D11 video processor.
- Audio: AAC-LC at 24, 44.1, and 48 kHz, including spectral Huffman decoding,
  inverse quantization,
  stereo tools, TNS, IMDCT/window overlap, native stereo playback,
  5.1-to-stereo mixing, PCE 7.1-to-stereo mixing, and XAudio2; Matroska
  mono/stereo Opus through the BSD-licensed libopus 1.5.2 decoder; plus Windows
  Media Foundation MP3 decoding for AVI; first-party Matroska FLAC decoding
  for 4- through 32-bit streams with up to eight channels and stereo output;
  and first-party MP4/Matroska/AVI/MPEG-TS AC-3 decoding with mono through 5.1
  input and stereo downmix. MPEG-TS AAC is extracted from ADTS frames.
  E-AC-3 and DTS tracks in supported Matroska or MPEG-TS files use a compatible
  external DirectShow audio decoder registered by the user and are converted
  to stereo PCM for playback.
- Seeking: MP4 sync-sample, MKV cue, AVI keyframe/index-or-scan, and MPEG-TS
  approximate byte-position seek followed by the next detected keyframe, with
  decoder and playback-clock reset.
- Subtitles: external SRT, WebVTT, ASS/SSA, and SMI/SAMI display; Matroska
  embedded `S_TEXT/ASS`, `S_TEXT/SSA`, `S_TEXT/UTF8`, and DVD VobSub
  (`S_VOBSUB`) bitmap subtitles with zlib decompression; plus an optional local
  native AI transcription/translation worker. The worker reuses the built-in
  MP4/AAC stack and a 63-tap 48 kHz-to-16 kHz FIR resampler.
- Scaling: D3D11 video processing and optional NVIDIA RTX Video VSR.

The H.264 and HEVC paths are focused playback implementations for ordinary
consumer video files commonly distributed online. They are not complete
implementations of every profile, level, chroma format, bit depth, container
combination, or optional bitstream feature in those standards. Unsupported or
unusual files may fail to open or decode. A compatible Windows hardware HEVC
Main or Main10 decoder is required for HEVC playback; H.264 uses the Windows
Media Foundation decoder and requests DXVA acceleration when available. The
native AAC-LC, FLAC, and AC-3 decoders are similarly scoped to the formats
listed above.

## Playback and Windows integration

- Open media from the file dialog, Windows command line, or drag and drop.
- Play, pause, stop, step one frame, seek by 10 seconds or with the seek bar,
  mute, adjust volume, loop, and select 0.50x, 0.75x, 1.00x, 1.25x, 1.50x, or
  2.00x speed.
- Toggle fullscreen with `F`, `F11`, `Enter`, or `Alt+Enter`; use `Esc` to
  leave fullscreen. Always-on-top is available from the View menu.
- Optionally open the next episode in the same directory. Matching recognizes
  `E`/`EP` episode tokens such as `E03` to `E04` and rejects candidates whose
  surrounding title tokens are not sufficiently similar.
- Select audio and embedded subtitle tracks from their menus, load an external
  subtitle explicitly, or ask MoviePlayer to find a matching subtitle beside
  the video.
- Register `.mp4`, `.mkv`, `.avi`, `.ts`, `.m2ts`, and `.mts` for the current
  user with `setup.exe` or from MoviePlayer, then choose defaults in Windows
  Settings. Windows retains control of the user's default-app selection, so a
  portable helper must not silently replace an existing default player.

## Twelve UI languages

The interface can be switched without restarting or closing the current video.
The selected code is stored under `HKCU\Software\MoviePlayer`, and missing
catalog entries fall back to English.

| Code | Language | Code | Language |
|---|---|---|---|
| `en` | English | `ja` | Japanese |
| `ko` | Korean | `fr` | French |
| `de` | German | `zh-CN` | Simplified Chinese |
| `zh-TW` | Traditional Chinese | `es` | Spanish |
| `pt` | Portuguese | `hi` | Hindi |
| `id` | Indonesian | `ar` | Arabic |

Validate every catalog after changing translations:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\validate_languages.ps1
```

## Local AI subtitle generation

The Subtitle menu exposes **Generate AI Subtitles...**. Speech-language
detection is automatic, and the current UI language is used as the requested
subtitle language. The compiled worker and native runtime libraries ship with
MoviePlayer; only the large model weights are downloaded separately.
`setup.exe` installs the standard models automatically during first-time setup.
The command file remains available for model-only installation or repair:

```powershell
.\install_ai_models.cmd
```

The current worker input path requires an MP4 file containing a supported
48 kHz AAC track. Its fully local pipeline is:

1. MoviePlayer's MP4 reader and AAC-LC decoder produce stereo PCM, which a
   63-tap FIR resampler converts from 48 kHz to 16 kHz mono.
2. whisper.cpp 1.9.1 runs `large-v3-turbo`, detects the speech language, and
   creates timestamped segments.
3. The original transcript is preserved beside the video as
   `<video>.<source>.whisper.srt`.
4. When source and target differ, CTranslate2 4.8.1 and SentencePiece 0.2.1 run
   the pinned M2M100 418M int8 translation model on the CPU.
5. Output is written through a temporary `.part` file and atomically replaces
   the final UTF-8 SRT, so an interruption does not leave a misleading finished
   subtitle.

Supported AI output targets are `en`, `ja`, `ko`, `fr`, `zh-CN`, `zh-TW`, `es`,
`pt`, `hi`, `id`, and `ar`. German is available for the application UI but is
not currently an AI subtitle output target. Simplified-to-Traditional Chinese
conversion is applied when `zh-TW` is requested.

The default model installer pins and verifies these publisher revisions,
required sizes, and SHA-256 hashes:

| Model | Revision | Purpose |
|---|---|---|
| `ggerganov/whisper.cpp` `ggml-large-v3-turbo.bin` | `5359861c739e955e79d9a303bcbc70fb988958b1` | Multilingual speech recognition |
| `gn64/M2M100_418M_CTranslate2` | `18e406c615ef2991fa74d53734bf66b0a6b10cb4` | Offline multilingual translation |

The optional `Hunhee/argos-ko-ja` Japanese-to-Korean native model is offered by
`setup.exe` with an off-by-default choice and a separate **Accept** license
dialog. It can also be installed directly after explicit acceptance of its
publisher-declared CC BY-NC 4.0 terms:

```powershell
.\install_japanese_translation_model.cmd
```

MoviePlayer prefers that model only for detected Japanese speech with Korean
output; incomplete or failed installations fall back to M2M100. After model
installation, recognition and translation do not send media or subtitles to a
cloud API. Generated text can still contain errors, so important subtitles
should be reviewed manually. See
[`tools/whisper/README.md`](tools/whisper/README.md) for worker details.

## NVIDIA RTX Video Super Resolution

Enable **View > NVIDIA RTX Video AI Upscaling (VSR)**. MoviePlayer invokes VSR
only when the source is smaller than the output and composites subtitles after
the upscale pass. Unsupported hardware, drivers, formats, or runtime failures
return automatically to normal D3D11 scaling without interrupting playback.

The SDK runtime is proprietary and is not covered by MoviePlayer's MIT License.
A compatible 64-bit Windows system, NVIDIA RTX GPU, current driver, and the
packaged NVIDIA runtime are required; review the NVIDIA terms before building
or redistributing it.

## NVIDIA 2x frame interpolation

Enable **View > NVIDIA 2x Frame Interpolation** for progressive
29-31 FPS content. MoviePlayer acquires the next decoded frame half an input
frame early, asks the NVIDIA Optical Flow SDK FRUC library for the midpoint,
and presents the midpoint and original at their respective timestamps. When
VSR is also enabled the fixed processing order is **FRUC first, VSR second**,
so both the generated midpoint and original frame are AI-upscaled before
subtitle composition and presentation.

Optical Flow SDK 5.0 is a separately licensed NVIDIA download requiring a
Developer Program login and explicit license acceptance. MoviePlayer does not
accept those terms or download it automatically. After downloading the
official `optical_flow_sdk_5.0.7.zip`, normalize the needed header and runtime:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  .\scripts\setup_nvidia_optical_flow_sdk.ps1 `
  -ArchivePath C:\path\to\optical_flow_sdk_5.0.7.zip
```

Without that package, the source still builds and the menu reports why FRUC
is unavailable. FRUC requires Windows 10 or later, an NVIDIA Turing-or-newer
GPU with Optical Flow support, and a current NVIDIA driver. Only progressive
Progressive 23-31 FPS input is doubled (for example, 23.976 to 47.952 FPS or
29.97 to 59.94 FPS); other frame rates retain their normal timing.

## Codec layout

```text
src/codec/
  core/                  bounds-checked readers, file I/O, shared media types
  container/
    MediaDemuxer.h       container-neutral interface
    avi/                 RIFF AVI idx1 reader with movi scan recovery
    mkv/                 focused Matroska EBML/Cluster/Cues reader
    mp4/                 ISO Base Media parser and sample-table index
    ts/                  MPEG transport stream PAT/PMT/PES reader
  audio/
    AudioDecoder.h       audio decoder interface
    aac/                 AAC-LC decoder and standard Huffman tables
    ac3/                 first-party ATSC A/52 AC-3 decoder
    flac/                first-party IETF RFC 9639 FLAC decoder
    directshow/          external DirectShow adapter for E-AC-3/DTS
    mp3/                 Windows Media Foundation MP3 backend
    opus/                libopus-backed Matroska Opus decoder
  subtitle/              embedded UTF-8/ASS text decoder
  video/
    VideoDecoder.h       video decoder interface shared by MF and HEVC paths
    h264/                Media Foundation backend for H.264 and legacy video
    hevc/                HEVC syntax parser and D3D11/DXVA backend
```

## Build from a clean clone

Requirements:

- Windows 10 or 11 x64
- Visual Studio 2019 16.11 with **Desktop development with C++** and the v142
  x64 toolset
- CMake bundled with Visual Studio
- PowerShell 5.1 or later, `curl.exe`, `git.exe`, and internet access for the
  first dependency download
- NVIDIA RTX Video SDK files when VSR is built
- Pinned native AI source dependencies for the subtitle worker

The supplied build selects `Visual Studio 16 2019`, `-A x64`, and `-T v142`;
Visual Studio 2022 is not used as a substitute. From a clean checkout:

```powershell
git clone https://github.com/tklee000/movieplayer.git
cd movieplayer
```

Runtime playback of supported Matroska or MPEG-TS E-AC-3 and DTS uses any
compatible external DirectShow audio decoder already registered on the system.
MoviePlayer does not install, bundle, or link an external codec; codec
installation and registration remain under the user's control. AC-3, E-AC-3,
and DTS tracks are selectable in the audio-track menu and are not labeled
`[Not Support]`.

Review the applicable third-party terms, set up the pinned dependencies, and
build Release:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_rtx_video_sdk.ps1
# After separately downloading and accepting the Optical Flow SDK terms:
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_nvidia_optical_flow_sdk.ps1 -ArchivePath C:\path\to\optical_flow_sdk_5.0.7.zip
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_opus.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_native_ai_dependencies.ps1
.\build.cmd -Configuration Release
```

`build.ps1` automatically installs the publicly downloadable RTX Video and
source dependencies when required. It never downloads Optical Flow SDK because
that download requires an NVIDIA login and explicit license acceptance.
Downloaded SDKs, upstream source trees, model weights, build products, release
packages, and test media are ignored by Git.

The build creates `build-vs2019\Release\MoviePlayer.exe` and the native subtitle
worker, copies `ctranslate2.dll`, NVIDIA VSR, and the app-local VC142 runtime
DLLs, and validates the release imports. Run the codec smoke test explicitly
with:

```powershell
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -version '[16.0,17.0)' -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --build build-vs2019 --config Release --target MovieCodecSmoke
.\build-vs2019\Release\MovieCodecSmoke.exe "D:\path\video.mp4"
```

Create the portable ZIP for GitHub Releases with:

```powershell
.\create_release.cmd
```

The release command rebuilds a fresh model-free deployment, requires the
licensed NVIDIA Optical Flow FRUC runtime, validates RTX VSR, 2x frame
interpolation, setup.exe, language catalogs, and app-local VC142 libraries, and
then writes the ZIP plus its SHA-256 checksum. After copying an extracted
folder, run `verify_portable.cmd` to recheck its layout. A developer-only build
without FRUC can be created explicitly with
`scripts\create_deploy.ps1 -AllowMissingFrameInterpolation`; it is not accepted
by the standard release command.

## Source layout

| Path | Contents |
|---|---|
| `src/` | Win32 UI, player engine, renderer, RTX VSR, localization, subtitles, and application resources |
| `src/codec/` | First-party demuxers, codec interfaces, audio decoders, HEVC/DXVA code, and subtitle decoders |
| `languages/` | Twelve external UTF-8 UI catalogs |
| `scripts/` | Verified dependency setup, portable validation, deployment, and release packaging |
| `tools/whisper/` | Native AI worker protocol and model-layout documentation |
| `third_party/` | Tracked placeholders; downloaded SDKs, libraries, sources, and models are ignored |
| `tests/` and `test-assets/` | Codec smoke test and test-media guidance |
| `docs/` | Technical guide and public screenshot |

## Keyboard shortcuts

| Key | Action |
|---|---|
| `Ctrl+O` | Open a video |
| `Space` | Play or pause |
| `Left` / `Right` | Seek backward or forward 10 seconds |
| `Up` / `Down` | Increase or decrease volume |
| `M` | Mute or unmute |
| `L` | Toggle loop playback |
| `F`, `F11`, `Enter`, or `Alt+Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen |

## Current limitations

- The player intentionally supports the container/codec combinations listed
  above rather than every variation accepted by a general-purpose framework.
- HEVC Main/Main10 playback has no software fallback and requires a matching
  D3D11/DXVA decoder profile.
- E-AC-3 and DTS require a compatible DirectShow decoder registered by the
  user; MoviePlayer does not bundle one.
- AI subtitle input currently requires MP4 with supported 48 kHz AAC audio,
  and German is not currently an AI output target.
- Recognition and translation are resource intensive, and output quality
  depends on recording quality, speakers, language pair, names, and terminology.
- RTX Video VSR availability depends on compatible NVIDIA hardware, drivers,
  input/output conditions, and the proprietary runtime.
- NVIDIA 2× interpolation additionally requires progressive 23-31 FPS
  content and the separately installed Optical Flow SDK 5.0 FRUC runtime.
- Windows 7/8 and 32-bit Windows are not supported.

## Third-party and legal notices

MoviePlayer first-party source is licensed under the [MIT License](LICENSE).
libopus, Windows components, NVIDIA software, native AI libraries, and model
weights retain their own licenses and terms. The MIT License does not relicense
those components or grant codec patent or media-content rights.

Read [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), the packaged `licenses`
directory, and `third_party/whisper/LICENSES.md` before redistribution. The
optional Japanese-to-Korean model remains separate because of its declared
non-commercial terms. These documents are engineering compliance material,
not legal advice.
