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
#include "rlgl.h"
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

// Links in one export. Each row costs EXP_CHAIN_ROW_H + 2 = 24px of modal, so 8
// is 192px of growth - which still fits a 720-tall window alongside the rest of
// the panel. More than that would need a scrolling sub-list inside a modal that
// is already the tallest thing in the editor.
#define EXP_CHAIN_MAX    8
#define EXP_CHAIN_ROW_H  22.0f
#define EXP_ADD_H        24.0f

#if !defined(PLATFORM_WEB)
// Opens the exports folder in the OS file manager. The three platforms share no
// common command, and the path handed over must be absolute - EXP_DIR is relative
// to the game's working directory, which the file manager knows nothing about.
//
// The folder is a compile-time constant, so no user text ever reaches this command
// line - unlike the output name, which SanitizeName() has to guard because it is
// passed to ffmpeg. The quotes here are for paths containing spaces, nothing more.
static void OpenExportDir(void)
{
    const char *abs = TextFormat("%s/%s", GetWorkingDirectory(), EXP_DIR);
    #if defined(_WIN32)
        system(TextFormat("explorer \"%s\"", abs));
    #elif defined(__APPLE__)
        system(TextFormat("open \"%s\"", abs));
    #else
        // & so a file manager that does not detach cannot freeze the editor:
        // system() blocks until the child exits, and a GUI app exits when the
        // user closes its window.
        system(TextFormat("xdg-open \"%s\" &", abs));
    #endif
}
#endif

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
// How opaque a pixel must be to survive a transparent gif. The format has no
// partial alpha, so a fade has to snap somewhere and where it snaps is a
// per-animation judgement call - hence a control rather than a constant.
static int   s_gifCutout = 128;         // 1..255
static int   s_videoQuality = 70;       // 0..100, mapped to CRF
static char  s_nameBuf[96] = "";
static bool  s_edName = false;
static bool  s_edFps = false;
// Seconds to freeze on each pause marker. A marker is a hold that waits for a
// KEYPRESS, and an exported file has nobody to press one, so the hold has to
// become a duration here or it vanishes from the output. 0 = the old behaviour,
// where the export runs straight through every marker.
static float s_pauseHold = 0.0f;

// format dropdown
static bool      s_fmtDrop = false;
static Rectangle s_fmtDropRect = { 0 };

// "+ Add animation" picker: a searchable list of every saved animation, drawn
// inline in the modal rather than as its own centered one. A full modal would
// dim and GuiLock the export modal it was opened FROM - including a running
// job's Cancel button.
static bool      s_addDrop = false;
static Rectangle s_addDropRect = { 0 };
static char      s_addSearch[ANIM_NAME_MAX] = "";
static bool      s_addSearchEdit = false;
static float     s_addScroll = 0.0f;

// job
static ExpState        s_state = EXP_IDLE;
static int             s_frame = 0, s_total = 0;
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

// ---------------------------------------------------------------------------
//  The chain
//
//  An export is an ordered list of animations played back to back, not one
//  document. A single animation is just the N=1 case of it, which is why there
//  is no separate code path for the common one.
//
//  Every link - INCLUDING the animation open in the editor - is loaded from its
//  .cfg at JobStart. That uniformity is why Export prompts to save a dirty
//  document first: without the prompt, the one entry that could have differed
//  from its file would silently export stale.
// ---------------------------------------------------------------------------
typedef struct { char name[ANIM_NAME_MAX]; } ExpChainEntry;

static ExpChainEntry s_chain[EXP_CHAIN_MAX];
static int           s_chainCount = 0;
// Whether a beat is held at each JOIN between animations. Without it clips hard
// cut into each other and the transition reads as a glitch. It reuses the
// s_pauseHold duration below rather than adding a second slider - the join is
// the same idea as a pause marker, applied where two documents meet.
static bool          s_joinPause = true;

// One document per link, loaded once at JobStart and held for the whole job.
// ~45 KB each, so 8 links is ~360 KB - against the 59 MB render target this
// same job allocates at 4x on a 1280x720 canvas. Streaming one at a time would
// put file I/O inside JobStep's per-frame path and move a failure mode from
// "before the encoder opens" to "halfway through a written file".
static AnimDoc s_scratch[EXP_CHAIN_MAX];

// A resolved link, frozen at JobStart. Everything the frame->time map needs, so
// JobStep never re-reads the chain or the settings: the document and the modal
// both stay live while a job runs, and s_total is already committed.
//
// holdAt lives HERE rather than in one flat file-static array. A flat
// ANIM_PAUSES_MAX array would be overrun by the second segment's markers.
typedef struct {
    const AnimDoc *doc;                 // &s_scratch[k]
    float start, end;                   // doc-clock range, [start, end)
    int   baseFrames;                   // (end-start)*fps, free-running
    float holdAt[ANIM_PAUSES_MAX];      // markers inside [start,end), ascending
    int   holdCount;
    int   joinHoldFrames;               // held AFTER this segment (0 on the last)
    int   frames;                       // baseFrames + holdCount*hf + joinHoldFrames
    int   offset;                       // global frame index of this segment's frame 0
} ExpSegment;

static ExpSegment s_seg[EXP_CHAIN_MAX];
static int        s_segCount = 0;

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

static void DocExportRange(const AnimDoc *d, bool showIntro,
                           float *start, float *end)
{
    // The outro is ALWAYS trimmed - it is the tail the doc plays on the way out,
    // and an export that includes it cannot loop. The intro is optional: it is
    // the run-in the animation plays once, so showing it makes a one-shot clip
    // and hiding it makes a seamless loop.
    *start = showIntro ? 0.0f : AnimDocIntroEnd(d);
    *end   = AnimDocOutroStart(d);
    if (*end < *start) *end = *start;
}

