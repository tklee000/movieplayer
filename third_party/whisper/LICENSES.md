# Native AI runtime and model notices

MoviePlayer does not bundle model weights in its source repository or standard
binary package. `scripts/setup_whisper.ps1` downloads the files directly from
their publishers after the user requests installation and verifies every file
with a pinned size and SHA-256 digest.

## Native runtime libraries

| Component | Pinned version | License |
|---|---:|---|
| whisper.cpp | 1.9.1 | MIT |
| CTranslate2 | 4.8.1 | MIT |
| SentencePiece | 0.2.1 | Apache License 2.0 |
| ruy | CTranslate2 pinned submodule | Apache License 2.0 |
| cpuinfo | ruy pinned submodule | BSD 2-Clause |
| cpu_features | CTranslate2 pinned submodule | Apache License 2.0 |
| spdlog | CTranslate2 pinned submodule | MIT |

The complete license texts for these libraries are copied into the release
`licenses` directory by `scripts/create_deploy.ps1`.

## Default downloaded models

| Model | Pinned revision | Purpose | License declared by publisher |
|---|---|---|---|
| `ggerganov/whisper.cpp` `ggml-large-v3-turbo.bin` | `5359861c739e955e79d9a303bcbc70fb988958b1` | Multilingual speech recognition | MIT |
| `gn64/M2M100_418M_CTranslate2` | `18e406c615ef2991fa74d53734bf66b0a6b10cb4` | Offline multilingual translation | Derived from `facebook/m2m100_418M`, MIT model card |

Model pages and upstream notices:

- https://huggingface.co/ggerganov/whisper.cpp
- https://github.com/openai/whisper
- https://huggingface.co/gn64/M2M100_418M_CTranslate2
- https://huggingface.co/facebook/m2m100_418M

## Optional Japanese-to-Korean model

The legacy `sappho192/aihub-ja-ko-translator` ONNX files are never included in
MoviePlayer releases or installed by the default model installer. If a user
keeps those files locally, their separate model card, base-model terms, AIHub
dataset terms, and non-commercial/share-alike restrictions remain the user's
responsibility. MoviePlayer's native worker falls back to the MIT-licensed
M2M100 model because that legacy model requires a different tokenizer and
inference runtime.

The separate `install_japanese_translation_model.cmd` installer downloads the
`Hunhee/argos-ko-ja` package pinned to revision
`15a9f14d22beefcd1cb4d45abc73f293ec2b56a8`. It verifies the 76,969,020-byte
archive against SHA-256
`b127ac3aea6f1a4c3628a33740c54e6e793e0b646d925bdb2ed52e3c8f584da8`
before installing it under `translation-ja-ko-native`. The publisher declares
the package as CC BY-NC 4.0. Package metadata credits OPUS,
Wiktionary/Wiktextract, and Stanza. The installer preserves that metadata and
writes source, attribution, and SHA-256 manifest files beside the model.

Model page and license:

- https://huggingface.co/Hunhee/argos-ko-ja
- https://creativecommons.org/licenses/by-nc/4.0/

This optional model is used only for detected Japanese speech translated to
Korean. A missing, incomplete, or failed installation falls back to M2M100.

This notice is informational and is not legal advice. Model publishers may
change their repository metadata; review the pinned revision and applicable
terms before redistributing any model file.
