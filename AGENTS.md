# AGENTS.md - Development Guide

Diese Datei richtet sich an Entwickler und AI-Agents, die an diesem FFmpeg-Fork weiterarbeiten möchten.

## Projekt-Übersicht

Dieser Fork (`efschu/FFmpeg`) fügt FFmpeg einen **VapourSynth-Video-Filter** hinzu. Der Filter ermöglicht es FFmpeg, VapourSynth-Scripts (`.vpy`) direkt als Filter-Stage auszuführen - ideal für AI-gestütztes Video-Processing (Real-ESRGAN, RIFE, etc.).

### Architektur

```
┌─────────────────────────────────────────────────────────────┐
│ FFmpeg Filter-Graph                                        │
│                                                             │
│  input → [scale] → vf_vapoursynth (NEU) → [encode] → output│
│                          │                                  │
│                          ▼                                  │
│                   ┌──────────────┐                          │
│                   │ VapourSynth  │                          │
│                   │   Script     │                          │
│                   │  (.vpy file) │                          │
│                   └──────────────┘                          │
│                          │                                  │
│                          ▼                                  │
│                   AI Models:                              │
│                   - Real-ESRGAN (Upscale)                  │
│                   - RIFE (Frame Interpolation)             │
│                   - TensorRT (GPU-Acceleration)            │
└─────────────────────────────────────────────────────────────┘
```

### Datenfluss

1. FFmpeg schickt AVFrame-Objekte an den Filter
2. Filter konvertiert sie zu VapourSynth VSFrame-Objekten
3. VapourSynth-Script verarbeitet die Frames (AI-Modelle, etc.)
4. Filter konvertiert die verarbeiteten VSFrames zurück zu AVFrames
5. FFmpeg schickt sie an den nächsten Filter

## Code-Struktur

### Relevante Dateien

| Datei | Zweck | Ändern wenn... |
|-------|-------|----------------|
| `libavfilter/vf_vapoursynth.c` | Hauptimplementierung des Filters | Filter-Logik ändern, neue Optionen |
| `libavfilter/vf_vapoursynth.h` | Header mit Struct-Definition | Context-Struct erweitern |
| `libavfilter/allfilters.c` | Filter-Registrierung | Filter umbenennen oder entfernen |
| `libavfilter/Makefile` | Build-Regeln | Neue Source-Files hinzufügen |
| `configure` | Build-Konfiguration | Neue `--enable-*` Optionen |
| `libavformat/vapoursynth.c` | VapourSynth-Demuxer (Standard-FFmpeg) | Demuxer-Verhalten ändern |

### Context-Struct (`VSContext`)

```c
typedef struct VSContext {
    AVFilterContext *ctx;                    // FFmpeg-Filter-Context
    
    // VapourSynth API
    const VSAPI *vsapi;                      // VapourSynth API-Pointer
    VSCore *vscore;                           // VapourSynth Core
    const VSSCRIPTAPI *vs_script_api;         // Script-API
    VSScript *vs_script;                      // Geladenes Script
    VSNode *in_node;                          // Input-Filter-Node
    VSNode *out_node;                         // Output-Node (vom Script)
    
    // Input-Format (vom ersten Frame gesetzt)
    int in_width, in_height, in_fmt;
    
    // Output-Format (vom Script bestimmt)
    int out_width, out_height;
    
    // Frame-Buffer
    AVFrame **buffered;                       // Input-Frame-Buffer
    int num_buffered, in_frameno;
    
    // Async VS-Frames
    VSFrame **vs_frames;                      // Pending VS-Frames
    int *vs_frame_numbers;                    // Frame-Numbers
    int max_requests, out_frameno;
    
    // Threading
    pthread_mutex_t lock;
    pthread_cond_t vs_wakeup;                 // VS → FFmpeg
    pthread_cond_t input_wakeup;              // FFmpeg → VS
    
    // State-Flags
    int done, eof, failed, initializing, initialized;
    
    // User-Optionen
    char *script_path;
    int maxbuffer, nb_threads;
} VSContext;
```

## Wichtige FFmpeg-Konzepte

### FFFilter Struct (FFmpeg 6.0+)

```c
const FFFilter ff_vf_vapoursynth = {
    .p.name        = "vapoursynth",          // Filter-Name
    .p.description = "...",                   // Beschreibung
    .p.priv_class  = &vapoursynth_class,     // Option-Class
    .p.flags       = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
    
    .init          = init,                   // Filter-Initialisierung
    .uninit        = uninit,                 // Filter-Cleanup
    .activate      = activate,               // Main-Loop
    FILTER_INPUTS(ff_video_default_filterpad),   // Input-Pads
    FILTER_OUTPUTS(ff_video_default_filterpad),  // Output-Pads
    FILTER_QUERY_FUNC2(query_formats),            // Format-Query
    .priv_size     = sizeof(VSContext),      // Größe der priv-Daten
};
```

### `activate()` - Die Main-Loop

