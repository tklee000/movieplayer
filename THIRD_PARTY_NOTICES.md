# MoviePlayer third-party components and license notices

This document describes the third-party components in the current MoviePlayer
0.1 source tree and Windows x64 binary package. The original license text from
each rights holder takes precedence over this summary. This document is not
legal advice.

Dependency versions and commits are pinned in
`scripts/setup_native_ai_dependencies.ps1`. Model revisions, expected sizes,
and SHA-256 digests are pinned in `scripts/setup_whisper.ps1` and
`scripts/setup_japanese_translation_model.ps1`. During packaging,
`scripts/create_deploy.ps1` copies the applicable license texts into the
`licenses` directory.

## 0. First-party implementation boundary

MoviePlayer's MP4/MKV/AVI container parsers, sample indexing and seeking,
codec-neutral interfaces, HEVC bitstream parsing and DXVA submission, AAC-LC
decoder, subtitle handling, channel mixer, and resampler are first-party C/C++
implementations written for this project.

"First-party" does not mean that MoviePlayer implements the Windows platform
or the separately licensed components that it calls. H.264, MPEG-4 Part 2, and
MP3 decoding use Windows Media Foundation. Video decoding and presentation use
D3D11/DXVA. Optional NVIDIA and AI components are identified separately below.
The H.264 and HEVC paths target ordinary consumer files and do not implement
every profile, level, color format, bit depth, or optional feature in their
respective standards.

## 1. Components built with or distributed beside the application

| Component | Pinned version or commit | Purpose | License | Original text or source location |
|---|---|---|---|---|
| NVIDIA RTX Video SDK | 1.1 | RTX Video VSR | Proprietary NVIDIA RTX SDK license | `third_party/rtx_video_sdk/NVIDIA_RTX_Video_SDK_License.pdf` |
| whisper.cpp and GGML | 1.9.1, `f049fff95a089aa9969deb009cdd4892b3e74916` | Speech recognition | MIT | `third_party/whisper_cpp/LICENSE` |
| CTranslate2 | 4.8.1, `0d8bcd362ac75ef860ef161d6f0efad0ae439ff0` | Translation-model inference | MIT | `third_party/ctranslate2/LICENSE` |
| spdlog | 1.10.0, `76fb40d95455f249bd70824ecfcae7a8f0930fa3` | CTranslate2 logging | MIT | `third_party/ctranslate2/third_party/spdlog/LICENSE` |
| cpu_features | 0.7.0, `8a494eb1e158ec2050e5f699a504fbc9b896a43b` | x86 CPU feature detection | Apache-2.0 | `third_party/ctranslate2/third_party/cpu_features/LICENSE` |
| Ruy | `363f252289fb7a1fba1703d99196524698cb884d` | CTranslate2 CPU matrix operations | Apache-2.0 | `third_party/ctranslate2/third_party/ruy/LICENSE` |
| cpuinfo | `082deffc80ce517f81dc2f3aebe6ba671fcd09c9` | CPU information for Ruy | BSD-2-Clause | `third_party/ctranslate2/third_party/ruy/third_party/cpuinfo/LICENSE` |
| clog | Copy included by cpuinfo | cpuinfo logging | BSD-2-Clause | `third_party/ctranslate2/third_party/ruy/third_party/cpuinfo/deps/clog/LICENSE` |
| BS::thread_pool | 5.1.0 | CTranslate2 CPU worker pool | MIT | `licenses/BS-thread-pool-LICENSE.txt` |
| avx_mathfun | Copy included by CTranslate2 | AVX math functions | Modified MIT and original zlib terms | `licenses/avx_mathfun-LICENSE.txt` |
| SIMD_Utils avx512_mathfun | Included copy from 0.2.5 | AVX-512 math functions | BSD-2-Clause | `licenses/SIMD-Utils-LICENSE.txt` |
| neon_mathfun | Copy included by CTranslate2 | ARM NEON math functions | zlib | `licenses/neon_mathfun-LICENSE.txt` |
| SentencePiece | 0.2.1, `31646a467d2051eb904e0b45de3a73e91fe1c1e3` | Translation tokenization | Apache-2.0 | `third_party/sentencepiece/LICENSE` |
| Selected Abseil code | Copy included by SentencePiece 0.2.1 | String and flag support | Apache-2.0 | `third_party/sentencepiece/third_party/absl/LICENSE` |
| Protocol Buffers Lite | Copy included by SentencePiece 0.2.1 | Model metadata serialization | BSD-3-Clause | `third_party/sentencepiece/third_party/protobuf-lite/LICENSE` |
| Darts-clone | Copy included by SentencePiece 0.2.1 | Dictionary data structures | BSD-3-Clause | `third_party/sentencepiece/third_party/darts_clone/LICENSE` |
| Microsoft Visual C++ redistributable runtime | Visual Studio 2019 VC142 | Application runtime DLLs | Microsoft redistribution terms | Visual Studio installation and redistribution terms |

GGML is part of the pinned whisper.cpp source tree rather than a separate
download. spdlog is included in header-only mode by CTranslate2. Ruy includes
cpuinfo, and cpuinfo includes clog. This SentencePiece build uses its bundled
protobuf-lite and selected Abseil and Darts code.

