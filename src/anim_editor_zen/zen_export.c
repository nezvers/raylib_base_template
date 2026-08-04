// ============================================================================
//  zen_export.c  -  File > Export...
//
//  WHY THIS EXISTS. Everything authored in the zen editor was trapped in it:
//  the only way to show someone a finished animation was to screen-record the
//  window. This renders the document to the formats people actually share -
//  GIF, MP4/WebM/AVI, or a PNG frame sequence - straight from the editor.
//
//  DETERMINISM IS WHAT MAKES THIS CHEAP. AnimDocDrawLoop is a pure function of
//  (doc, t): nothing in src/anim/ reads GetTime/GetFrameTime or any RNG (the
//  crumble scatter hashes the GLYPH INDEX, not the clock). So a frame is not
//  "the editor's current state captured" - it is a value we can ask for. The
//  exporter never touches the editor's playhead, and frame i comes from
//  t = start + i/fps rather than an accumulated dt, which also means no drift.
//
//  IT RUNS IN CHUNKS, NOT IN ONE GO. A 10s 60fps export is 600 frames. Doing
//  those in a single tick freezes the window long enough for the OS to offer to
//  kill it. Instead EXPORT_CHUNK frames are rendered per tick, so the modal can
//  show a progress bar, an ETA, and a Cancel that actually works.
//
//  WHY IT DRAWS FROM Gui() AND NOT Draw(). BeginTextureMode cannot nest, and
//  main.c wraps the whole Draw() pass in one. The Gui() pass runs outside it,
//  which is the only place we are free to bind our own render target.
//
//  ENCODERS. GIF is msf_gif.h (vendored in include/, MIT/public-domain) using
//  its incremental to-file API, so memory stays bounded by one frame instead of
//  the whole gif. Video is a raw RGBA stream piped to ffmpeg's stdin - no temp
//  frames on disk, one pass. ffmpeg is optional and detected once; when it is
//  missing the video formats stay VISIBLE but disabled with a reason, because
//  silently hiding them would just look like the feature is broken.
//
//  File-static state (the zen_clone_modal.c pattern), so it costs nothing in
//  ZenCtx and - more importantly - nothing in the 16-deep undo snapshot ring.
//  Export settings are not document state and must never be undoable.
// ============================================================================

#include "raylib.h"
#include "raygui.h"
#include "zen_internal.h"
#include "../screen_state/screen_state.h"
#include "../audio_state/audio_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MSF_GIF_IMPL
#include "msf_gif.h"

#if !defined(PLATFORM_WEB)
    #if defined(_WIN32)
        #include <io.h>
        #include <fcntl.h>
        #define ExpPopen(c)  _popen((c), "wb")
        #define ExpPclose(p) _pclose(p)
    #else
        #define ExpPopen(c)  popen((c), "w")
        #define ExpPclose(p) pclose(p)
    #endif
    #define ZEN_EXPORT_VIDEO 1
#else
    #define ZEN_EXPORT_VIDEO 0
#endif

// Frames rendered per tick. Each is a full scene draw + a GPU readback, so this
// trades export speed against how responsive the window stays. 4 keeps a tick
// comfortably inside a 60Hz budget at the sizes this editor produces.
#define EXPORT_CHUNK    4

#define EXP_W           440.0f
#define EXP_TITLE_H     24.0f
#define EXP_ROW_H       24.0f
#define EXP_GAP         6.0f
#define EXP_DIR         ZEN_ANIM_DIR "/exports"

// ---------------------------------------------------------------------------
//  Formats
// ---------------------------------------------------------------------------
typedef enum {
    EXP_FMT_GIF = 0,
    EXP_FMT_MP4,
    EXP_FMT_WEBM,
    EXP_FMT_AVI,        // uncompressed rawvideo
    EXP_FMT_PNG,        // numbered frame sequence
    EXP_FMT_COUNT,
} ExpFormat;

typedef struct {
    const char *label;
    const char *ext;
    bool        needsFfmpeg;
    const char *desc;
} ExpFormatInfo;

static const ExpFormatInfo s_fmt[EXP_FMT_COUNT] = {
    { "GIF",                  ".gif",  false,
      "Animated GIF. Plays anywhere, 256 colors per frame, no audio." },
    { "MP4  (H.264)",         ".mp4",  true,
      "H.264 video. The safe default for sharing - plays in every browser." },
    { "WebM (VP9)",           ".webm", true,
      "VP9 video. Smaller than MP4 at the same quality, supports transparency." },
    { "AVI  (uncompressed)",  ".avi",  true,
      "Raw uncompressed video. Lossless and huge - for editing, not sharing." },
    { "PNG frame sequence",   "",      false,
      "One .png per frame in its own folder. Always available, no dependencies." },
};

typedef enum { EXP_IDLE = 0, EXP_RUNNING, EXP_DONE, EXP_FAILED } ExpState;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
static bool      s_open = false;
static Vector2   s_pos = { 0 };
static bool      s_placed = false;      // first show centers it
static Rectangle s_rect = { 0 };
static bool      s_dragging = false;
static Vector2   s_dragOff = { 0 };

