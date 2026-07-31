# MoviePlayer 0.5 binary package

Run `MoviePlayer.exe` from this folder. Required VC142 runtime DLLs are supplied
beside the application, so a system-wide Visual C++ 2019 Redistributable
installation is not required. Keep the supplied VC142 DLLs, `nvngx_vsr.dll`,
`languages`, `tools`, `scripts`, and `licenses` beside the executable.

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
language and selects translation. Select it or run:

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

The MSI package intentionally excludes the optional Japanese-to-Korean model
and its installer. The standard Whisper and M2M100 models are downloaded and
verified automatically during MSI installation. The portable ZIP and source
tree retain the separate optional installer described below.

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

## License notices

MoviePlayer first-party code is MIT licensed. libopus is BSD-3-Clause licensed;
its complete notice is in `licenses\libopus-LICENSE.txt`. Windows components
retain their Microsoft terms, and NVIDIA components have proprietary terms.
whisper.cpp, CTranslate2, SentencePiece, their dependencies, and model weights
retain their own licenses. Read `THIRD_PARTY_NOTICES.md` and every file in
`licenses` before redistributing this package. Do not redistribute
`nvngx_vsr.dll` as a standalone product or remove third-party notices.
