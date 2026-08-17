# MoviePlayer 0.6 binary package

Keep this folder together when moving it to another Windows 10/11 x64 PC. Run
`setup.exe` once for the easiest first-time setup, or run `MoviePlayer.exe`
directly. Required VC142 runtime DLLs are supplied beside the application, so a
system-wide Visual C++ 2019 Redistributable installation is not required. Keep
the supplied VC142 DLLs, `nvngx_vsr.dll`, `NvOFFRUC.dll`, `cudart64_*.dll`,
`languages`, `tools`, `scripts`, and `licenses` beside the executable. Run
`verify_portable.cmd` after copying to validate the runtime layout.

`setup.exe` does not copy the application into Program Files and does not need
administrator privileges. It registers `.mp4`, `.mkv`, `.avi`, `.ts`, `.m2ts`,
and `.mts` for the current user, installs the standard AI models described
below, and opens Windows Default Apps so the user can confirm the desired
associations. It then offers the Japanese-to-Korean model as an off-by-default
option and installs it only after showing its third-party terms and receiving
explicit **Accept** consent.

## Playback implementation and codec scope

| Layer | Responsibility |
|---|---|
| First-party MoviePlayer code | MP4/MKV/AVI/MPEG-TS parsing, indexing and seeking, HEVC bitstream parsing and DXVA submission, AAC-LC/FLAC/AC-3 decoding, subtitle handling, channel mixing, and resampling |
| BSD-licensed libopus 1.5.2 | Matroska mono/stereo Opus decoding |
| Windows Media Foundation | H.264, MPEG-4 Part 2, MPEG-2, WMV3, Microsoft MPEG-4 v3, and MP3 decoding |
| External DirectShow audio decoder | E-AC-3 and DTS decoding to stereo PCM through a compatible registered decoder |
| D3D11/DXVA/DXGI | Hardware decode services, NV12/P010 video processing, scaling, and presentation |
| XAudio2 | PCM audio output and the preferred playback clock, with external-clock fallback |

H.264 asks the Windows decoder to use DXVA when available and can use the
decoder's software path otherwise. HEVC Main10 requires a compatible D3D11
Main10/P010 hardware decoder and has no software fallback. Optional RTX Video
VSR automatically falls back to normal D3D11 scaling when unavailable.
Optional NVIDIA FRUC doubles progressive 23-31 FPS video before VSR;
if its runtime or supported hardware is absent, source-rate playback continues.

The supported H.264, HEVC, MPEG-4 Part 2, MPEG-2, WMV3, and Microsoft MPEG-4
v3 paths target ordinary consumer video files commonly
distributed online; they do not implement every profile, level, chroma format,
bit depth, container combination, or optional feature in those standards.
Unusual files can be unsupported, and HEVC Main/Main10 requires a compatible
Windows hardware decoder.

Matroska `A_OPUS` mono and stereo tracks are decoded at 48 kHz. Matroska FLAC
and MP4/Matroska/AVI/MPEG-TS AC-3 tracks are decoded by the built-in C++
decoders and delivered or mixed to stereo. Supported Matroska or MPEG-TS
E-AC-3 and DTS tracks use a compatible external DirectShow audio decoder
registered on the system. AC-3, E-AC-3, and DTS tracks are
selectable from the audio-track menu and are not labeled `[Not Support]`.
External codec binaries are not included in this MoviePlayer package.
MoviePlayer does not prescribe or install a particular codec; users manage
their own DirectShow codec installation.

## Optional AI subtitles

The native AI worker and runtime libraries are included, but the large model
weights are not embedded in the portable package. MoviePlayer provides
one **Generate AI Subtitles...** command that automatically detects the speech
language and selects translation. Select it, run `setup.exe`, or run the
model-only command directly:

```text
install_ai_models.cmd
```

The installer downloads model files only: whisper.cpp
`ggml-large-v3-turbo.bin` and the M2M100 418M int8 translation model. The
native inference worker and its required libraries are already beside
MoviePlayer. The current UI language becomes the target language. Generated
source and translated SRT files are written next to the video. Installation
requires an internet connection; generation is local after setup.

The default installation uses Whisper plus M2M100 for every language pair. An
optional native Japanese-to-Korean CTranslate2 model can be installed
separately and is selected automatically only for Japanese speech with Korean
output; missing, incomplete, or failed optional installations fall back to
M2M100.

Portable `setup.exe` does not select the optional Japanese-to-Korean model by
default. Both command files remain in the package, so the standard models can
be repaired with `install_ai_models.cmd` and the separately licensed Japanese
model can still be installed explicitly as described below.

To consider that optional model, first read
`licenses\AI-RUNTIME-AND-MODELS.md`, then run:

```text
install_japanese_translation_model.cmd
```

No model directory is requested. After explicit terms acceptance, the installer
downloads the pinned `Hunhee/argos-ko-ja` package, verifies its exact size and
SHA-256 digest, and installs the native model with its metadata, attribution,
and file-hash manifest. The publisher declares the package as CC BY-NC 4.0, so
review the model card and non-commercial terms before use.

## NVIDIA RTX Video Super Resolution

Use **View > NVIDIA RTX Video AI Upscaling (VSR)**. A compatible NVIDIA RTX GPU
and driver are required. Unsupported systems automatically use normal D3D11
scaling. The NVIDIA runtime is proprietary and governed by the license in
`licenses\NVIDIA-RTX-Video-SDK-License.pdf`.

## NVIDIA 2x frame interpolation

Use **View > NVIDIA 2x Frame Interpolation** with progressive 23-31 FPS
video. When VSR is also enabled, MoviePlayer runs FRUC before VSR. This feature
requires an NVIDIA Turing-or-newer GPU, current driver, `NvOFFRUC.dll`, and the
matching CUDA runtime from Optical Flow SDK 5.0. Those files retain NVIDIA's
license and must not be redistributed without satisfying its requirements.

## License notices

MoviePlayer first-party code is MIT licensed. libopus is BSD-3-Clause licensed;
its complete notice is in `licenses\libopus-LICENSE.txt`. Windows components
retain their Microsoft terms, and NVIDIA components have proprietary terms.
whisper.cpp, CTranslate2, SentencePiece, their dependencies, and model weights
retain their own licenses. Read `THIRD_PARTY_NOTICES.md` and every file in
`licenses` before redistributing this package. Do not redistribute
`nvngx_vsr.dll` as a standalone product or remove third-party notices.
