# H.264 A/V Sync and Video Corruption Fix History

Date: 2026-08-02

## Summary

MoviePlayer could begin with correct lip sync and then make audio appear more
than one second late after crossing a specific H.264 segment. The same segment
could also show a distorted picture or a short burst of accelerated video.
Seeking directly past the affected dependency chain could hide the problem.

The focused reproduction sought to approximately `44:25` and played through
`44:48` and `45:05`. An independent Windows Media Foundation player remained
synchronized and rendered the same source correctly.

## Investigation history

1. The audio output was measured first because the symptom sounded like an
   accumulating audio delay. The source was 16-bit, 48 kHz stereo PCM after
   decoding. The 24 queued XAudio2 buffers represented 24,576 sample frames, or
   512 ms of future audio at 48 kHz; they did not represent a 480 kHz stream.
2. A full AAC timeline probe decoded 181,049 packets, 185,394,176 sample frames,
   and reported no meaningful gap or overlap. Its total duration error was
   approximately 0.333 ms.
3. A reference comparison decoded the same AAC with MoviePlayer and the Windows
   Media Foundation pipeline. All 41 analysis windows selected a 0 ms delay,
   with a minimum correlation of 0.9953. This ruled out the AAC decoder and the
   audio timeline as the cause of the persistent one-second offset.
4. Video diagnostics exposed both the timestamp assigned by MoviePlayer and the
   original timestamp on the Media Foundation output sample. The direct H.264
   transform dropped 35 pictures in the focused 60-second probe. MoviePlayer's
   old min-heap fallback then assigned the unused input timestamps to newer
   decoded pictures. Numeric A/V logs appeared synchronized even though the
   visible picture content had moved ahead.
5. A brief decoder timestamp reorder (`2705.74` to `2705.60` seconds) also made
   an intermediate fix permanently select the synthetic timeline. The error
   then grew to approximately 1.17 seconds and caused a 19-frame catch-up burst.
6. A standalone Media Foundation reference player confirmed that the file was
   valid and that the Windows file-backed decode path rendered the problem
   segment without the corruption seen in the direct-transform path.

## Changes

- File-backed H.264 playback now uses the Windows Media Foundation Source
  Reader, matching the independently verified reference pipeline. Direct MFT
  input remains available for streams that have no source path.
- Decoded output timestamps are preserved on `VideoFrame`. A submitted input
  timestamp is used only when the decoder supplies no usable timestamp.
- The video presentation queue sorts by PTS, never deliberately selects a
  future frame, and records frame acquisition and skip behavior.
- Audio seeks trim PCM to the exact target sample instead of retaining the
  leading portion of a partially overlapping audio frame.
- The XAudio2 clock accounts for filtered endpoint latency and an additional
  output margin, remains monotonic, and records queue depth in sample frames and
  milliseconds.
- The DXGI swap chain uses the low-latency flip model when available and limits
  accumulated presentation frames to one.
- Codec smoke tests now include full audio timeline, independent AAC reference,
  focused video throughput, and timestamp anomaly probes.
- `MfReferencePlayer` provides an independent visible A/V pipeline for future
  regression checks.

## Validation

- Release build and language/resource verification: passed.
- Focused H.264 probe from 2,665 seconds for 60 seconds:
  - before the Source Reader change: 1,764 decoded frames and 35 missing frames;
  - after the change: all 1,799 expected frames, no timestamp anomalies, and
    approximately 970 decoded frames per second on the test system.
- Visible playback from `44:25.05` through `45:40`:
  - synthetic displayed timestamps: 0;
  - skipped video frames: 0;
  - maximum decoder-to-assigned timestamp difference: 0 ms;
  - maximum absolute video-to-audio clock difference: 33.4 ms, within one
    29.97 fps frame.
- Final user-visible replay confirmed correct audio synchronization and picture
  output across the affected segment.

## Diagnostic commands

```powershell
MovieCodecSmoke.exe <media> --audio-timeline-probe
MovieCodecSmoke.exe <media> --audio-reference-probe=2520,420
MovieCodecSmoke.exe <media> --video-throughput-probe=2665
MfReferencePlayer.exe <media> 2665
```

Normal MoviePlayer runs also write `MoviePlayer-sync.csv` and
`MoviePlayer-video-sync.csv` to the current user's temporary directory when the
player closes normally.