// settings
static int   s_format = EXP_FMT_GIF;
static int   s_scale = 2;               // 1..4
static int   s_fps = 30;
static bool  s_showIntro = false;
static bool  s_transparent = false;
static int   s_gifQuality = 16;         // msf_gif 1..16
static int   s_videoQuality = 70;       // 0..100, mapped to CRF
static char  s_nameBuf[96] = "";
static bool  s_edName = false;
static bool  s_edFps = false;

// format dropdown
static bool      s_fmtDrop = false;
static Rectangle s_fmtDropRect = { 0 };

// job
static ExpState        s_state = EXP_IDLE;
static int             s_frame = 0, s_total = 0;
static float           s_tStart = 0.0f;
static double          s_jobStart = 0.0;
static char            s_status[192] = "";
static char            s_outPath[512] = "";
static RenderTexture2D s_rt = { 0 };
static bool            s_rtValid = false;
static MsfGifState     s_gif = { 0 };
static bool            s_gifOpen = false;
static FILE           *s_gifFile = NULL;
static FILE           *s_pipe = NULL;
static long long       s_bytes = 0;     // actual size after a PNG-sequence run

// ffmpeg presence: -1 unknown, 0 missing, 1 present. Probed once - spawning a
// process every frame to ask the same question would be absurd.
static int s_ffmpeg = -1;

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
static bool FfmpegProbe(void)
{
#if ZEN_EXPORT_VIDEO
    #if defined(_WIN32)
        FILE *p = _popen("ffmpeg -version 2>NUL", "r");
    #else
        FILE *p = popen("ffmpeg -version 2>/dev/null", "r");
    #endif
    if (!p) return false;
    char buf[64];
    bool ok = (fgets(buf, sizeof(buf), p) != NULL);
    #if defined(_WIN32)
        _pclose(p);
    #else
        pclose(p);
    #endif
    return ok;
#else
    return false;
#endif
}

static bool FormatAvailable(int f)
{
    if (!s_fmt[f].needsFfmpeg) return true;
    return s_ffmpeg == 1;
}

// Exported canvas size: the doc renders at its native target size and is scaled
// up afterwards. Element geometry is a FRACTION of ScreenStateTargetSize(), so
// rendering at a different size would move things, not enlarge them.
static void ExportSize(int *w, int *h)
{
    Vector2 game = ScreenStateTargetSize();
    *w = (int)game.x * s_scale;
    *h = (int)game.y * s_scale;
}

static void ExportRange(float *start, float *end)
{
    // The outro is ALWAYS trimmed - it is the tail the doc plays on the way out,
    // and an export that includes it cannot loop. The intro is optional: it is
    // the run-in the animation plays once, so showing it makes a one-shot clip
    // and hiding it makes a seamless loop.
    *start = s_showIntro ? 0.0f : AnimDocIntroEnd(&zen.doc);
    *end   = AnimDocOutroStart(&zen.doc);
    if (*end < *start) *end = *start;
}

// An intro of zero length is nothing to show or hide, so the checkbox has no
// meaning for this doc and is greyed out rather than silently doing nothing.
static bool HasIntro(void)
{
    return AnimDocIntroEnd(&zen.doc) > 0.0001f;
}

static int ExportFrameCount(void)
{
    float a, b;
    ExportRange(&a, &b);
    int n = (int)((b - a) * (float)s_fps + 0.5f);
    return n < 1 ? 1 : n;
}

// Highest fps worth offering for the current format.
//
// GIF stores its frame delay in CENTISECONDS, so the only rates it can express
// exactly are 100/n: 50, 33.3, 25, 20... Past 50 the quantization gets coarse
// enough to be a lie (asking for 60 plays at 50), and 100 is the format's hard
// ceiling. It is capped at 60 to stay in the range where the "plays at X fps"
// note below is a small correction rather than a different animation.
// Video has no such quantization - its timebase is exact - so it goes to 120.
static int MaxFps(void)
{
    return (s_format == EXP_FMT_GIF) ? 60 : 120;
}

// CRF from the 0..100 quality slider, inverted: more quality = lower CRF.
static int VideoCRF(void)
{
    float q = (float)s_videoQuality / 100.0f;
    return (int)(34.0f - q * 16.0f + 0.5f);     // 34 (worst) .. 18 (best)
}

static const char *HumanSize(double bytes)
{
    if (bytes < 1024.0)              return TextFormat("%d B", (int)bytes);
    if (bytes < 1024.0*1024.0)       return TextFormat("%.0f KB", bytes/1024.0);
    return TextFormat("%.1f MB", bytes/(1024.0*1024.0));
}