Diese Funktion wird aufgerufen, wenn FFmpeg Frames verarbeiten muss:

```c
static int activate(AVFilterContext *ctx) {
    // 1. Initialisierung (einmalig, wenn erstes Frame kommt)
    if (!vs->initialized) {
        // Lade VapourSynth-Library
        init_vs_lib(vs);
        // Evaluiere das .vpy-Script
        load_vs_script(vs);
        // Setze Output-Format
    }
    
    // 2. Versuche, ready Frames auszugeben
    if (ready_frame_available) {
        return ff_filter_frame(outlink, frame);
    }
    
    // 3. Konsumiere Input-Frames in den Buffer
    while (buffer_not_full && input_available) {
        AVFrame *frame = ff_inlink_consume_frame(inlink);
        buffer[count++] = frame;
    }
    
    // 4. Fordere mehr Output-Frames von VapourSynth an
    request_vs_frames(vs);
    
    // 5. Fordere mehr Input von FFmpeg an
    ff_inlink_request_frame(inlink);
    
    return 0;
}
```

## Wichtige VapourSynth-Konzepte

### API Version

- **VapourSynth R73** nutzt **API v4.0** (`VAPOURSYNTH_API_VERSION`)
- **VSScript API v4.1** (`VSSCRIPT_API_VERSION`)

### VSVideoFormat Struct

```c
typedef struct VSVideoFormat {
    int colorFamily;      // cfGray, cfRGB, cfYUV
    int sampleType;       // stInteger, stFloat
    int bitsPerSample;    // 8, 10, 12, 16, 32
    int bytesPerSample;   // 1, 2, 4
    int subSamplingW;     // 0, 1, 2 (für YUV)
    int subSamplingH;     // 0, 1, 2 (für YUV)
    int numPlanes;        // 1 (Gray/RGB), 3 (YUV)
} VSVideoFormat;
```

**Wichtig:** `VSVideoFormat` hat **kein `id` field** in R73! Verwende `getVideoFormatByID()` um von einer ID zum Struct zu kommen.

### VapourSynth-Pixel-Format-IDs (Auszug)

```c
pfYUV420P8   = VS_MAKE_VIDEO_ID(cfYUV, stInteger, 8, 1, 1)
pfYUV420P10  = VS_MAKE_VIDEO_ID(cfYUV, stInteger, 10, 1, 1)
pfYUV422P8   = VS_MAKE_VIDEO_ID(cfYUV, stInteger, 8, 1, 0)
pfYUV444P8   = VS_MAKE_VIDEO_ID(cfYUV, stInteger, 8, 0, 0)
pfGray8      = VS_MAKE_VIDEO_ID(cfGray, stInteger, 8, 0, 0)
pfRGB24      = VS_MAKE_VIDEO_ID(cfRGB, stInteger, 8, 0, 0)
```

## Build-Befehle

```bash
# Standard-Build
./configure --enable-vapoursynth
make -j$(nproc)
make install

# Mit Debug-Symbolen
./configure --enable-vapoursynth --enable-debug --disable-stripping
make -j$(nproc)

# Aufräumen
make distclean
```

## Häufige Probleme und Lösungen

### Problem: `undefined reference to ff_vf_vapoursynth`

**Ursache:** Der Filter ist registriert (extern in `allfilters.c`), aber das `.o` wird nicht kompiliert.

**Lösung:** In `libavfilter/Makefile` muss die Zeile aktiv sein:
```makefile
OBJS-$(CONFIG_VAPOURSYNTH_FILTER) += vf_vapoursynth.o
```
(Auskommentiert = Filter wird nicht gebaut!)

### Problem: `CONFIG_VAPOURSYNTH_FILTER is not set`

**Ursache:** Das configure-Logik aktiviert den Filter nicht, obwohl `vapoursynth_filter_deps` gesetzt ist.

**Lösung:** In `configure` muss die Zeile vorhanden sein:
```bash
vapoursynth_filter_deps="vapoursynth"
```

### Problem: `VSAPI has no member 'getVideoFormatDescriptor'`

**Ursache:** Diese Funktion existiert in R73 nicht.

**Lösung:** Verwende `getVideoFormatByID()`:
```c
VSVideoFormat fmt;
vsapi->getVideoFormatByID(&fmt, pfYUV420P8, core);
```

### Problem: Cython-Fehler beim VapourSynth-Build

**Ursache:** VapourSynth R73 nutzt Python 3.8+ Syntax (`/` positional-only), die Cython 0.29.x nicht versteht.

**Lösung:** Patch `src/cython/vapoursynth.pyx`:
```bash
sed -i 's|key, /, default=None|key, default=None|g' src/cython/vapoursynth.pyx
sed -i 's|key, /, default=_EMPTY|key, default=_EMPTY|g' src/cython/vapoursynth.pyx
sed -i 's|key, default=0, /)|key, default=0)|g' src/cython/vapoursynth.pyx
```

