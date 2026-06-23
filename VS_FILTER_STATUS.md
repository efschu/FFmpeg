# VapourSynth Filter Status

## Issue
The original `vf_vapoursynth.c` filter was added to this fork but is **not compatible with modern FFmpeg** (6.0+).

## Problems
1. Uses non-existent `internal.h` header
2. Uses removed FFmpeg API: `pkt_duration`, `interlaced_frame`, `top_field_first`
3. References missing `EOF_FRAME` constant
4. Uses old `getVSScriptAPI` function signature

## Workaround: Use the Demuxer Instead
The VapourSynth **demuxer** (`-f vapoursynth`) IS fully functional in this fork.

### Usage
```bash
ffmpeg -f vapoursynth -i script.vpy -c:v libx264 output.mkv
```

### Jellyfin Integration
Use the demuxer-based approach in `VsFilterPipeline.cs`:
- Write VapourSynth script to temp file
- Use `ffmpeg -f vapoursynth -i /tmp/script.vpy` instead of `-vf vapoursynth=file=...`
- Requires FFmpeg to demux from the script and pipe raw frames

## Long-term Solution
The `vf_vapoursynth.c` file needs to be modernized for current FFmpeg API:
- Replace `pkt_duration` with `duration`
- Remove `interlaced_frame`/`top_field_first` (moved to side data)
- Update VapourSynth API calls to R73 signature
- Fix `init_vs_lib` function pointer declarations