// An intro of zero length is nothing to show or hide, so the checkbox has no
// meaning for this doc and is greyed out rather than silently doing nothing.
static bool DocHasIntro(const AnimDoc *d)
{
    return AnimDocIntroEnd(d) > 0.0001f;
}

// The pause markers inside the export range, ascending (doc.pauses already is).
//
// Containment is [a, b), NOT the (from, to] that AnimDocNextPause uses. That
// exclusive start is a RESUME rule - it stops the runtime re-firing the marker
// it just released - and a single forward export pass has no such history. Used
// here it would silently drop a marker sitting exactly at the range start,
// which is precisely where an intro-trimmed loop tends to put one.
//
// `once` is ignored for the same reason: an export is one pass, so every marker
// is on its first pass and `once` can never suppress anything.
static int DocHoldMarkers(const AnimDoc *d, float a, float b, float *out)
{
    int n = 0;
    for (int i = 0; i < d->pauseCount; i++)
    {
        float t = d->pauses[i].t;
        if (t >= a && t < b) out[n++] = t;
    }
    return n;
}

static int HoldFrameCount(void)
{
    int f = (int)(s_pauseHold * (float)s_fps + 0.5f);
    return f < 0 ? 0 : f;
}

// ---------------------------------------------------------------------------
//  Chain planning
// ---------------------------------------------------------------------------

// Resolve the chain into s_seg[], loading every link's document. Called from
// JobStart only; the modal's frames/estimate readout works off the cheaper
// s_info cache instead, because loading eight .cfg files to draw eight labels
// EVERY FRAME would be absurd.
//
// This one is authoritative. If it ever disagrees with what the modal
// predicted, the cost is a progress bar that reads slightly off - never a
// broken output, because s_total comes from here.
static bool ChainBuild(void)
{
    s_segCount = 0;
    int off = 0, hf = HoldFrameCount();

    for (int k = 0; k < s_chainCount; k++)
    {
        if (!AnimDocLoad(&s_scratch[k], ZenAnimPath(s_chain[k].name)))
            return false;

        ExpSegment *g = &s_seg[s_segCount];
        g->doc = &s_scratch[k];
        DocExportRange(g->doc, s_showIntro, &g->start, &g->end);
        g->baseFrames = (int)((g->end - g->start) * (float)s_fps + 0.5f);
        // Clamped PER SEGMENT, not just on the total: a degenerate link still
        // has to occupy a frame, or the segments after it start at an offset
        // the map can never reach.
        if (g->baseFrames < 1) g->baseFrames = 1;
        g->holdCount = DocHoldMarkers(g->doc, g->start, g->end, g->holdAt);

        bool joins = (s_chainCount > 1) && s_joinPause && (k < s_chainCount - 1);
        g->joinHoldFrames = joins ? hf : 0;
        g->frames = g->baseFrames + g->holdCount*hf + g->joinHoldFrames;
        g->offset = off;
        off += g->frames;
        s_segCount++;
    }
    return s_segCount > 0;
}

static int ChainFrameCount(void)
{
    int n = 0;
    for (int i = 0; i < s_segCount; i++) n += s_seg[i].frames;
    return n < 1 ? 1 : n;
}

// Global frame index -> (segment, doc time). The one place the export's clock
// is defined.
//
// Segments lie end to end: segment k owns global frames
// [offset, offset + frames). Inside one, the map is the single-document one -
// the marker walk below is unchanged except that `frame` is rebased to a
// segment-local index. A marker in segment k can only push frames later WITHIN
// k, because its hold is already counted in k's `frames` and therefore in every
// later `offset`.
//
// The linear search is over at most EXP_CHAIN_MAX segments, once per frame,
// next to a full scene draw and a GPU readback. A binary search would be noise.
static const ExpSegment *SegmentAt(int frame, float *tOut)
{
    const ExpSegment *g = &s_seg[s_segCount - 1];
    for (int i = 0; i < s_segCount; i++)
        if (frame < s_seg[i].offset + s_seg[i].frames) { g = &s_seg[i]; break; }

    int local = frame - g->offset;
    if (local < 0) local = 0;

    int hf = HoldFrameCount();
    int freeAndHeld = g->baseFrames + g->holdCount*hf;

    // -- the join beat: parked on this segment's LAST PLAYED pose -------------
    // baseFrames-1, not g->end. `end` is exclusive and the free-running path
    // below never reaches it either; parking on it would show a pose the clip
    // does not play - a one-frame pop right before the cut.
    if (local >= freeAndHeld)
    {
        *tOut = g->start + (float)(g->baseFrames - 1) / (float)s_fps;
        return g;
    }

    if (hf <= 0 || g->holdCount <= 0)
    {
        *tOut = g->start + (float)local / (float)s_fps;
        return g;
    }

    // Walk the markers in order, tracking where each one lands once the earlier
    // holds have pushed it later. At most ANIM_PAUSES_MAX of them.
    //
    // The marker's OWN frame is the last free-running one, and the hold adds hf
    // after it - hence `at + 1`. Counting the marker's frame as the first held
    // frame instead would show that pose hf + 1 times and make a "2 second"
    // hold two seconds plus a frame.
    int consumed = 0;
    for (int i = 0; i < g->holdCount; i++)
    {
        int at = (int)((g->holdAt[i] - g->start) * (float)s_fps + 0.5f) + consumed;
        if (local <= at) break;                                     // running freely
        if (local <= at + hf) { *tOut = g->holdAt[i]; return g; }   // parked on it
        consumed += hf;
    }
    *tOut = g->start + (float)(local - consumed) / (float)s_fps;
    return g;
}