// Output size guess.
//
// CALIBRATED, NOT DERIVED. The coefficients below were fitted against real
// exports of four of this project's own animations across 16 fps/scale
// combinations, because the obvious "pixels x bytes-per-pixel" model was wrong
// by up to 112x. Two things break it:
//
//   - Upscaling does NOT cost proportionally more. A 2x nearest-neighbour
//     upscale duplicates every pixel, and both LZW (gif/png) and H.264 encode
//     that redundancy almost for free. Cost grows about scale^1.1 for the
//     LZW formats and scale^0.6 for H.264 - not scale^2. So the estimate is
//     built from BASE canvas pixels with an explicit scale exponent.
//   - Compressed size tracks how much the animation MOVES, which nothing in
//     these settings can observe. That is the error floor, and it is why the
//     video numbers are labelled a rough guess rather than an estimate.
//
// Accuracy on the fitting set: gif within 1.45x, png similar, video within
// ~2.1x, uncompressed AVI exact.
static double EstimateBytes(void)
{
    int w, h; ExportSize(&w, &h);
    int frames = ExportFrameCount();
    Vector2 game = ScreenStateTargetSize();
    double basePx = (double)game.x * (double)game.y * (double)frames;
    double sc = (double)s_scale;

    switch (s_format)
    {
        case EXP_FMT_GIF:
            // quality is the palette depth, which moves the size roughly
            // linearly once the frame differencing has had its say.
            return 0.0112 * basePx * pow(sc, 1.09) *
                   ((double)s_gifQuality / 16.0);
        case EXP_FMT_PNG:
            return 0.0121 * basePx * pow(sc, 1.09);
        case EXP_FMT_AVI:
            // rawvideo bgr24 is 3 bytes per pixel with no compression at all:
            // this one is exact arithmetic, not a guess.
            return (double)w * (double)h * (double)frames * 3.0;
        case EXP_FMT_MP4:
        case EXP_FMT_WEBM:
        {
            // CRF moves the size a little under 2x per 6 points, the usual
            // rule of thumb; the rest is the fitted content-independent floor.
            double crf = (double)VideoCRF();
            double q = pow(0.89, crf - 23.0);
            double bytes = 0.01146 * (double)game.x * (double)game.y *
                           pow((double)frames, 0.55) * pow(sc, 0.6) * q;
            if (s_format == EXP_FMT_WEBM) bytes *= 0.85;
            return bytes;
        }
        default: return 0.0;
    }
}

// Strip anything that has no business in a filename. The name reaches a shell
// command line for video export, so this is a correctness guard, not polish.
static void SanitizeName(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '"' || *p == '\'' || *p == '\\' || *p == '/' ||
            *p == '`'  || *p == '$'  || *p == ';'  || *p == '&' ||
            *p == '|'  || *p == '<'  || *p == '>'  || *p == '\n')
            *p = '_';
}

static void DefaultName(void)
{
    // an unsaved doc has no file to borrow a name from.
    const char *nm = (zen.animCurrent >= 0 && zen.animCurrent < zen.animCount)
                   ? zen.animList[zen.animCurrent] : "animation";
    TextCopy(s_nameBuf, nm);
    SanitizeName(s_nameBuf);
}

// ---------------------------------------------------------------------------
//  Job teardown. Runs on success, failure and cancel alike - every early exit
//  path goes through here so a half-open encoder can never leak.
// ---------------------------------------------------------------------------
static void JobCleanup(bool deletePartial)
{
    if (s_gifOpen)
    {
        msf_gif_end_to_file(&s_gif);
        s_gifOpen = false;
    }
    if (s_gifFile) { fclose(s_gifFile); s_gifFile = NULL; }
#if ZEN_EXPORT_VIDEO
    if (s_pipe) { ExpPclose(s_pipe); s_pipe = NULL; }
#endif
    if (s_rtValid) { UnloadRenderTexture(s_rt); s_rtValid = false; }

    if (deletePartial && s_outPath[0] && s_format != EXP_FMT_PNG &&
        FileExists(s_outPath))
        remove(s_outPath);
}

static void JobFail(const char *msg)
{
    JobCleanup(true);
    s_state = EXP_FAILED;
    TextCopy(s_status, msg);
}

