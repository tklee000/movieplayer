# MoviePlayer 0.2 binary package

Run `MoviePlayer.exe` from this folder. Required VC142 runtime DLLs are supplied
beside the application, so a system-wide Visual C++ 2019 Redistributable
installation is not required. Keep the supplied VC142 DLLs, `nvngx_vsr.dll`,
`languages`, `tools`, `scripts`, and `licenses` beside the executable.

## Playback implementation and codec scope

| Layer | Responsibility |
|---|---|
| First-party MoviePlayer code | MP4/MKV/AVI parsing, indexing and seeking, HEVC bitstream parsing and DXVA submission, AAC-LC decoding, subtitle handling, channel mixing, and resampling |
| Windows Media Foundation | H.264, MPEG-4 Part 2, and MP3 decoding |
| D3D11/DXVA/DXGI | Hardware decode services, NV12/P010 video processing, scaling, and presentation |
| XAudio2 | PCM audio output and the playback master clock |

H.264 asks the Windows decoder to use DXVA when available and can use the
decoder's software path otherwise. HEVC Main10 requires a compatible D3D11
Main10/P010 hardware decoder and has no software fallback. Optional RTX Video
VSR automatically falls back to normal D3D11 scaling when unavailable.

The H.264 and HEVC paths target ordinary consumer video files commonly
distributed online; they do not implement every profile, level, chroma format,
bit depth, container combination, or optional feature in those standards.
Unusual files can be unsupported, and HEVC Main10 requires a compatible
Windows hardware decoder.

## Optional AI subtitles

The large AI runtime and model weights are not included. MoviePlayer provides
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

MoviePlayer first-party code is MIT licensed. Windows components retain their
Microsoft terms, and NVIDIA components have proprietary terms. whisper.cpp,
CTranslate2, SentencePiece, their dependencies, and model weights retain their
own licenses. Read `THIRD_PARTY_NOTICES.md` and every file in `licenses` before
redistributing this package. Do not redistribute `nvngx_vsr.dll` as a standalone
product or remove third-party notices.
