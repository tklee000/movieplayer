# MoviePlayer 0.2

MoviePlayer is a native Windows x64 MP4/MKV/AVI player written in C++17. Its
first-party media layer was implemented directly in C/C++ for this project.
The repository contains the container
parsers, sample indexing and seeking, codec interfaces, HEVC bitstream and
DXVA submission code, AAC-LC decoder, channel mixer, subtitle parsers, and
subtitle-audio resampler.

## Implementation boundary

| Layer | MoviePlayer uses it for |
|---|---|
| First-party C/C++ | MP4/MKV/AVI parsing, indexing and seeking, codec interfaces, HEVC syntax parsing and DXVA submission, AAC-LC decoding, channel mixing, resampling, subtitle handling, playback scheduling, and the Win32 UI |
| Windows Media Foundation | H.264 and MPEG-4 Part 2 video decoding, plus MP3 audio decoding |
| D3D11, DXVA, DXGI, and XAudio2 | Hardware video decode services, GPU video processing and presentation, software-rendering fallback, and audio output |
| Optional NVIDIA and native AI components | RTX Video VSR, speech recognition, and translation |

The decoder and renderer share one D3D11 device. H.264 requests DXVA through
the Windows transform and can fall back to its software path. HEVC Main10 uses
MoviePlayer's bitstream parser and requires the GPU's D3D11 Main10/P010 decode
profile; there is no current software HEVC fallback. RTX Video VSR is optional,
and any VSR failure returns to standard D3D11 scaling without stopping playback.

See [the technical guide](docs/MoviePlayer-Wiki.md) for the full architecture,
implementation ownership, acceleration requirements, and fallback matrix.

## Current playback scope

- Container: non-fragmented MP4 with `moov`, `stbl`, 32/64-bit chunk offsets,
  decode/composition timing, sync samples, `avcC`, `hvcC`, and `esds`; plus
  focused Matroska/MKV playback with `SeekHead`, `Cues`, clusters, block groups,
  and fixed/Xiph/EBML lacing for H.264/HEVC, AAC, and text/bitmap subtitle tracks;
  plus classic indexed RIFF AVI (`idx1`) for Xvid/DX50 and MP3.
- Video: H.264 `avc1`/`avc3` MP4 video through the Windows Media Foundation
  decoder with NV12 output, including the tested High Profile Level 4.2 title;
  and HEVC Main/Main10 4:2:0 streams matching the supplied x265 test title.
  Xvid/DX50 MPEG-4 Part 2 is decoded through Windows Media Foundation. Video
  surfaces are presented through the D3D11 video processor.
- Audio: AAC-LC at 24, 44.1, and 48 kHz, including spectral Huffman decoding,
  inverse quantization,
  stereo tools, TNS, IMDCT/window overlap, native stereo playback,
  5.1-to-stereo mixing, PCE 7.1-to-stereo mixing, and XAudio2; plus Windows
  Media Foundation MP3 decoding for AVI.
- Seeking: MP4 sync-sample, MKV cue, and AVI keyframe-index seek with decoder
  and audio-clock reset.
- Subtitles: external SRT, ASS/SSA, and SMI display; Matroska embedded
  `S_TEXT/ASS`, `S_TEXT/SSA`, `S_TEXT/UTF8`, and DVD VobSub (`S_VOBSUB`) bitmap
  subtitles with zlib decompression; plus an optional local native AI
  transcription/translation worker. The worker reuses the built-in MP4/AAC
  stack and a 63-tap 48 kHz-to-16 kHz FIR resampler.
- Scaling: D3D11 video processing and optional NVIDIA RTX Video VSR.

The H.264 and HEVC paths are focused playback implementations for ordinary
consumer video files commonly distributed online. They are not complete
implementations of every profile, level, chroma format, bit depth, container
combination, or optional bitstream feature in those standards. Unsupported or
unusual files may fail to open or decode. A compatible Windows hardware HEVC
Main10 decoder is required for HEVC Main10 playback; H.264 uses the Windows
Media Foundation decoder and requests DXVA acceleration when available. The
native AAC-LC decoder is similarly scoped to the formats listed above.

## Codec layout

```text
src/codec/
  core/                  bounds-checked readers, file I/O, shared media types
  container/
    MediaDemuxer.h       container-neutral interface
    avi/                 indexed RIFF AVI reader
    mkv/                 focused Matroska EBML/Cluster/Cues reader
    mp4/                 ISO Base Media parser and sample-table index
  audio/
    AudioDecoder.h       audio decoder interface
    aac/                 AAC-LC decoder and standard Huffman tables
    mp3/                 Windows Media Foundation MP3 backend
  subtitle/              embedded UTF-8/ASS text decoder
  video/
    VideoDecoder.h       video decoder interface shared by H.264 and HEVC
    h264/                H.264/MPEG-4 Part 2 Media Foundation backend
    hevc/                HEVC syntax parser and D3D11/DXVA backend
```

## Build

Requirements:

- Windows 10 or 11 x64
- Visual Studio 2019 with Desktop development with C++
- CMake bundled with Visual Studio
- NVIDIA RTX Video SDK files when VSR is built
- Pinned native AI source dependencies for the subtitle worker

Set up the remaining optional/source dependencies, then build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_rtx_video_sdk.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\setup_native_ai_dependencies.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Configuration Release
```

The build creates `build-vs2019\Release\MoviePlayer.exe` and the native subtitle
worker. Run the codec smoke test explicitly with:

```powershell
cmake --build build-vs2019 --config Release --target MovieCodecSmoke
.\build-vs2019\Release\MovieCodecSmoke.exe "D:\path\video.mp4"
```

## License

MoviePlayer first-party source is licensed under the MIT License. NVIDIA and
the optional native AI libraries/models retain their own licenses; see
`THIRD_PARTY_NOTICES.md`. The MIT license covers only MoviePlayer's first-party
code and does not relicense Windows components, SDKs, libraries, or models.