// ---------------------------------------------------------------------------
//  Chain summary cache
//
//  The modal draws each link's duration and derives the frame count, the
//  estimate and every enable rule from the whole chain - all of which need the
//  links' documents. Re-reading eight .cfg files every frame to render eight
//  labels is not acceptable, so the few numbers the UI actually wants are
//  cached and refreshed only when the chain CHANGES.
//
//  What the UI derives per frame (frames, ranges, sizes) is computed from these
//  numbers plus the live fps / intro / hold settings, so a slider drag costs no
//  file I/O.
// ---------------------------------------------------------------------------
typedef struct {
    bool  ok;               // the .cfg resolved
    bool  hasIntro;
    float introEnd, outroStart;
    int   markersFull;      // markers in [0, outroStart)   - intro shown
    int   markersLoop;      // markers in [introEnd, outroStart) - intro trimmed
    int   elemCount;
} ExpChainInfo;

static ExpChainInfo s_info[EXP_CHAIN_MAX];

// One reusable document for the summary pass. Separate from s_scratch[] on
// purpose: that array belongs to a running job and must not be disturbed by the
// modal redrawing behind it.
static AnimDoc s_peek;

static void ChainRefreshInfo(void)
{
    for (int k = 0; k < s_chainCount; k++)
    {
        ExpChainInfo *in = &s_info[k];
        *in = (ExpChainInfo){ 0 };
        if (!AnimDocLoad(&s_peek, ZenAnimPath(s_chain[k].name))) continue;

        in->ok         = true;
        in->hasIntro   = DocHasIntro(&s_peek);
        in->introEnd   = AnimDocIntroEnd(&s_peek);
        in->outroStart = AnimDocOutroStart(&s_peek);
        in->elemCount  = s_peek.elemCount;

        float a, b, marks[ANIM_PAUSES_MAX];
        DocExportRange(&s_peek, true,  &a, &b);
        in->markersFull = DocHoldMarkers(&s_peek, a, b, marks);
        DocExportRange(&s_peek, false, &a, &b);
        in->markersLoop = DocHoldMarkers(&s_peek, a, b, marks);
    }
}

// Length of link k as the CURRENT settings would export it, before any holds.
static float ChainPlayLen(int k)
{
    const ExpChainInfo *in = &s_info[k];
    if (!in->ok) return 0.0f;
    float a = s_showIntro ? 0.0f : in->introEnd;
    float b = in->outroStart;
    return (b > a) ? b - a : 0.0f;
}

static int ChainBaseFrames(int k)
{
    int n = (int)(ChainPlayLen(k) * (float)s_fps + 0.5f);
    return n < 1 ? 1 : n;
}

// Pause markers across the whole chain, under the current intro setting.
static int ChainMarkerCount(void)
{
    int n = 0;
    for (int k = 0; k < s_chainCount; k++)
        if (s_info[k].ok)
            n += s_showIntro ? s_info[k].markersFull : s_info[k].markersLoop;
    return n;
}

// Joins that will actually hold: one between each adjacent pair, and only when
// the chain has more than one link and the user asked for them.
static int ChainJoinCount(void)
{
    if (s_chainCount < 2 || !s_joinPause) return 0;
    return s_chainCount - 1;
}

// Frames the whole chain will export, from the cache. ChainBuild() recomputes
// this from freshly loaded documents at JobStart and that result is the one the
// job runs on; this is the modal's preview of it.
static int ChainFrameCountUI(void)
{
    int hf = HoldFrameCount(), n = 0;
    for (int k = 0; k < s_chainCount; k++) n += ChainBaseFrames(k);
    n += (ChainMarkerCount() + ChainJoinCount()) * hf;
    return n < 1 ? 1 : n;
}

// Frames spent frozen - in-document markers and join beats alike. They are real
// seconds of output, so they count toward the duration; they are also nearly
// free to encode, which is what EstimateFrames() discounts.
static int ChainHeldFrames(void)
{
    return (ChainMarkerCount() + ChainJoinCount()) * HoldFrameCount();
}

// The intro checkbox is meaningful if ANY link has a run-in. Asking only the
// first would grey out a box that links 2 and 3 need.
static bool ChainHasIntro(void)
{
    for (int k = 0; k < s_chainCount; k++)
        if (s_info[k].ok && s_info[k].hasIntro) return true;
    return false;
}

// Every link still resolves to a file on disk.
static bool ChainResolvable(void)
{
    for (int k = 0; k < s_chainCount; k++)
        if (!s_info[k].ok) return false;
    return s_chainCount > 0;
}

// At least one link has something in it. Requiring EVERY link to be non-empty
// would be wrong - a deliberately blank clip used as a gap is legitimate - but
// a chain of nothing but blanks exports a blank file, which is never wanted.
static bool ChainAnyElems(void)
{
    for (int k = 0; k < s_chainCount; k++)
        if (s_info[k].ok && s_info[k].elemCount > 0) return true;
    return false;
}