// ---------------------------------------------------------------------------
//  Job start
// ---------------------------------------------------------------------------
static void JobStart(void)
{
    if (s_nameBuf[0] == '\0') DefaultName();
    SanitizeName(s_nameBuf);

    if (!DirectoryExists(EXP_DIR)) MakeDirectory(EXP_DIR);

    int w, h; ExportSize(&w, &h);
    s_total = ExportFrameCount();
    s_frame = 0;
    s_bytes = 0;
    float end;
    ExportRange(&s_tStart, &end);

    if (s_format == EXP_FMT_PNG)
    {
        // a sequence is many files, so it gets its own folder rather than
        // scattering hundreds of pngs next to the single-file exports.
        TextCopy(s_outPath, TextFormat("%s/%s", EXP_DIR, s_nameBuf));
        if (!DirectoryExists(s_outPath)) MakeDirectory(s_outPath);
    }
    else
        TextCopy(s_outPath, TextFormat("%s/%s%s", EXP_DIR, s_nameBuf,
                                       s_fmt[s_format].ext));

    // the scene always renders at the NATIVE canvas size and is upscaled per
    // frame - element geometry is a fraction of the target size, so rendering
    // large would reposition everything rather than enlarge it.
    Vector2 game = ScreenStateTargetSize();
    s_rt = LoadRenderTexture((int)game.x, (int)game.y);
    if (s_rt.id == 0) { JobFail("could not create the render target"); return; }
    s_rtValid = true;
    SetTextureFilter(s_rt.texture, TEXTURE_FILTER_POINT);

    if (s_format == EXP_FMT_GIF)
    {
        s_gifFile = fopen(s_outPath, "wb");
        if (!s_gifFile) { JobFail("could not open the output file for writing"); return; }
        // 1-bit alpha: 0 disables it entirely, which is what an opaque gif wants.
        msf_gif_alpha_threshold = s_transparent ? 128 : 0;
        if (!msf_gif_begin_to_file(&s_gif, w, h, (MsfGifFileWriteFunc)fwrite,
                                   (void *)s_gifFile))
        { JobFail("gif encoder failed to start (out of memory?)"); return; }
        s_gifOpen = true;
    }
#if ZEN_EXPORT_VIDEO
    else if (s_format != EXP_FMT_PNG)
    {
        const char *codec;
        char extra[160];
        if (s_format == EXP_FMT_AVI)
        {
            codec = "rawvideo";
            TextCopy(extra, "-pix_fmt bgr24");
        }
        else if (s_format == EXP_FMT_WEBM)
        {
            // No alpha here on purpose. libvpx-vp9 ACCEPTS yuva420p and even
            // reports the stream as such, but the frames decode back fully
            // opaque - verified by round-tripping a half-transparent clip.
            // Transparency is offered for GIF and PNG, which actually keep it.
            codec = "libvpx-vp9";
            snprintf(extra, sizeof(extra), "-crf %d -b:v 0 -pix_fmt yuv420p",
                     VideoCRF());
        }
        else
        {
            codec = "libx264";
            // yuv420p + even dimensions: without both, the result will not play
            // in browsers or most players regardless of it being valid H.264.
            snprintf(extra, sizeof(extra),
                     "-crf %d -pix_fmt yuv420p "
                     "-vf \"scale=trunc(iw/2)*2:trunc(ih/2)*2\"", VideoCRF());
        }

        char cmd[900];
        snprintf(cmd, sizeof(cmd),
                 "ffmpeg -y -hide_banner -loglevel error "
                 "-f rawvideo -pix_fmt rgba -s %dx%d -r %d -i - "
                 "-c:v %s %s \"%s\"",
                 w, h, s_fps, codec, extra, s_outPath);

        s_pipe = ExpPopen(cmd);
        if (!s_pipe) { JobFail("could not start ffmpeg"); return; }
        #if defined(_WIN32)
            // Without this the CRT rewrites 0x0A bytes in the RGBA stream as
            // CRLF and every single frame arrives corrupted.
            _setmode(_fileno(s_pipe), _O_BINARY);
        #endif
    }
#endif

    s_jobStart = GetTime();
    s_state = EXP_RUNNING;
    s_status[0] = '\0';
}

// ---------------------------------------------------------------------------
//  Render one frame and hand it to the active encoder.
// ---------------------------------------------------------------------------
static bool JobStep(void)
{
    int w, h; ExportSize(&w, &h);
    float t = s_tStart + (float)s_frame / (float)s_fps;

    // Only GIF and PNG can carry alpha; the video encoders would bake a
    // transparent clear into black, so they always get the real background.
    bool alpha = s_transparent && (s_format == EXP_FMT_GIF ||
                                   s_format == EXP_FMT_PNG);
    BeginTextureMode(s_rt);
        ClearBackground(alpha ? BLANK : ScreenStateGet()->clear_color);
        AnimDocDrawLoop(&zen.doc, t, NULL, !s_showIntro);
    EndTextureMode();

    Image img = LoadImageFromTexture(s_rt.texture);
    if (!img.data) return false;
    // render textures are bottom-left origin (the same reason screen_state.c
    // loads its target with a negative height).
    ImageFlipVertical(&img);
    if (s_scale > 1) ImageResizeNN(&img, w, h);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    bool ok = true;
    if (s_format == EXP_FMT_GIF)
    {
        // gif delays are centiseconds - a format limitation, not a choice.
        int cs = (int)(100.0f / (float)s_fps + 0.5f);
        if (cs < 1) cs = 1;
        ok = msf_gif_frame_to_file(&s_gif, (uint8_t *)img.data, cs,
                                   s_gifQuality, w * 4) != 0;
    }
    else if (s_format == EXP_FMT_PNG)
    {
        const char *fp = TextFormat("%s/%s_%04d.png", s_outPath, s_nameBuf,
                                    s_frame);
        ok = ExportImage(img, fp);
        if (ok) s_bytes += (long long)GetFileLength(fp);
    }
#if ZEN_EXPORT_VIDEO
    else if (s_pipe)
        ok = fwrite(img.data, 4, (size_t)w * (size_t)h, s_pipe) ==
             (size_t)w * (size_t)h;
#endif

    UnloadImage(img);
    return ok;
}