`avx_mathfun.h`, `avx512_mathfun.h`, and `neon_mathfun.h` are selected
according to the target CPU and compiler options. AVX-family code can be used
by the current Windows x64 build. NEON code remains in the source tree for ARM
targets. Copyright and license notices in those source files must be retained,
and the package includes the corresponding license text in `licenses`.

## 2. Present in the source tree but not linked into the current package

| Component | Status | License |
|---|---|---|
| esaxx | Source for SentencePiece training tools; not linked into the current `sentencepiece-static` runtime | MIT (`third_party/sentencepiece/third_party/esaxx/LICENSE`) |
| GoogleTest in Ruy | Present in the Ruy tree; tests are disabled and it is not linked into release binaries | BSD-3-Clause (`third_party/ctranslate2/third_party/ruy/third_party/googletest/LICENSE`) |
| CTranslate2 cxxopts, Thrust/CCCL, CUTLASS, and GoogleTest | Submodule paths exist, but the current CPU-only setup does not check them out | Not part of the current package |
| whisper.cpp examples, tests, and bindings | Present in the full source checkout; not built because `WHISPER_BUILD_EXAMPLES=OFF` and `WHISPER_BUILD_TESTS=OFF` | Notices in the corresponding source files apply |

If a future build enables or distributes any component in this table, its
original license text and any newly included transitive dependencies must also
be added to the package.

## 3. AI models installed separately by the user

The source repository and standard binary package do not include model weights.
Installers download pinned files from the publishers' repositories and verify
their exact size and SHA-256 digest.

| Model | Pinned revision | Purpose | Publisher-declared license |
|---|---|---|---|
| `ggerganov/whisper.cpp` `ggml-large-v3-turbo.bin` | `5359861c739e955e79d9a303bcbc70fb988958b1` | Multilingual speech recognition | MIT |
| `gn64/M2M100_418M_CTranslate2` | `18e406c615ef2991fa74d53734bf66b0a6b10cb4` | Offline multilingual translation | MIT; converted from `facebook/m2m100_418M` |
| `Hunhee/argos-ko-ja` | `15a9f14d22beefcd1cb4d45abc73f293ec2b56a8` | Optional Japanese-Korean translation | CC-BY-NC-4.0 |

Model sources:

- https://huggingface.co/ggerganov/whisper.cpp
- https://github.com/openai/whisper
- https://huggingface.co/gn64/M2M100_418M_CTranslate2
- https://huggingface.co/facebook/m2m100_418M
- https://huggingface.co/Hunhee/argos-ko-ja
- https://creativecommons.org/licenses/by-nc/4.0/

`Hunhee/argos-ko-ja` is an optional component with non-commercial terms. Its
installer proceeds only after explicit acceptance of the third-party terms and
preserves metadata, source, attribution, and a file-hash manifest beside the
installed model. Anyone redistributing or commercially using a model must
review the model card, base-model terms, and training-data conditions.

## 4. NVIDIA RTX Video SDK

RTX Video VSR uses NVIDIA's proprietary SDK. The SDK is excluded from the Git
repository and must be installed separately. A binary package can include
`nvngx_vsr.dll` and the unmodified NVIDIA SDK license required to run the
application.

Do not redistribute the SDK runtime as a standalone product, remove required
notices, bypass protections, or imply that NVIDIA endorses MoviePlayer.
NVIDIA, GeForce, and RTX are trademarks or registered trademarks of NVIDIA
Corporation. MoviePlayer is not sponsored or endorsed by NVIDIA.

Official information:

- https://developer.nvidia.com/rtx-video-sdk/getting-started
- https://developer.nvidia.com/gameworks/nvidia_rtx_sdks_license_12apr2021.pdf

## 5. Microsoft and Windows components

MoviePlayer uses Direct3D 10/11, DXGI, D3DCompiler, XAudio2, Common Controls,
DWM, Media Foundation, and other Windows system APIs. Windows supplies these
components; their source is not included as MoviePlayer third-party code.

The package can include Visual Studio 2019 VC142 redistributable DLLs beside
the application under Microsoft's redistribution terms. MoviePlayer's MIT
License does not change the ownership or terms of Microsoft SDK or runtime
components.

## 6. Test material

`test-assets/japanese_speech_cc0.ogg` is CC0 1.0 material obtained from
Wikimedia Commons. Its exact source and SHA-256 are recorded in
`test-assets/README.md`. `test-assets/korean_subtitle_test.srt` is a
first-party MoviePlayer test fixture.

## 7. Distribution checklist

- Ship this `THIRD_PARTY_NOTICES.md` file and the `licenses` directory.
- Retain all MIT, BSD, Apache, and zlib notices in source and binary packages.
- Run `scripts/verify_release.ps1` to validate executable imports and runtime
  files in the distribution directory.
- Keep the original NVIDIA SDK license with `nvngx_vsr.dll`.
- Recheck the pinned model card and data terms before redistributing any model.
- Do not include the `CC-BY-NC-4.0` model in a commercial distribution.
