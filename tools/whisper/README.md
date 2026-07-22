# Native AI subtitle worker

MoviePlayer now runs AI subtitles through the compiled
`MoviePlayerSubtitleWorker.exe`. The implementation is in
`src/SubtitleWorker.cpp` and uses these native libraries:

- whisper.cpp 1.9.1 for multilingual speech recognition
- CTranslate2 4.8.1 with the ruy CPU backend for M2M100 int8 translation
- SentencePiece 0.2.1 for M2M100 tokenization
- MoviePlayer's built-in MP4/AAC decoder and FIR resampler for 16 kHz mono PCM

No script interpreter or package runtime is installed. Run
`scripts/setup_whisper.ps1` to download and verify only the GGML Whisper model
and the CTranslate2 M2M100 model files.

Build dependencies are fetched at pinned revisions by
`scripts/setup_native_ai_dependencies.ps1`; their source trees are ignored by
Git and are not part of the MoviePlayer source distribution.