static void JobFinish(void)
{
    // the gif and the pipe are only complete once their encoder is closed, so
    // the on-disk size can't be read before cleanup.
    bool png = (s_format == EXP_FMT_PNG);
    JobCleanup(false);

    if (!png) s_bytes = FileExists(s_outPath) ? (long long)GetFileLength(s_outPath) : 0;

    s_state = EXP_DONE;
    double secs = GetTime() - s_jobStart;
    TextCopy(s_status, TextFormat("%d frames in %.1fs - %s", s_total, secs,
                                  HumanSize((double)s_bytes)));
}

// ---------------------------------------------------------------------------
//  Accessors for the Gui()/ESC plumbing in anim_editor_zen.c
// ---------------------------------------------------------------------------
void ZenExportShow(void)
{
    s_open = true;
    s_fmtDrop = false;
    s_edName = false;
    s_edFps = false;
    s_status[0] = '\0';
    s_state = EXP_IDLE;

    if (s_ffmpeg < 0) s_ffmpeg = FfmpegProbe() ? 1 : 0;
    // land on something that can actually run rather than a disabled row.
    if (!FormatAvailable(s_format)) s_format = EXP_FMT_GIF;
    if (s_nameBuf[0] == '\0') DefaultName();

    if (!s_placed)
    {
        Vector2 screen = ScreenStateSize();
        s_pos = (Vector2){ (screen.x - EXP_W) * 0.5f, screen.y * 0.16f };
        s_placed = true;
    }
}

bool ZenExportOpen(void)   { return s_open; }
bool ZenExportBusy(void)   { return s_open && s_state == EXP_RUNNING; }
bool ZenExportTyping(void) { return s_open && (s_edName || s_edFps); }
Rectangle ZenExportRect(void) { return s_open ? s_rect : (Rectangle){ 0 }; }

bool ZenExportEscClose(void)
{
    if (!s_open) return false;
    // two-stage: a running export is what ESC cancels first. Closing the modal
    // out from under a live encoder would strand the job with nothing driving
    // it, since the render loop lives in ZenExportGui.
    if (s_state == EXP_RUNNING)
    {
        JobCleanup(true);
        s_state = EXP_IDLE;
        TextCopy(s_status, "export cancelled");
        return true;
    }
    if (s_fmtDrop) { s_fmtDrop = false; return true; }
    s_open = false; s_edName = false; s_edFps = false;
    return true;
}