// Does the chain include the animation currently open in the editor? Only then
// do unsaved edits matter, and only then is the save prompt worth showing.
static bool ChainUsesCurrent(void)
{
    if (zen.animCurrent < 0 || zen.animCurrent >= zen.animCount) return false;
    const char *cur = zen.animList[zen.animCurrent];
    for (int k = 0; k < s_chainCount; k++)
        if (TextIsEqual(s_chain[k].name, cur)) return true;
    return false;
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
//
// Frames held on a pause marker - or on a join between two chained animations -
// are charged at a DISCOUNT for the formats that difference between frames: a
// held frame is pixel-identical to the one before it, so gif's frame
// differencing and H.264's P-frames encode it for almost nothing. Charging it
// in full would let a 12-second hold triple a size that barely moves. PNG (a
// whole file per frame) and raw AVI get no discount - there the duplicate
// genuinely costs full price.
static double EstimateFrames(void)
{
    int frames = ChainFrameCountUI();
    int held   = ChainHeldFrames();
    if (held <= 0 || held > frames) return (double)frames;

    bool differenced = (s_format == EXP_FMT_GIF || s_format == EXP_FMT_MP4 ||
                        s_format == EXP_FMT_WEBM);
    if (!differenced) return (double)frames;
    return (double)(frames - held) + 0.1 * (double)held;
}

static double EstimateBytes(void)
{
    int w, h; ExportSize(&w, &h);
    double frames = EstimateFrames();
    Vector2 game = ScreenStateTargetSize();
    double basePx = (double)game.x * (double)game.y * frames;
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
            return (double)w * (double)h * frames * 3.0;
        case EXP_FMT_MP4:
        case EXP_FMT_WEBM:
        {
            // CRF moves the size a little under 2x per 6 points, the usual
            // rule of thumb; the rest is the fitted content-independent floor.
            double crf = (double)VideoCRF();
            double q = pow(0.89, crf - 23.0);
            double bytes = 0.01146 * (double)game.x * (double)game.y *
                           pow(frames, 0.55) * pow(sc, 0.6) * q;
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
    // A chain has no single source name to borrow, and joining all of them
    // would blow past s_nameBuf at four links - so the first name plus a count.
    if (s_chainCount > 1)
        TextCopy(s_nameBuf, TextFormat("%s_chain%d", s_chain[0].name,
                                       s_chainCount));
    else if (s_chainCount == 1)
        TextCopy(s_nameBuf, s_chain[0].name);
    else
    {
        // an unsaved doc has no file to borrow a name from.
        const char *nm = (zen.animCurrent >= 0 && zen.animCurrent < zen.animCount)
                       ? zen.animList[zen.animCurrent] : "animation";
        TextCopy(s_nameBuf, nm);
    }
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
    // Freeze the chain BEFORE anything is allocated: a link whose .cfg has gone
    // missing fails here, with no encoder open and no partial file on disk.
    // s_total and SegmentAt() then read the same snapshot for the whole job,
    // even if a document is edited or the chain reordered underneath them.
    if (!ChainBuild())
    { JobFail("a chained animation's .cfg could not be loaded"); return; }
    s_total = ChainFrameCount();
    s_frame = 0;
    s_bytes = 0;

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
        msf_gif_alpha_threshold = s_transparent ? s_gifCutout : 0;
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

// Undo the premultiply the separate-blend export path leaves behind, so the
// encoders get straight alpha. Nothing to do at either end of the range: a == 0
// has no colour to recover, a == 255 was never scaled.
static void UnpremultiplyRGBA(Image *img)
{
    unsigned char *p = (unsigned char *)img->data;
    int n = img->width * img->height;
    for (int i = 0; i < n; i++, p += 4)
    {
        int a = p[3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; c++)
        {
            int v = (p[c] * 255 + a / 2) / a;
            p[c] = (unsigned char)(v > 255 ? 255 : v);
        }
    }
}

// ---------------------------------------------------------------------------
//  Render one frame and hand it to the active encoder.
// ---------------------------------------------------------------------------
static bool JobStep(void)
{
    int w, h; ExportSize(&w, &h);
    float t;
    const ExpSegment *seg = SegmentAt(s_frame, &t);

    // Only GIF and PNG can carry alpha; the video encoders would bake a
    // transparent clear into black, so they always get the real background.
    bool alpha = s_transparent && (s_format == EXP_FMT_GIF ||
                                   s_format == EXP_FMT_PNG);
    BeginTextureMode(s_rt);
        ClearBackground(alpha ? BLANK : ScreenStateGet()->clear_color);
        if (alpha)
        {
            // The default BLEND_ALPHA multiplies the ALPHA channel by src.a as
            // well, so drawing onto a BLANK target SQUARES it - a 50% element
            // lands at 25% and the cutout below then erases it entirely. That
            // is what used to swallow anything mid-fade (a crumbling text is
            // partly transparent for its whole run, so it vanished outright).
            // Alpha wants a straight "over"; rgb comes out premultiplied and is
            // undone on readback.
            rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA,
                                      RL_ONE,       RL_ONE_MINUS_SRC_ALPHA,
                                      RL_FUNC_ADD,  RL_FUNC_ADD);
            BeginBlendMode(BLEND_CUSTOM_SEPARATE);
        }
        AnimDocDrawLoop(seg->doc, t, NULL, !s_showIntro);
        if (alpha) EndBlendMode();
    EndTextureMode();

    Image img = LoadImageFromTexture(s_rt.texture);
    if (!img.data) return false;
    // render textures are bottom-left origin (the same reason screen_state.c
    // loads its target with a negative height).
    ImageFlipVertical(&img);
    if (s_scale > 1) ImageResizeNN(&img, w, h);
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    // after the format conversion: the helper indexes raw RGBA8 bytes.
    if (alpha) UnpremultiplyRGBA(&img);

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
    s_addDrop = false;
    s_addSearch[0] = '\0';
    s_addSearchEdit = false;
    s_addScroll = 0.0f;
    s_edName = false;
    s_edFps = false;
    s_status[0] = '\0';
    s_state = EXP_IDLE;

    // Drop links whose .cfg is gone (deleted or renamed since the chain was
    // built - possibly in an earlier session). A rename is NOT followed: the
    // registry has no identity beyond the name, so a renamed animation leaves
    // the chain and has to be re-added deliberately.
    int keep = 0;
    for (int k = 0; k < s_chainCount; k++)
        if (ZenAnimFind(s_chain[k].name) >= 0) s_chain[keep++] = s_chain[k];
    s_chainCount = keep;

    // A chain the user built survives closing and reopening the modal. Only an
    // empty one is seeded, and only from a SAVED animation - an unsaved
    // document has no .cfg to chain, so the chain stays empty and canExport
    // says why.
    if (s_chainCount == 0 && zen.animCurrent >= 0 &&
        zen.animCurrent < zen.animCount)
    {
        TextCopy(s_chain[0].name, zen.animList[zen.animCurrent]);
        s_chainCount = 1;
    }
    ChainRefreshInfo();

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

// Where the save-before-export prompt lands after Save or Discard. Every link
// is read from disk, so the prompt has to resolve before the job may start -
// otherwise the one entry that could differ from its file exports stale.
void ZenExportStartAfterSave(void)
{
    if (!s_open) return;
    ChainRefreshInfo();     // the save may have changed the current anim's file
    JobStart();
}
bool ZenExportTyping(void)
{
    return s_open && (s_edName || s_edFps || s_addSearchEdit);
}
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
    if (s_addDrop) { s_addDrop = false; s_addSearchEdit = false; return true; }
    if (s_fmtDrop) { s_fmtDrop = false; return true; }
    s_open = false; s_edName = false; s_edFps = false; s_addSearchEdit = false;
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
    // The chain block is the one section whose height varies with what the user
    // built rather than with a format choice, which is why it is drawn first:
    // everything below it then shifts as a block.
    float chainH = 18 + 2                                         // header label
                 + (float)s_chainCount*(EXP_CHAIN_ROW_H + 2)      // one per link
                 + EXP_ADD_H + EXP_GAP;                           // the add plate
    float bodyH = EXP_TITLE_H + 8
                + chainH
                + EXP_ROW_H + EXP_GAP        // format
                + EXP_ROW_H + EXP_GAP        // name
                + 18 + EXP_GAP               // resolved path
                + EXP_ROW_H + EXP_GAP        // scale
                + EXP_ROW_H + EXP_GAP        // fps
                + EXP_ROW_H + EXP_GAP        // loop range
                + EXP_ROW_H + EXP_GAP        // pause hold
                + 18 + EXP_GAP;              // frames/duration line
    // the join checkbox exists only once there IS a join. A single-animation
    // export has none, so a greyed row would be noise in an already dense modal.
    if (s_chainCount > 1) bodyH += EXP_ROW_H + EXP_GAP;
    // quality + alpha, plus the cutout row that only shows when alpha is on
    if (s_format == EXP_FMT_GIF)
        bodyH += (2 + (s_transparent ? 1 : 0)) * (EXP_ROW_H + EXP_GAP);
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
    // desync the frames already handed to the encoder. The chain is frozen for
    // the same reason - s_total was derived from it.
    if (running) GuiDisable();

    // -- the chain ------------------------------------------------------------
    GuiLabel((Rectangle){ x, cy, wIn, 18 }, "Exporting animations:");
    cy += 18 + 2;

    // Mutations are recorded here and applied AFTER the loop: reshaping the
    // array while iterating it is how a row ends up drawn twice or skipped.
    int wantRemove = -1, wantMove = 0, moveIdx = -1;

    for (int k = 0; k < s_chainCount; k++)
    {
        Rectangle rr = { x, cy, wIn, EXP_CHAIN_ROW_H };
        bool ok = s_info[k].ok;

        GuiLabel((Rectangle){ rr.x, rr.y, 18, rr.height },
                 TextFormat("%d.", k + 1));
        ZenLabelTip((Rectangle){ rr.x + 22, rr.y, 176, rr.height },
                    s_chain[k].name,
                    ok ? TextFormat("anims/%s.cfg - played as step %d of %d",
                                    s_chain[k].name, k + 1, s_chainCount)
                       : "This animation's file no longer exists. Remove it "
                         "from the chain, or re-add it under its new name.");

        if (ok)
            GuiLabel((Rectangle){ rr.x + 202, rr.y, 108, rr.height },
                     TextFormat("%.2fs  %d f", ChainPlayLen(k),
                                ChainBaseFrames(k)));
        else
            GuiLabel((Rectangle){ rr.x + 202, rr.y, 108, rr.height }, "(missing)");

        // ZenButton, not GuiButton: these rows move and vanish under a held
        // cursor, which is exactly the release-fires-on-whatever-slid-here bug
        // it guards against.
        if (k == 0) GuiDisable();
        if (ZenButton((Rectangle){ rr.x + wIn - 78, rr.y, 22, rr.height }, "#121#"))
        { AudioPlayButton(); moveIdx = k; wantMove = -1; }
        if (k == 0 && !running) GuiEnable();
        ZenTip((Rectangle){ rr.x + wIn - 78, rr.y, 22, rr.height },
               "Play this one earlier");

        if (k == s_chainCount - 1) GuiDisable();
        if (ZenButton((Rectangle){ rr.x + wIn - 54, rr.y, 22, rr.height }, "#120#"))
        { AudioPlayButton(); moveIdx = k; wantMove = +1; }
        if (k == s_chainCount - 1 && !running) GuiEnable();
        ZenTip((Rectangle){ rr.x + wIn - 54, rr.y, 22, rr.height },
               "Play this one later");

        // There must always be something to export, so the last link stays.
        if (s_chainCount <= 1) GuiDisable();
        if (ZenButton((Rectangle){ rr.x + wIn - 26, rr.y, 22, rr.height }, "x"))
        { AudioPlayButton(); wantRemove = k; }
        if (s_chainCount <= 1 && !running) GuiEnable();
        ZenTip((Rectangle){ rr.x + wIn - 26, rr.y, 22, rr.height },
               s_chainCount > 1 ? "Remove this animation from the export"
                                : "The export needs at least one animation");

        cy += EXP_CHAIN_ROW_H + 2;
    }

    if (s_chainCount == 0)
    {
        ZenLabelTip((Rectangle){ x, cy, wIn, EXP_CHAIN_ROW_H },
                    "(nothing to export - save this animation first)",
                    "Every link is read from its .cfg, so an animation that has "
                    "never been saved cannot be exported. Save it, then reopen "
                    "this window.");
        cy += EXP_CHAIN_ROW_H + 2;
    }

    // -- "+ Add animation" ----------------------------------------------------
    // Hand-drawn rather than a GuiButton: raygui's default is a flat grey slab
    // that reads as one more setting row, and this is an ACTION. The accent is
    // the {90,140,220} already used for the selected row in the format dropdown
    // and File > Open, so "blue means the thing you can act on" stays one
    // language across the editor.
    {
        Rectangle addR = { x, cy, wIn, EXP_ADD_H };
        bool addFull = (s_chainCount >= EXP_CHAIN_MAX);
        bool addHot  = !addFull && !running && !s_addDrop &&
                       ZenLayerActive(ZEN_LAYER_FLOAT_EXPORT) &&
                       CheckCollisionPointRec(GetMousePosition(), addR);

        Color fill = (addFull || running) ? (Color){ 60, 62, 70, 200 }
                   : addHot               ? (Color){ 90, 140, 220, 90 }
                                          : (Color){ 90, 140, 220, 45 };
        Color line = (addFull || running) ? (Color){ 80, 84, 94, 255 }
                                          : (Color){ 90, 140, 220,
                                                     addHot ? 230 : 150 };
        DrawRectangleRounded(addR, 0.32f, 6, fill);
        DrawRectangleRoundedLines(addR, 0.32f, 6, line);

        const char *addTxt = addFull
            ? TextFormat("chain is full (%d max)", EXP_CHAIN_MAX)
            : "+  Add animation";
        float fs = (float)GuiGetStyle(DEFAULT, TEXT_SIZE);
        float tw = ZenTextW(addTxt);
        DrawTextEx(GuiGetFont(), addTxt,
                   (Vector2){ addR.x + (addR.width - tw)*0.5f,
                              addR.y + (addR.height - fs)*0.5f },
                   fs, (float)GuiGetStyle(DEFAULT, TEXT_SPACING),
                   (addFull || running) ? (Color){ 130, 134, 144, 255 }
                                        : (Color){ 210, 224, 245, 255 });

        // Hand-rolled, so it has to honour the gesture rules GuiButton and
        // ZenButton apply for it: a press that began elsewhere is not a click
        // here, and neither is one on a plate that just reflowed under the
        // cursor.
        if (addHot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !zen.mouseReflow)
        {
            AudioPlayButton();
            s_addDrop = true;
            s_fmtDrop = false;              // never both at once
            s_addSearch[0] = '\0';
            s_addSearchEdit = false;
            s_addScroll = 0.0f;
        }
        s_addDropRect = addR;
        ZenTip(addR, addFull
            ? TextFormat("An export can chain at most %d animations.",
                         EXP_CHAIN_MAX)
            : "Append another saved animation. They render back to back, in "
              "this order, into one file.");
        cy += EXP_ADD_H + EXP_GAP;
    }

    // Applied after the loop, and each one re-reads the summaries the rows and
    // the frame count are drawn from. ZenMouseReflow poisons the gesture in
    // flight, because every row below the change has just moved.
    if (wantMove && moveIdx >= 0)
    {
        int j = moveIdx + wantMove;
        if (j >= 0 && j < s_chainCount)
        {
            ExpChainEntry te = s_chain[moveIdx];
            s_chain[moveIdx] = s_chain[j];
            s_chain[j] = te;
            ExpChainInfo ti = s_info[moveIdx];
            s_info[moveIdx] = s_info[j];
            s_info[j] = ti;
            ZenMouseReflow();
        }
    }
    if (wantRemove >= 0 && s_chainCount > 1)
    {
        for (int k = wantRemove; k < s_chainCount - 1; k++)
        {
            s_chain[k] = s_chain[k + 1];
            s_info[k]  = s_info[k + 1];
        }
        s_chainCount--;
        ZenMouseReflow();
    }

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
    bool hasIntro  = ChainHasIntro();
    bool ownDisable = !hasIntro && !running;
    if (!hasIntro)
        // Force it off while disabled so the range stays truthful: a checked box
        // that cannot be unchecked must not keep steering the export range.
        s_showIntro = false;
    if (ownDisable) GuiDisable();
    bool introWas = s_showIntro;
    GuiCheckBox((Rectangle){ x + lw, cy + 3, 18, 18 }, "Show intro",
                &s_showIntro);
    if (ownDisable) GuiEnable();
    // Every link's range moves with this, and the rows above print those ranges.
    if (s_showIntro != introWas) ChainRefreshInfo();
    ZenTip((Rectangle){ x + lw, cy + 3, 160, 18 },
           hasIntro ? (s_chainCount > 1
                    ? "Include the run-in each animation plays once before it "
                      "settles. Off exports the looping sections only. Applies "
                      "to every animation in the chain; outros are always "
                      "trimmed."
                    : "Include the run-in the animation plays once before it "
                      "settles. Off exports the looping section only, so the "
                      "result loops seamlessly. The outro is always trimmed.")
                    : "No animation in this export has an intro. The looping "
                      "sections are exported; outros are always trimmed.");
    cy += EXP_ROW_H + EXP_GAP;

    // -- pause hold -----------------------------------------------------------
    // Same ownDisable dance as the row above: the panel is already inside the
    // running-job GuiDisable, so this row may only re-enable when IT is the one
    // that disabled.
    //
    // The duration now feeds two things - markers inside a clip, and the beat
    // between two chained clips - so it stays live if EITHER is in play. Killing
    // it on "no markers" alone would make the join checkbox silently do nothing.
    int  nMarks = ChainMarkerCount();
    int  nJoins = ChainJoinCount();
    bool holdUseless = (nMarks == 0 && nJoins == 0);
    bool ownDisableHold = holdUseless && !running;
    if (holdUseless)
        // Nothing to hold on, so the value must not keep inflating the frame
        // count - same reasoning as the intro checkbox above.
        s_pauseHold = 0.0f;
    if (ownDisableHold) GuiDisable();
    GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Pause hold");
    Rectangle phR = { x + lw, cy, wIn - lw - 46, EXP_ROW_H };
    GuiSlider(phR, NULL, TextFormat("%.1fs", s_pauseHold), &s_pauseHold,
              0.0f, 12.0f);
    // 12s across ~280px is ~40ms a pixel, so the raw value lands on unreachable
    // numbers like 3.4297; snapped to a tenth it reads as something chosen.
    s_pauseHold = roundf(s_pauseHold * 10.0f) / 10.0f;
    if (ownDisableHold) GuiEnable();
    ZenTip(phR,
        (nMarks && nJoins)
            ? TextFormat("Each of the %d pause marker%s and each of the %d join%s "
                         "between animations freezes for this long. 0 runs "
                         "straight through them.",
                         nMarks, nMarks == 1 ? "" : "s",
                         nJoins, nJoins == 1 ? "" : "s")
        : nJoins
            ? TextFormat("Freezes for this long at each of the %d join%s between "
                         "the chained animations. 0 cuts straight from one to "
                         "the next.", nJoins, nJoins == 1 ? "" : "s")
        : nMarks
            ? TextFormat("A pause marker waits for a keypress, and a file has "
                         "nobody to press one - so each of this range's %d marker%s "
                         "freezes for this long instead. 0 runs straight through "
                         "them.", nMarks, nMarks == 1 ? "" : "s")
            : "Nothing in this export pauses: no pause markers in range, and no "
              "joins between animations");
    cy += EXP_ROW_H + EXP_GAP;

    // -- pause between animations ---------------------------------------------
    if (s_chainCount > 1)
    {
        bool joinWas = s_joinPause;
        GuiCheckBox((Rectangle){ x + lw, cy + 3, 18, 18 },
                    "Pause between animations", &s_joinPause);
        if (s_joinPause != joinWas) ZenMouseReflow();   // frame count moves
        ZenTip((Rectangle){ x + lw, cy + 3, 200, 18 },
               "Freezes on the last frame of each animation before the next one "
               "starts, for the same length as the Pause hold above. Off cuts "
               "straight from one to the next, which usually reads as a glitch.");
        cy += EXP_ROW_H + EXP_GAP;
    }

    int frames = ChainFrameCountUI();
    int heldFrames = ChainHeldFrames();
    // Duration comes from the FRAME COUNT, not the range, so the held frames
    // are counted - they are real seconds of output.
    GuiLabel((Rectangle){ x, cy, wIn, 18 },
             TextFormat("%d frames  -  %.2fs%s", frames,
                        (float)frames / (float)s_fps,
                        heldFrames ? TextFormat("   (%d held)", heldFrames)
                                   : ""));
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

        if (s_transparent)
        {
            GuiLabel((Rectangle){ x, cy, lw, EXP_ROW_H }, "Cutout");
            float c = (float)s_gifCutout;
            GuiSlider((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H }, NULL,
                      TextFormat("%d", s_gifCutout), &c, 1, 255);
            s_gifCutout = (int)(c + 0.5f);
            ZenTip((Rectangle){ x + lw, cy, wIn - lw - 46, EXP_ROW_H },
                   "How opaque a pixel must be to be kept, since GIF alpha is "
                   "on/off. Lower keeps faint pixels such as fading or "
                   "crumbling text; too low and a semi-transparent full-screen "
                   "fade becomes a solid wall.");
            cy += EXP_ROW_H + EXP_GAP;
        }
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
    // zen.doc.elemCount no longer speaks for the export: the chain may not even
    // contain the animation that is open. Every link has to resolve, and at
    // least one has to have something in it.
    bool chainOk = ChainResolvable() && ChainAnyElems();
    bool canExport = avail && frames > 0 && chainOk;

#if !defined(PLATFORM_WEB)
    // The folder only exists once something has been exported. Greyed rather than
    // created on click - a button that says "open" should not also make things.
    // The tip sits outside the disable pair: while the button is dead, that
    // tooltip is the only thing saying why.
    bool haveDir = DirectoryExists(EXP_DIR);
    if (!haveDir) GuiDisable();
    if (GuiButton((Rectangle){ m.x + 12, by, 86, bh }, "Folder"))
    { AudioPlayButton(); OpenExportDir(); }
    if (!haveDir) GuiEnable();
    ZenTip((Rectangle){ m.x + 12, by, 86, bh },
           haveDir ? "Open the exports folder in the file manager"
                   : "Nothing has been exported yet - the folder is created on "
                     "the first export");
#endif

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
        Rectangle expR = { m.x + EXP_W - 2*86 - 24, by, 86, bh };
        if (!canExport) GuiDisable();
        if (GuiButton(expR, "Export"))
        {
            AudioPlayButton();
            // Every link is read from its .cfg, so an unsaved edit to the open
            // animation would export stale. Ask first rather than quietly
            // shipping the version on disk; the prompt calls back into
            // ZenExportStartAfterSave to run the job.
            if (zen.docDirty && ChainUsesCurrent())
                zen.prompt = ZEN_PROMPT_SAVE_THEN_EXPORT;
            else
                JobStart();
        }
        if (!canExport) GuiEnable();
        // Outside the disable pair: while the button is dead this tip is the
        // only thing saying why.
        ZenTip(expR, canExport ? "Render the chain to the file above"
               : s_chainCount == 0
                   ? "Nothing to export - save this animation first, then "
                     "reopen this window"
               : !ChainResolvable()
                   ? "One of the chained animations no longer exists on disk - "
                     "the row marked (missing)"
               : !ChainAnyElems()
                   ? "Every animation in the chain is empty"
                   : "ffmpeg is needed for this format");
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

    // -- the add-animation picker, drawn last so it lands over everything ------
    if (s_addDrop)
    {
        // Filter first, so the scroll bounds match what is actually shown.
        // Animations already in the chain are NOT excluded: A,B,A is a
        // legitimate sequence, and so is the same clip twice for emphasis.
        int vis[ZEN_ANIM_LIST_MAX], nvis = 0;
        for (int i = 0; i < zen.animCount; i++)
            if (ZenStrContainsCI(zen.animList[i], s_addSearch)) vis[nvis++] = i;

        float ih = 22.0f, sbH = 24.0f;
        float listH = (float)nvis * ih;
        if (listH > 6*ih) listH = 6*ih;         // at most six rows before scroll
        if (listH < ih)   listH = ih;           // room for the "(no match)" line
        float dropH = sbH + 2 + listH;

        Rectangle hdr = s_addDropRect;
        float ly = (hdr.y + hdr.height + dropH <= screen.y - 4.0f)
                 ? hdr.y + hdr.height : hdr.y - dropH;
        if (ly < 4.0f) ly = 4.0f;
        Rectangle bg = { hdr.x, ly, hdr.width, dropH };
        DrawRectangleRec(bg, (Color){ 32, 34, 40, 255 });
        DrawRectangleLinesEx(bg, 1.0f, (Color){ 90, 140, 220, 180 });

        Rectangle sb = { bg.x + 4, bg.y + 2, bg.width - 8, sbH - 4 };
        if (GuiTextBox(sb, s_addSearch, ANIM_NAME_MAX, s_addSearchEdit))
            s_addSearchEdit = !s_addSearchEdit;
        if (!s_addSearch[0] && !s_addSearchEdit)
            GuiLabel((Rectangle){ sb.x + 8, sb.y, sb.width - 16, sb.height },
                     "search...");

        Rectangle list = { bg.x, bg.y + sbH + 2, bg.width, listH };
        if (CheckCollisionPointRec(GetMousePosition(), list))
            s_addScroll += GetMouseWheelMove() * ih;
        float maxScroll = (float)nvis*ih - list.height;
        if (maxScroll < 0) maxScroll = 0;
        if (s_addScroll < -maxScroll) s_addScroll = -maxScroll;
        if (s_addScroll > 0) s_addScroll = 0;

        int pick = -1;
        BeginScissorMode((int)list.x, (int)list.y,
                         (int)list.width, (int)list.height);
        float ry = list.y + s_addScroll;
        for (int v = 0; v < nvis; v++)
        {
            Rectangle rr = { list.x, ry, list.width, ih };
            if (GuiButton(rr, zen.animList[vis[v]])) pick = vis[v];
            if (vis[v] == zen.animCurrent)
                DrawRectangleRec(rr, (Color){ 90, 140, 220, 60 });
            ry += ih;
        }
        EndScissorMode();
        if (nvis == 0)
            GuiLabel((Rectangle){ list.x + 6, list.y + 2, list.width - 12, 18 },
                     "(no match)");

        // Enter takes the first match, the same as File > Open's list.
        if (pick < 0 && s_addSearchEdit && nvis > 0 &&
            (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)))
            pick = vis[0];

        if (pick >= 0 && s_chainCount < EXP_CHAIN_MAX)
        {
            AudioPlayButton();
            TextCopy(s_chain[s_chainCount].name, zen.animList[pick]);
            s_chainCount++;
            ChainRefreshInfo();
            s_addDrop = false;
            s_addSearchEdit = false;
            // The modal just grew by a row (and possibly the join checkbox), so
            // everything below the list has moved out from under the cursor.
            ZenMouseReflow();
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            !CheckCollisionPointRec(GetMousePosition(), bg) &&
            !CheckCollisionPointRec(GetMousePosition(), hdr))
        { s_addDrop = false; s_addSearchEdit = false; }
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