### Problem: `libvapoursynth.so` lädt nicht

**Ursache:** Python-Version-Mismatch zwischen Build und Runtime.

**Lösung:** VapourSynth muss gegen die gleiche Python-Version gebaut werden, mit der es läuft.

## Development-Workflow

### Änderungen am Filter testen

```bash
# 1. Code ändern in libavfilter/vf_vapoursynth.c
# 2. Nur den Filter neu kompilieren
make libavfilter/vf_vapoursynth.o
make libavfilter/libavfilter.so

# 3. In FFmpeg installieren
make install

# 4. Test
ffmpeg -i test.mkv -vf "vapoursynth=file=upscale.vpy" -frames:v 1 -f null -
```

### Neuen Pixel-Format-Support hinzufügen

In `vs_to_ff_pix_fmt()` neue cases hinzufügen:
```c
case pfYUV444P12:  return AV_PIX_FMT_YUV444P12;
```

### Neues VapourSynth-Modell unterstützen

Die Filter-Implementation muss nicht geändert werden - das passiert komplett im `.vpy`-Script. Beispiel:

```python
# Neues Modell: SwinIR statt Real-ESRGAN
clip = core.resize.Bicubic(clip, format=vs.RGBS, matrix_in_s='709')
clip = core.swinir.SwinIR(clip, model_path='/models/swinir.pth')
```

## Wichtige FFmpeg-Funktionen

| Funktion | Zweck |
|----------|-------|
| `ff_inlink_consume_frame()` | Input-Frame aus FFmpeg holen |
| `ff_filter_frame()` | Output-Frame an FFmpeg schicken |
| `ff_inlink_request_frame()` | Mehr Input anfordern |
| `ff_inlink_acknowledge_status()` | EOF-Status prüfen |
| `ff_set_common_formats2()` | Unterstützte Formate setzen |
| `ff_make_format_list()` | Format-Liste erstellen |

## Wichtige VapourSynth-Funktionen

| Funktion | Zweck |
|----------|-------|
| `vsapi->createVideoFilter2()` | Input-Filter-Node erstellen |
| `vsapi->getFrameAsync()` | Async-Frame-Request |
| `vsapi->newVideoFrame()` | Neuen Output-Frame erstellen |
| `vsapi->getWritePtr()` | Schreibzugriff auf Plane |
| `vsapi->getReadPtr()` | Lesezugriff auf Plane |
| `vsapi->getStride()` | Plane-Stride |
| `vsapi->getVideoFrameFormat()` | Format-Descriptor holen |
| `vsapi->getVideoFormatByID()` | Format-Descriptor von ID |
| `vsapi->setThreadCount()` | Thread-Anzahl setzen |
| `vs_script_api->evaluateFile()` | .vpy-Script laden |
| `vs_script_api->getOutputNode()` | Output-Node holen |

## Coding-Standards

1. **Thread-Safety:** Alle Mutationen von `VSContext` müssen unter `vs->lock` sein
2. **Resource-Cleanup:** Immer in `uninit()` alle Ressourcen freigeben
3. **Error-Handling:** Verwende `av_log()` für Fehler, return AVERROR-Codes
4. **Memory:** Verwende `av_malloc`/`av_free` statt `malloc`/`free`
5. **Naming:** FFmpeg nutzt snake_case, nicht camelCase
6. **Comments:** Wichtige Architektur-Entscheidungen kommentieren

## Performance-Tipps

- **Async I/O:** Verwende `getFrameAsync()` statt `getFrame()` für parallele Verarbeitung
- **Buffer-Größe:** Größere `maxbuffer` = mehr Memory, aber weniger Stalls
- **maxrequests:** Sollte ≥ Anzahl der CPU-Cores sein für volle Parallelität
- **Memory:** Pro pending Frame ~ Breite × Höhe × 1.5 Bytes (für YUV420)

## Testing

```bash
# Syntax-Check
./configure --enable-vapoursynth 2>&1 | grep -i vapoursynth

# Compile-Check
make -j$(nproc) 2>&1 | grep -E "error:|warning:" | head

# Functional-Test
cat > /tmp/test.vpy << 'EOF'
import vapoursynth as vs
core = vs.core
clip = core.std.BlankClip(format=vs.YUV420P8, width=320, height=240, length=5)
clip.set_output()
EOF

ffmpeg -i test.mp4 -vf "vapoursynth=file=/tmp/test.vpy" -frames:v 1 -f null -
```

## Links

- **VapourSynth API Docs:** https://www.vapoursynth.com/doc/api.html
- **FFmpeg Filter-Guide:** https://ffmpeg.org/ffmpeg-filters.html
- **Jellyfin-Fork** (Integration): https://github.com/efschu/jellyfin/tree/feature/vsfilter
- **Docker-Image:** `efschu/jellyfin-vsfilter:latest`

## Kontakt

Bei Fragen oder Problemen: Erstelle ein Issue auf GitHub.
