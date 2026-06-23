# Changelog

Alle wichtigen Änderungen an diesem FFmpeg-Fork.

## [Unreleased] - 2026-06-23

### Added
- **VapourSynth-Video-Filter** (`-vf vapoursynth=file=script.vpy`)
  - In-process VapourSynth-Ausführung (kein externer `vspipe` nötig)
  - Async-Frame-Processing mit `getFrameAsync()`
  - Vollständige Format-Konvertierung (FFmpeg ↔ VapourSynth)
  - Thread-Safety mit Mutexen und Condition-Variables
- **Konfigurationsoptionen:**
  - `--enable-vapoursynth` aktiviert Filter und Demuxer
  - `file=script.vpy` (erforderlich)
  - `maxbuffer=N` (default: 16)
  - `maxrequests=N` (default: 8)
  - `threads=N` (default: 0 = auto)

### Changed
- **`libavfilter/vf_vapoursynth.c`** - Komplettes Rewrite für FFmpeg 6.0+ und VapourSynth R73
- **`libavfilter/vf_vapoursynth.h`** - Header mit Context-Struct und Konstanten
- **`libavfilter/allfilters.c`** - `extern const FFFilter ff_vf_vapoursynth;` Registrierung

### Fixed
- **`undefined reference to ff_vf_vapoursynth`** - Makefile hatte die `OBJS-$(CONFIG_VAPOURSYNTH_FILTER)` Zeile auskommentiert
- **`AVFilter → FFFilter`** - Type-Mismatch (FFmpeg 6.0+ verwendet FFFilter)
- **`libavfilter/internal.h`** - Existiert nicht mehr, wurde entfernt
- **`pkt_duration → duration`** - FFmpeg API-Änderung
- **`EOF_FRAME → AVERROR_EOF`** - Korrekte AVERROR-Konstante
- **`FILTER_FORMATS_QUERY_FUNC2 → FF_FILTER_FORMATS_QUERY_FUNC2`** - Korrekter Macro-Name
- **`formats.query_func2`** - Innerhalb Union, muss via `FILTER_QUERY_FUNC2` Macro gesetzt werden

### Documentation
- **README.md** - Vollständige Anleitung mit Beispielen
- **AGENTS.md** - Entwickler-Guide für Maintainer

## Commits

```
7b2e769 docs: Add comprehensive README and AGENTS.md
9951a4c fix: Use FILTER_QUERY_FUNC2 macro instead of formats.query_func2
98711a7 fix: Three more API fixes for vf_vapoursynth.c
b6c2ffc fix: Complete rewrite of vf_vapoursynth.c for modern FFmpeg and VapourSynth R73
ef5d2cf fix: Uncomment vf_vapoursynth.o in libavfilter/Makefile
6ec0c20 fix: Add vapoursynth_filter_deps so configure auto-enables the filter
8977d9f fix: Register vf_vapoursynth in allfilters.c
e0c8ab5 feat: Rewrite vf_vapoursynth.c for FFmpeg 6.0+ and VapourSynth R73
9d32e79 fix: Remove non-buildable vf_vapoursynth.c filter
27be0b0 fix: Modernize vf_vapoursynth.c for FFmpeg 6.0+ API
9b2693b docs: Document vf_vapoursynth.c status and demuxer workaround
621a95f fix: Include vapoursynth/VSScript4.h with correct path
b938aab fix: Remove non-existent internal.h include
b451d61 fix: Register vapoursynth filter in build system
5d7a02b feat: add 4kx2 vspipe transcoding pipeline (original)
```

## Bekannte Probleme

- **Python-Versions-Mismatch:** VapourSynth muss gegen die gleiche Python-Version gebaut werden, mit der es läuft
- **Performance:** AI-Modelle benötigen GPU-Beschleunigung (TensorRT) für Echtzeit
- **VapourSynth-Version:** Nur R73 (API v4.0) wird offiziell unterstützt
