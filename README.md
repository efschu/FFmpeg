# FFmpeg mit VapourSynth-Filter-Support

Dies ist ein Fork von [FFmpeg](https://github.com/FFmpeg/FFmpeg), der einen **VapourSynth-Filter** für AI-gestütztes Video-Processing hinzufügt.

## Was ist neu?

Dieser Fork fügt einen **in-process VapourSynth-Filter** (`-vf vapoursynth=file=script.vpy`) zu FFmpeg hinzu. Damit kann FFmpeg VapourSynth-Scripts direkt ausführen - ohne externen `vspipe`-Prozess, ohne Named Pipes.

**Verwendung:**
```bash
# Standard VapourSynth-Filter (NEU in diesem Fork)
ffmpeg -i input.mkv -vf "vapoursynth=file=upscale.vpy" -c:v libx264 output.mkv

# VapourSynth-Demuxer (funktioniert auch in Standard-FFmpeg)
ffmpeg -f vapoursynth -i script.vpy -c:v libx264 output.mkv
```

## Was wurde geändert?

Dieser Fork basiert auf FFmpeg `master` und fügt VapourSynth-Support auf drei Ebenen hinzu:

### 1. Build-System (`configure`)

- **Neue Option:** `--enable-vapoursynth` aktiviert sowohl den Demuxer als auch den Filter
- **`vapoursynth_demuxer_deps="vapoursynth"`** - bereits im Standard-FFmpeg
- **`vapoursynth_filter_deps="vapoursynth"`** - neu hinzugefügt für die Filter-Aktivierung

### 2. Build-System (`libavfilter/Makefile`)

- **`OBJS-$(CONFIG_VAPOURSYNTH_FILTER) += vf_vapoursynth.o`** - war im Original-Fork auskommentiert, jetzt aktiviert

### 3. Filter-Implementation

Drei neue/geänderte Dateien:
- **`libavfilter/vf_vapoursynth.c`** - Der VapourSynth-Video-Filter
- **`libavfilter/vf_vapoursynth.h`** - Filter-Context-Struct und Konstanten
- **`libavfilter/allfilters.c`** - `extern const FFFilter ff_vf_vapoursynth;` Registrierung

## Was der Filter tut

Der `vapoursynth`-Filter:
1. **Lädt** eine VapourSynth-Script-Datei (`.vpy`)
2. **Initialisiert** VapourSynth (R73, API v4.0)
3. **Empfängt** FFmpeg-Frames als Input für das VapourSynth-Script
4. **Liefert** die vom VapourSynth-Script verarbeiteten Frames zurück an FFmpeg
5. **Threading**: Nutzt FFmpeg's `activate()` und VapourSynth's `getFrameAsync()` für asynchrone Verarbeitung

## Build

### Voraussetzungen

```bash
# Ubuntu 22.04
sudo apt-get install -y build-essential cmake pkg-config nasm yasm libtool \
    autoconf automake libssl-dev python3 python3-pip cython3

# VapourSynth R73 wird während des Builds aus Source kompiliert
```

### Bauen

```bash
git clone https://github.com/efschu/FFmpeg.git
cd FFmpeg
./configure --enable-vapoursynth --enable-gpl --enable-version3 \
            --enable-libx264 --enable-libx265 --enable-libvpx \
            --enable-libmp3lame --enable-libopus --enable-libass \
            --enable-libfreetype --enable-libfribidi --enable-libharfbuzz
make -j$(nproc)
make install
```

## Verwendung

### Basis-Syntax

```bash
ffmpeg -i input.mkv \
       -vf "vapoursynth=file=upscale.vpy" \
       -c:v libx264 -preset slow -crf 18 \
       output.mkv
```

### Filter-Optionen

```
vapoursynth=file=script.vpy[:maxbuffer=N][:maxrequests=N][:threads=N]
```

| Option | Default | Beschreibung |
|--------|---------|--------------|
| `file` | (erforderlich) | Pfad zur `.vpy`-Script-Datei |
| `maxbuffer` | 16 | Anzahl der Input-Frames im Buffer |
| `maxrequests` | 8 | Anzahl gleichzeitiger VapourSynth-Frame-Requests |
| `threads` | 0 | VapourSynth-Threads (0 = auto-detect) |

### Beispiel-Script (Anime Upscaling mit Real-ESRGAN)

```python
# upscale.vpy
import vapoursynth as vs
core = vs.core

# TensorRT-Plugin für GPU-Beschleunigung laden
try:
    core.std.LoadPlugin('/usr/lib/vapoursynth/libvstrt.so')
except:
    pass

# Input laden
clip = core.lsmas.LWLibavSource(source='input.mkv')

# Real-ESRGAN Upscaling (2x)
clip = core.resize.Bicubic(clip, format=vs.RGBS, matrix_in_s='709')
clip = core.trt.Model(clip, engine='/models/realesr-anime.engine')
clip = core.resize.Bicubic(clip, width=3840, height=2160, format=vs.RGBS, matrix_s='709')

# RIFE Frame Interpolation (60fps)
clip = core.rife.RIFE(clip, model='rife.25', factor_num=60000, factor_den=1001, trt=True)

# Output-Format
clip = core.resize.Bicubic(clip, format=vs.YUV420P10, matrix_s='709')
clip.set_output()
```

## Geschichte der Änderungen

| Commit | Beschreibung |
|--------|--------------|
| `9d32e79` | Entfernt nicht-baufähiges `vf_vapoursynth.c` (Vorversuch) |
| `e0c8ab5` | **Komplettes Rewrite** von `vf_vapoursynth.c` für FFmpeg 6.0+ und VapourSynth R73 |
| `8977d9f` | Registriert `vf_vapoursynth` in `allfilters.c` |
| `6ec0c20` | Fügt `vapoursynth_filter_deps` in `configure` hinzu |
| `ef5d2cf` | **Wurzelursache**: Uncommented `OBJS-$(CONFIG_VAPOURSYNTH_FILTER)` in `Makefile` |
| `b6c2ffc` | Erste komplette Neuimplementierung mit FFmpeg 6.0+ API |
| `98711a7` | Drei weitere API-Fixes (vs_options, getFramePlaneCount, FF_FILTER_FORMATS_QUERY_FUNC2) |
| `9951a4c` | Korrekte Verwendung von `FILTER_QUERY_FUNC2` macro |

Siehe [CHANGELOG.md](CHANGELOG.md) für Details.

## Bekannte Einschränkungen

- **VapourSynth Version:** Getestet mit R73 (API v4.0). Ältere Versionen werden nicht unterstützt.
- **Python Version:** Das VapourSynth-Script wird gegen die gleiche Python-Version gelinkt, mit der VapourSynth gebaut wurde.
- **Performance:** AI-Modelle (Real-ESRGAN, RIFE) benötigen GPU-Beschleunigung (TensorRT) für Echtzeit-Verarbeitung.

## Verwandte Projekte

- **Jellyfin Fork** mit VapourSynth-Integration: [efschu/jellyfin](https://github.com/efschu/jellyfin) (`feature/vsfilter` Branch)
- **Docker Image** mit allem vorbereitet: `efschu/jellyfin-vsfilter:latest`

## Lizenz

FFmpeg ist unter LGPL 2.1+ lizenziert. Siehe [LICENSE](LICENSE) für Details.

## Credits

- FFmpeg: [FFmpeg Project](https://ffmpeg.org/)
- VapourSynth: [vapoursynth.com](https://www.vapoursynth.com/)
- Original `vf_vapoursynth.c`: Ursprünglich aus dem mpv-Projekt portiert