// ---------------------------------------------------------------------------
//  Draw + drive the job
// ---------------------------------------------------------------------------
void ZenExportGui(void)
{
    if (!s_open) { s_rect = (Rectangle){ 0 }; return; }

    bool running = (s_state == EXP_RUNNING);
    bool video   = (s_format == EXP_FMT_MP4 || s_format == EXP_FMT_WEBM ||
                    s_format == EXP_FMT_AVI);
    bool avail   = FormatAvailable(s_format);

    // -- height depends on which format's panel is showing --------------------
    float bodyH = EXP_TITLE_H + 8
                + EXP_ROW_H + EXP_GAP        // format
                + EXP_ROW_H + EXP_GAP        // name
                + 18 + EXP_GAP               // resolved path
                + EXP_ROW_H + EXP_GAP        // scale
                + EXP_ROW_H + EXP_GAP        // fps
                + EXP_ROW_H + EXP_GAP        // loop range
                + 18 + EXP_GAP;              // frames/duration line
    if (s_format == EXP_FMT_GIF)  bodyH += 2*(EXP_ROW_H + EXP_GAP);  // quality + alpha
    if (s_format == EXP_FMT_PNG)  bodyH += EXP_ROW_H + EXP_GAP;      // alpha
    if (video && s_format != EXP_FMT_AVI) bodyH += EXP_ROW_H + EXP_GAP;  // quality
    bodyH += 18 + EXP_GAP;                   // estimate
    if (!avail)   bodyH += 18 + EXP_GAP;     // ffmpeg-missing hint
    if (running)  bodyH += EXP_ROW_H + EXP_GAP;
    if (s_status[0]) bodyH += 18 + EXP_GAP;
    bodyH += 28 + 12 + 8;                    // footer

    Rectangle m = { s_pos.x, s_pos.y, EXP_W, bodyH };
    Vector2 screen = ScreenStateSize();
    if (m.x < 0) m.x = 0; if (m.y < 0) m.y = 0;
    if (m.x + m.width  > screen.x) m.x = screen.x - m.width;
    if (m.y + m.height > screen.y) m.y = screen.y - m.height;
    s_pos = (Vector2){ m.x, m.y };
    s_rect = m;

    // -- chrome ---------------------------------------------------------------
    Rectangle title = { m.x, m.y, m.width - 24, EXP_TITLE_H };
    ZenModalDrag(title, &s_pos, (Vector2){ m.x, m.y }, &s_dragging, &s_dragOff,
                 ZEN_LAYER_FLOAT_EXPORT);

    DrawRectangleRec(m, (Color){ 40, 42, 48, 252 });
    DrawRectangleLinesEx(m, 1.0f, (Color){ 110, 114, 126, 255 });
    DrawRectangleRec(title, (Color){ 52, 56, 66, 255 });
    ZenLabelTip((Rectangle){ m.x + 8, m.y + 3, title.width - 8, 18 }, "EXPORT",
                "Drag this bar to move the modal");
    if (GuiButton((Rectangle){ m.x + m.width - 22, m.y + 2, 20, 20 }, "x"))
    {
        AudioPlayButton();
        if (running) { JobCleanup(true); s_state = EXP_IDLE; }
        s_open = false;
        return;
    }

    float x = m.x + 12, wIn = EXP_W - 24;
    float cy = m.y + EXP_TITLE_H + 8;
    float lw = 86;      // label column

    // settings are frozen while rendering: changing fps or scale mid-job would
    // desync the frames already handed to the encoder.
    if (running) GuiDisable();

    // -- format ---------------------------------------------------------------
    GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Format");
    Rectangle fr = { x + lw, cy, wIn - lw, EXP_ROW_H };
    if (GuiButton(fr, TextFormat("%s   #120#", s_fmt[s_format].label)))
    { AudioPlayButton(); s_fmtDrop = !s_fmtDrop; }
    s_fmtDropRect = fr;
    ZenTip(fr, s_fmt[s_format].desc);
    cy += EXP_ROW_H + EXP_GAP;

    // -- name -----------------------------------------------------------------
    GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Name");
    if (GuiTextBox((Rectangle){ x + lw, cy, wIn - lw, EXP_ROW_H }, s_nameBuf,
                   (int)sizeof(s_nameBuf), s_edName))
    { s_edName = !s_edName; if (!s_edName) SanitizeName(s_nameBuf); }
    ZenTip((Rectangle){ x + lw, cy, wIn - lw, EXP_ROW_H },
           "Output file name, without the extension");
    cy += EXP_ROW_H + EXP_GAP;

    const char *shownPath = (s_format == EXP_FMT_PNG)
        ? TextFormat("%s/%s/%s_0000.png", EXP_DIR, s_nameBuf, s_nameBuf)
        : TextFormat("%s/%s%s", EXP_DIR, s_nameBuf, s_fmt[s_format].ext);
    ZenLabelTip((Rectangle){ x, cy, wIn, 18 }, shownPath,
                "Where the export is written, relative to the game folder");
    cy += 18 + EXP_GAP;

    // -- scale ----------------------------------------------------------------
    int w, h; ExportSize(&w, &h);
    GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Scale");
    int scaleIdx = s_scale - 1;
    if (GuiToggleGroup((Rectangle){ x + lw, cy, 52, EXP_ROW_H },
                       "1x;2x;3x;4x", &scaleIdx))
        AudioPlayButton();
    if (scaleIdx + 1 != s_scale) { s_scale = scaleIdx + 1; ExportSize(&w, &h); }
    GuiLabel((Rectangle){ x + lw + 4*54 + 8, cy, 100, EXP_ROW_H },
             TextFormat("%d x %d", w, h));
    ZenTip((Rectangle){ x + lw, cy, 4*54, EXP_ROW_H },
           "Whole-number upscale of the canvas. Nearest-neighbour, so pixels "
           "stay sharp instead of blurring.");
    cy += EXP_ROW_H + EXP_GAP;

    // -- fps ------------------------------------------------------------------
    GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "FPS");
    Rectangle fpsR = { x + lw, cy, 110, EXP_ROW_H };
    int fpsMax = MaxFps();

    // Ctrl+Click steps by 10. GuiSpinner hardcodes +/-1 with no step parameter,
    // so the arrows are intercepted BEFORE it runs: it is handed a scratch copy
    // that it bumps by its own 1, and that result is discarded on a ctrl-click
    // in favour of the 10 applied here. Cheaper than reimplementing the widget,
    // and it keeps the normal click path exactly as raygui draws it.
    bool ctrlStep = false;
    if (!running && (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
        !s_edFps && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        ZenLayerActive(ZEN_LAYER_FLOAT_EXPORT))
    {
        float bw = (float)GuiGetStyle(VALUEBOX, SPINNER_BUTTON_WIDTH);
        Rectangle lb = { fpsR.x, fpsR.y, bw, fpsR.height };
        Rectangle rb = { fpsR.x + fpsR.width - bw, fpsR.y, bw, fpsR.height };
        Vector2 mp = GetMousePosition();
        int dir = 0;
        if (CheckCollisionPointRec(mp, lb))      dir = -1;
        else if (CheckCollisionPointRec(mp, rb)) dir = +1;
        if (dir)
        {
            ctrlStep = true;
            AudioPlayButton();
            // Snap to the next multiple of 10 in the direction travelled rather
            // than adding 10 and flooring: from an off-grid 15, down should
            // reach 10, not fall past it to 1.
            if (dir > 0) s_fps = (s_fps / 10) * 10 + 10;
            else         s_fps = (s_fps % 10) ? (s_fps / 10) * 10
                                              : s_fps - 10;
            if (s_fps < 1) s_fps = 1;
            if (s_fps > fpsMax) s_fps = fpsMax;
        }
    }

    int fpsShown = s_fps;
    if (GuiSpinner(fpsR, NULL, &fpsShown, 1, fpsMax, s_edFps))
        s_edFps = !s_edFps;
    if (!ctrlStep) s_fps = fpsShown;
    ZenTip(fpsR, "Frames rendered per second. Ctrl+Click the arrows to step "
                 "by 10.");

    // a format switch can leave the rate above what the new one allows.
    if (s_fps > fpsMax) s_fps = fpsMax;
    if (s_format == EXP_FMT_GIF)
    {
        // gif delays quantize to centiseconds, so most rates are not exactly
        // representable (only 100/n is). Say what it will ACTUALLY play at
        // rather than showing the requested number and exporting something
        // else - 50, 25 and 20 are exact, 30 and 60 are not.
        int cs = (int)(100.0f / (float)s_fps + 0.5f); if (cs < 1) cs = 1;
        float real = 100.0f / (float)cs;
        if (fabsf(real - (float)s_fps) > 0.05f)
            GuiLabel((Rectangle){ x + lw + 118, cy, 200, EXP_ROW_H },
                     TextFormat("plays at %.1f fps", real));
    }
    cy += EXP_ROW_H + EXP_GAP;

    // -- loop range -----------------------------------------------------------
    // `running` already disabled everything below it, so only touch the gui
    // state when this row is the one adding the restriction - otherwise the
    // GuiEnable() would lift the whole running-job lock for every later widget.
    bool hasIntro  = HasIntro();
    bool ownDisable = !hasIntro && !running;
    if (!hasIntro)
        // Force it off while disabled so the range stays truthful: a checked box
        // that cannot be unchecked must not keep steering ExportRange().
        s_showIntro = false;
    if (ownDisable) GuiDisable();
    GuiCheckBox((Rectangle){ x + lw, cy + 3, 18, 18 }, "Show intro",
                &s_showIntro);
    if (ownDisable) GuiEnable();
    ZenTip((Rectangle){ x + lw, cy + 3, 160, 18 },
           hasIntro ? "Include the run-in the animation plays once before it "
                      "settles. Off exports the looping section only, so the "
                      "result loops seamlessly. The outro is always trimmed."
                    : "This animation has no intro. The looping section is "
                      "exported; the outro is always trimmed.");
    cy += EXP_ROW_H + EXP_GAP;

    float a, b; ExportRange(&a, &b);
    int frames = ExportFrameCount();
    GuiLabel((Rectangle){ x, cy, wIn, 18 },
             TextFormat("%d frames  -  %.2fs", frames, b - a));
    cy += 18 + EXP_GAP;

    // -- per-format controls --------------------------------------------------
    if (s_format == EXP_FMT_GIF)
    {
        GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Quality");
        float q = (float)s_gifQuality;
        GuiSlider((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H }, NULL,
                  TextFormat("%d", s_gifQuality), &q, 1, 16);
        s_gifQuality = (int)(q + 0.5f);
        ZenTip((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H },
               "Colour accuracy per frame (1-16). Lower means a smaller file "
               "and a heavier dither pattern.");
        cy += EXP_ROW_H + EXP_GAP;

        GuiCheckBox((Rectangle){ x + lw, cy + 3, 18, 18 },
                    "Transparent background", &s_transparent);
        ZenTip((Rectangle){ x + lw, cy + 3, 180, 18 },
               "GIF transparency is 1-bit: a pixel is either fully see-through "
               "or fully opaque, with no soft edges.");
        cy += EXP_ROW_H + EXP_GAP;
    }
    else if (video && s_format != EXP_FMT_AVI)
    {
        GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Quality");
        float q = (float)s_videoQuality;
        GuiSlider((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H }, NULL,
                  TextFormat("crf %d", VideoCRF()), &q, 0, 100);
        s_videoQuality = (int)(q + 0.5f);
        ZenTip((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H },
               "Higher is better looking and bigger. Shown as the encoder's "
               "CRF, where a LOWER CRF is higher quality.");
        cy += EXP_ROW_H + EXP_GAP;
    }
    else if (s_format == EXP_FMT_PNG)
    {
        GuiCheckBox((Rectangle){ x + lw, cy + 3, 18, 18 },
                    "Transparent background", &s_transparent);
        ZenTip((Rectangle){ x + lw, cy + 3, 180, 18 },
               "PNG stores a full alpha channel, so soft edges survive - "
               "unlike GIF, which only has on/off transparency.");
        cy += EXP_ROW_H + EXP_GAP;
    }

    // -- estimate -------------------------------------------------------------
    double est = EstimateBytes();
    bool exact = (s_format == EXP_FMT_AVI);
    // video is the least predictable of the three (its size is dominated by how
    // much the picture changes), so it says so rather than borrowing the
    // credibility of the gif/png numbers.
    bool rough = video && !exact;
    GuiLabel((Rectangle){ x, cy, wIn, 18 },
             exact   ? TextFormat("Output size: %s", HumanSize(est))
             : rough ? TextFormat("Rough guess: ~%s", HumanSize(est))
                     : TextFormat("Estimated size: ~%s", HumanSize(est)));
    ZenTip((Rectangle){ x, cy, wIn, 18 },
           exact   ? "Uncompressed video has no compression, so this is exact "
                     "arithmetic rather than a guess."
           : rough ? "Video size depends mostly on how much the picture MOVES, "
                     "which these settings cannot predict. Treat this as an "
                     "order of magnitude - it can be out by 2x either way."
                   : "Fitted against real exports of this project's animations. "
                     "Usually within about 1.5x; busier animations run larger.");
    cy += 18 + EXP_GAP;

    if (!avail)
    {
        GuiLabel((Rectangle){ x, cy, wIn, 18 },
                 "ffmpeg not found on PATH - use GIF or PNG frames");
        ZenTip((Rectangle){ x, cy, wIn, 18 },
               "Video export shells out to ffmpeg. Install it and reopen this "
               "window, or export a PNG sequence and encode it yourself.");
        cy += 18 + EXP_GAP;
    }

    if (running) GuiEnable();

    // -- progress -------------------------------------------------------------
    if (running)
    {
        float p = s_total ? (float)s_frame / (float)s_total : 0.0f;
        double elapsed = GetTime() - s_jobStart;
        const char *eta = "";
        if (s_frame > 4)
        {
            double per = elapsed / (double)s_frame;
            eta = TextFormat("  ~%.0fs left", per * (double)(s_total - s_frame));
        }
        GuiProgressBar((Rectangle){ x, cy, wIn - 96, EXP_ROW_H }, NULL, NULL,
                       &p, 0.0f, 1.0f);
        GuiLabel((Rectangle){ x + wIn - 90, cy, 90, EXP_ROW_H },
                 TextFormat("%d/%d", s_frame, s_total));
        if (eta[0])
            GuiLabel((Rectangle){ x, cy, wIn - 96, EXP_ROW_H }, eta);
        cy += EXP_ROW_H + EXP_GAP;
    }

    if (s_status[0])
    {
        GuiLabel((Rectangle){ x, cy, wIn, 18 }, s_status);
        ZenTip((Rectangle){ x, cy, wIn, 18 }, s_status);
        cy += 18 + EXP_GAP;
    }

    // -- footer ---------------------------------------------------------------
    float bh = 28, by = m.y + bodyH - bh - 12;
    bool canExport = avail && frames > 0 && zen.doc.elemCount > 0;

    if (running)
    {
        if (GuiButton((Rectangle){ m.x + EXP_W - 2*86 - 24, by, 86, bh }, "Cancel"))
        {
            AudioPlayButton();
            JobCleanup(true);
            s_state = EXP_IDLE;
            TextCopy(s_status, "export cancelled");
        }
    }
    else
    {
        if (!canExport) GuiDisable();
        if (GuiButton((Rectangle){ m.x + EXP_W - 2*86 - 24, by, 86, bh }, "Export"))
        { AudioPlayButton(); JobStart(); }
        if (!canExport) GuiEnable();
    }

    if (GuiButton((Rectangle){ m.x + EXP_W - 86 - 12, by, 86, bh }, "Close"))
    {
        AudioPlayButton();
        if (s_state == EXP_RUNNING) { JobCleanup(true); s_state = EXP_IDLE; }
        s_open = false; s_edName = false; s_edFps = false;
        return;
    }

    // -- format dropdown, drawn last so it lands over the rows below it -------
    if (s_fmtDrop)
    {
        float ih = 22.0f, listH = ih * EXP_FMT_COUNT;
        Rectangle hdr = s_fmtDropRect;
        float ly = (hdr.y + hdr.height + listH <= screen.y - 4.0f)
                 ? hdr.y + hdr.height : hdr.y - listH;
        if (ly < 4.0f) ly = 4.0f;
        Rectangle bg = { hdr.x, ly, hdr.width, listH };
        DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
        DrawRectangleLinesEx(bg, 1.0f, (Color){ 70, 74, 84, 255 });

        for (int i = 0; i < EXP_FMT_COUNT; i++)
        {
            Rectangle rr = { bg.x, bg.y + i*ih, bg.width, ih };
            bool ok = FormatAvailable(i);
            if (!ok) GuiDisable();
            if (GuiButton(rr, s_fmt[i].label))
            {
                AudioPlayButton();
                s_fmtDrop = false;
                if (i != s_format)
                {
                    s_format = i;
                    s_state = EXP_IDLE;
                    s_status[0] = '\0';
                    // the panel below changes height, so the rows under the
                    // cursor move. Poison the gesture or whatever slid under
                    // the pointer fires on this same click's release.
                    ZenMouseReflow();
                }
            }
            if (!ok) GuiEnable();
            if (i == s_format)
                DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !CheckCollisionPointRec(GetMousePosition(), bg) &&
            !CheckCollisionPointRec(GetMousePosition(), hdr))
            s_fmtDrop = false;
    }

    // -- advance the job ------------------------------------------------------
    // After drawing, so the bar the user sees matches the frame count that was
    // true when it was drawn, and a cancel this frame is honoured before any
    // more work happens.
    if (s_state == EXP_RUNNING)
    {
        for (int i = 0; i < EXPORT_CHUNK && s_state == EXP_RUNNING; i++)
        {
            if (!JobStep()) { JobFail("encoder failed while writing a frame"); return; }
            s_frame++;
            if (s_frame >= s_total) { JobFinish(); return; }
        }
    }
}
