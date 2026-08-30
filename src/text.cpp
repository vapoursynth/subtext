/*
 * AssVapour
 *
 * Copyright (c) 2012, Martin Herkt <lachs0r@srsfckn.biz>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include <ass/ass.h>

#include <VapourSynth4.h>
#include <VSConstants4.h>
#include <VSHelper4.h>

#include "common.h"

#define _r(c) ((c) >> 24)
#define _g(c) (((c) >> 16) & 0xFF)
#define _b(c) (((c) >> 8) & 0xFF)
#define _a(c) (255 - ((c) & 0xFF))

#define div256(x) (((x + 128) >> 8))
#define div255(x) ((div256(x + div256(x))))

#define blend(srcA, srcRGB, dstA, dstRGB, outA)  \
    ((srcA * 255 * srcRGB + (dstRGB * dstA * (255 - srcA))) / outA)


/* Only the things rendering a frame needs. Everything the arguments are turned
   into is used while the filter is being created and nothing more. The order
   the members are declared in is the order they have to be destroyed in: the
   track belongs to the library, and the renderer uses both. */
struct AssData {
    VSNode *node = nullptr;
    VSVideoInfo vi[2] = { };

    int lastn = -1;
    VSFrame *lastframe = nullptr;

    bool adapt_sar = false;
    bool own_track = false;

    AssLibraryPtr ass_library;
    AssRendererPtr ass_renderer;
    AssTrackPtr ass;
};


static std::string replaceAll(const std::string &in, const std::string &from, const std::string &to)
{
    if(from.empty())
        return in;

    std::string out;
    size_t position = 0;

    for(size_t match; (match = in.find(from, position)) != std::string::npos; position = match + from.size()) {
        out.append(in, position, match - position);
        out.append(to);
    }

    out.append(in, position, std::string::npos);

    return out;
}

static void assDebugCallback(int level, const char *fmt, va_list va, void *data)
{
    if(level < (intptr_t)data) {
        fprintf(stderr, "libass: ");
        vfprintf(stderr, fmt, va);
        fprintf(stderr, "\n");
    }
}

static void assRender(VSFrame *dst, VSFrame *alpha, const VSAPI *vsapi,
                      ASS_Image *img)
{
    uint8_t *planes[4];
    ptrdiff_t strides[4], p;

    for(p = 0; p < 4; p++) {
        VSFrame *fr = p == 3 ? alpha : dst;

        planes[p] = vsapi->getWritePtr(fr, p % 3);
        strides[p] = vsapi->getStride(fr, p % 3);
        memset(planes[p], 0, strides[p] * vsapi->getFrameHeight(fr, p % 3));
    }

    while(img) {
        uint8_t *dstp[4], *alphap, *sp, color[4];
        uint16_t outa;
        int x, y, k;

        if(img->w == 0 || img->h == 0) {
            img = img->next;
            continue;
        }

        color[0] = _r(img->color);
        color[1] = _g(img->color);
        color[2] = _b(img->color);
        color[3] = _a(img->color);

        dstp[0] = planes[0] + (strides[0] * img->dst_y) + img->dst_x;
        dstp[1] = planes[1] + (strides[1] * img->dst_y) + img->dst_x;
        dstp[2] = planes[2] + (strides[2] * img->dst_y) + img->dst_x;
        dstp[3] = planes[3] + (strides[3] * img->dst_y) + img->dst_x;
        alphap = dstp[3];
        sp = img->bitmap;

        for(y = 0; y < img->h; y++) {
            for(x = 0; x < img->w; x++) {
                k = div255(sp[x] * color[3]);
                outa = k * 255 + (alphap[x] * (255 - k));

                if(outa != 0) {
                    dstp[0][x] = blend(k, color[0], alphap[x], dstp[0][x], outa);
                    dstp[1][x] = blend(k, color[1], alphap[x], dstp[1][x], outa);
                    dstp[2][x] = blend(k, color[2], alphap[x], dstp[2][x], outa);
                    dstp[3][x] = div255(outa);
                }
            }

            dstp[0] += strides[0];
            dstp[1] += strides[1];
            dstp[2] += strides[2];
            dstp[3] += strides[3];
            alphap += strides[3];
            sp += img->stride;
        }

        img = img->next;
    }
}

/* Reads the sample aspect ratio out of the frame properties of the clip.
   Square pixels unless the frame says otherwise. */
static double frameToSampleAspect(const VSFrame *src, const VSAPI *vsapi)
{
    const VSMap *props = vsapi->getFramePropertiesRO(src);
    int num_err, den_err;

    int64_t sar_num = vsapi->mapGetInt(props, "_SARNum", 0, &num_err);
    int64_t sar_den = vsapi->mapGetInt(props, "_SARDen", 0, &den_err);

    if(num_err || den_err || sar_num <= 0 || sar_den <= 0)
        return 1;

    return (double)sar_num / (double)sar_den;
}

/* Tells libass what the subtitles are laid out for in square pixel
   resolution: the width of the clip scaled by the SAR at the height the clip
   already has.

   Only width moves because libass divides width by display scale for blur. */
static void setLayoutSize(AssData *d, double sar)
{
    int layout_x = (int)std::lround(d->vi[0].width * sar);

    if(layout_x <= 0)
        layout_x = d->vi[0].width;

    ass_set_storage_size(d->ass_renderer.get(), layout_x, d->vi[0].height);

    /* PlayRes is the coordinate space the subtitles are written in, and libass
       measures drawings, letter spacing and borders against it, so it has to
       describe the same display shape the layout resolution does or only the
       glyphs come out compensated. The vertical half is left alone because
       font sizes are relative to it; only PlayResX moves. Tracks read from a
       subtitle file say what they were written for and are never touched. */
    if(d->own_track && d->ass && d->ass->PlayResY > 0) {
        int play_x = (int)std::lround((double)d->ass->PlayResY *
                                      layout_x / d->vi[0].height);

        if(play_x > 0 && play_x != d->ass->PlayResX) {
            /* Style margins are measured in the same units, so they have to
               follow PlayResX to keep covering the same part of the frame. */
            if(d->ass->PlayResX > 0) {
                double fix = (double)play_x / d->ass->PlayResX;

                for(int i = 0; i < d->ass->n_styles; i++) {
                    d->ass->styles[i].MarginL =
                        (int)std::lround(d->ass->styles[i].MarginL * fix);
                    d->ass->styles[i].MarginR =
                        (int)std::lround(d->ass->styles[i].MarginR * fix);
                }
            }

            d->ass->PlayResX = play_x;
        }
    }
}

static const VSFrame *VS_CC assGetFrame(int n, int activationReason,
        void *instanceData, [[maybe_unused]] void **frameData,
        VSFrameContext *frameCtx, VSCore *core,
        const VSAPI *vsapi)
{
    AssData *d = (AssData *) instanceData;

    /* Without a frame of the clip there is nothing to wait for, so the frame
       is produced right away in the arInitial call. */
    if(d->adapt_sar) {
        if(activationReason == arInitial) {
            vsapi->requestFrameFilter(n, d->node, frameCtx);

            return NULL;
        }

        if(activationReason != arAllFramesReady)
            return NULL;
    }

    if(n != d->lastn) {
        ASS_Image *img;
        int64_t ts = 0;
        int changed;

        if(d->adapt_sar) {
            const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
            double sar = frameToSampleAspect(src, vsapi);
            vsapi->freeFrame(src);

            /* Setting the same value again costs nothing, the layout only
               changes when the aspect ratio does. A change empties the libass
               caches, which makes the rendered images differ, so the change
               detection below picks it up. */
            setLayoutSize(d, sar);
        }

        ts = (int64_t)n * 1000 * d->vi[0].fpsDen / d->vi[0].fpsNum;

        img = ass_render_frame(d->ass_renderer.get(), d->ass.get(), ts, &changed);

        if (changed) {
            VSFrame *dst = vsapi->newVideoFrame(&d->vi[0].format,
                                                   d->vi[0].width,
                                                   d->vi[0].height,
                                                   NULL, core);

            VSFrame *a = vsapi->newVideoFrame(&d->vi[1].format,
                                                 d->vi[1].width,
                                                 d->vi[1].height,
                                                 NULL, core);

            assRender(dst, a, vsapi, img);
            vsapi->mapSetInt(vsapi->getFramePropertiesRW(a), "_Range", VSC_RANGE_FULL, maReplace);
            vsapi->mapConsumeFrame(vsapi->getFramePropertiesRW(dst), "_Alpha", a, maAppend);
            vsapi->freeFrame(d->lastframe);
            d->lastframe = dst;
        }

        d->lastn = n;
    }

    return vsapi->addFrameRef(d->lastframe);
}

static void VS_CC assFree(void *instanceData, [[maybe_unused]] VSCore *core, const VSAPI *vsapi)
{
    AssData *d = (AssData *)instanceData;
    vsapi->freeNode(d->node);
    vsapi->freeFrame(d->lastframe);
    delete d;
}

static std::optional<std::string> frameToTime(int frame, int64_t fpsNum, int64_t fpsDen)
{
    if(frame < 0 || fpsNum <= 0 || fpsDen <= 0)
        return {};

    if(frame != 0 && (INT64_MAX / fpsDen) < ((int64_t)frame * 100))
        return {}; // Overflow would occur

    int64_t timeint = (int64_t)frame * 100 * fpsDen / fpsNum;

    // Not using gmtime(), which wraps around after 24 hours and can fail outright.
    int64_t centiseconds = timeint % 100;
    int64_t seconds = timeint / 100;
    int64_t minutes = seconds / 60;
    int64_t hours = minutes / 60;

    char str[64];
    snprintf(str, sizeof(str), "%" PRId64 ":%02" PRId64 ":%02" PRId64 ".%02" PRId64,
             hours, minutes % 60, seconds % 60, centiseconds);

    return std::string(str);
}


static void VS_CC assRenderCreate(const VSMap *in, VSMap *out, void *userData,
                                  VSCore *core, const VSAPI *vsapi)
{
    AssData d;
    int err;

    const std::string filter_name = (const char *)userData;

    // Only the first problem found is reported.
    std::string error;
    auto setError = [&](const std::string &message) {
        if(error.empty())
            error = filter_name + ": " + message;
    };
    auto fail = [&](const std::string &message) {
        vsapi->mapSetError(out, (filter_name + ": " + message).c_str());
        vsapi->freeNode(d.node);
    };

    d.node = vsapi->mapGetNode(in, "clip", 0, 0);
    d.vi[0] = *vsapi->getVideoInfo(d.node);

    if (d.vi[0].format.colorFamily == cfUndefined || !d.vi[0].width || !d.vi[0].height)
        setError("clip must have known format and dimensions.");

    if (!d.vi[0].fpsNum || !d.vi[0].fpsDen)
        setError("clip must have a known constant frame rate.");

    vsapi->getVideoFormatByID(&d.vi[0].format, pfRGB24, core);
    d.vi[1] = d.vi[0];
    vsapi->getVideoFormatByID(&d.vi[1].format, pfGray8, core);

    std::optional<std::string> file;
    std::string text;
    int startframe = 0;
    int endframe = 0;

    if(const char *file_arg = vsapi->mapGetData(in, "file", 0, &err); !err) {
        file = file_arg;
    } else {
        text = vsapi->mapGetData(in, "text", 0, &err);

        startframe = vsapi->mapGetIntSaturated(in, "start", 0, &err);
        if (err) {
            startframe = 0;
        }
        else if(startframe < 0) {
            setError("start must not be negative");
        }
        else if(startframe >= d.vi[0].numFrames) {
            setError("start must be smaller than the clip length");
        }


        endframe = vsapi->mapGetIntSaturated(in, "end", 0, &err);
        if(err) {
            endframe = d.vi[0].numFrames;
        }
        else if(endframe > d.vi[0].numFrames) {
            setError("end must be smaller than the clip length");
        }
        else if(!(startframe < endframe)) {
            setError("end must be larger than start");
        }
    }

    std::optional<std::string> style;

    if(const char *style_arg = vsapi->mapGetData(in, "style", 0, &err); !err)
        style = style_arg;
    else if(!file)
        style = "sans-serif,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1";

    const char *charset_arg = vsapi->mapGetData(in, "charset", 0, &err);
    const std::string charset = err ? "UTF-8" : charset_arg;

    const char *fontdir = vsapi->mapGetData(in, "fontdir", 0, &err);
    if(err)
        fontdir = nullptr;

    double scale = vsapi->mapGetFloat(in, "scale", 0, &err);

    if(err)
        scale = 1;

    if(scale <= 0)
        setError("scale must be greater than 0");

    double linespacing = vsapi->mapGetFloat(in, "linespacing", 0, &err);

    if(linespacing < 0)
        setError("linespacing must be positive");

    /* Zero, which is also what an absent argument gives, means the pixel
       aspect ratio of each frame of the clip is used instead. */
    double sar = vsapi->mapGetFloat(in, "sar", 0, &err);

    if(sar < 0)
        setError("sar must be positive");

    d.adapt_sar = !sar;

    int num_margins = vsapi->mapNumElements(in, "margins");

    if(num_margins > -1 && num_margins != 4)
        setError("margins must have exactly 4 elements");

    int margins[4] = { };

    for(int i = 0; i < 4; i++) {
        margins[i] = vsapi->mapGetIntSaturated(in, "margins", i, &err);

        if(err) {
            margins[i] = 0;
        } else if(margins[i] < 0) {
            setError("margins must be positive");
            break;
        }
    }

    intptr_t debuglevel = (intptr_t)vsapi->mapGetInt(in, "debuglevel", 0, &err);

    if(!error.empty()) {
        vsapi->mapSetError(out, error.c_str());
        vsapi->freeNode(d.node);
        return;
    }

    d.ass_library.reset(ass_library_init());

    if(!d.ass_library) {
        fail("failed to initialize ASS library");
        return;
    }

    ass_set_message_cb(d.ass_library.get(), assDebugCallback, (void *)debuglevel);
    ass_set_extract_fonts(d.ass_library.get(), 0);
    ass_set_style_overrides(d.ass_library.get(), 0);

    d.ass_renderer.reset(ass_renderer_init(d.ass_library.get()));

    if(!d.ass_renderer) {
        fail("failed to initialize ASS renderer");
        return;
    }

    ass_set_font_scale(d.ass_renderer.get(), scale);
    ass_set_frame_size(d.ass_renderer.get(), d.vi[0].width, d.vi[0].height);
    ass_set_margins(d.ass_renderer.get(),
                    margins[0], margins[1], margins[2], margins[3]);
    ass_set_use_margins(d.ass_renderer.get(), 0);

    if(linespacing)
        ass_set_line_spacing(d.ass_renderer.get(), linespacing);

    if(fontdir)
        ass_set_fonts_dir(d.ass_library.get(), fontdir);

    ass_set_fonts(d.ass_renderer.get(), NULL, NULL, 1, NULL, 1);

    if(!file) {
        std::optional<std::string> start = frameToTime(startframe, d.vi[0].fpsNum, d.vi[0].fpsDen);
        std::optional<std::string> end = frameToTime(endframe, d.vi[0].fpsNum, d.vi[0].fpsDen);

        if (!start || !end) {
            fail(std::string("Unable to calculate ") + (start ? "end" : "start") + " time");
            return;
        }

        // Normalize CRLF and lone CR line endings first so no stray \r
        // ends up in the dialogue line.
        std::string dialogue = replaceAll(replaceAll(replaceAll(text, "\r\n", "\n"), "\r", "\n"), "\n", "\\N");

        std::string script =
            "[Script Info]\n"
            "ScriptType: v4.00+\n"
            "PlayResX: " + std::to_string(d.vi[0].width) + "\n"
            "PlayResY: " + std::to_string(d.vi[0].height) + "\n"
            "ScaledBorderAndShadow: yes\n"
            "[V4+ Styles]\n"
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
            "Style: Default," + *style + "\n"
            "[Events]\n"
            "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
            "Dialogue: 0," + *start + "," + *end + ",Default,,0,0,0,," + dialogue + "\n";

        d.ass.reset(ass_new_track(d.ass_library.get()));

        if (!d.ass) {
            fail("failed to create ASS track");
            return;
        }

        ass_process_data(d.ass.get(), script.data(), (int)script.size());

        d.own_track = true;
    } else {
        std::string conversion_error;
        std::optional<std::string> contents = convertToUtf8(*file, charset, conversion_error);

        if (contents) {
            d.ass.reset(ass_read_memory(d.ass_library.get(), contents->data(), contents->size(), NULL));

            if (!d.ass) {
                d.ass.reset(convertToASS(*file, *contents, d.ass_library.get(), style ? style->c_str() : nullptr, charset, conversion_error));

                /* libavcodec's conversion, not a script anybody authored. */
                d.own_track = true;
            }
        }

        if (!d.ass) {
            fail(conversion_error);
            return;
        }
    }

    /* Subtitles read from a file were written for the video the way it is
       stored, which is how every other renderer treats them, so the aspect of
       the clip is only followed for the tracks made here. Passing sar asks for
       it anyway. A track carrying LayoutRes headers says what it's laid out
       for and wins over both, no rendered frame needed. */
    if(!d.own_track || (d.ass->LayoutResX > 0 && d.ass->LayoutResY > 0))
        d.adapt_sar = false;

    setLayoutSize(&d, sar ? sar : 1);

    VSFrame *frame = vsapi->newVideoFrame(&d.vi[0].format,
                                       d.vi[0].width,
                                       d.vi[0].height,
                                       NULL, core);

    VSFrame *alpha = vsapi->newVideoFrame(&d.vi[1].format,
                                       d.vi[1].width,
                                       d.vi[1].height,
                                       NULL, core);

    for (int plane = 0; plane < d.vi[0].format.numPlanes; plane++)
        memset(vsapi->getWritePtr(frame, plane), 0, vsapi->getStride(frame, plane) * vsapi->getFrameHeight(frame, plane));
    memset(vsapi->getWritePtr(alpha, 0), 0, vsapi->getStride(alpha, 0) *vsapi->getFrameHeight(alpha, 0));

    vsapi->mapSetInt(vsapi->getFramePropertiesRW(alpha), "_Range", VSC_RANGE_FULL, maReplace);

    vsapi->mapConsumeFrame(vsapi->getFramePropertiesRW(frame), "_Alpha", alpha, maAppend);

    d.lastframe = frame;

    VSNode *node = d.node;
    AssData *data = new AssData(std::move(d));

    /* A frame of the clip is only ever needed to look up its pixel aspect
       ratio, so with sar given nothing is requested from it at all. */
    VSFilterDependency deps[] = {{ node, rpStrictSpatial }};

    vsapi->createVideoFilter(out, filter_name.c_str(), data->vi, assGetFrame, assFree,
                        fmUnordered, data->adapt_sar ? deps : nullptr, data->adapt_sar ? 1 : 0, data, core);

    if (vsapi->mapGetError(out)) {
        assFree(data, core, vsapi);
        return;
    }

    int blend = !!vsapi->mapGetInt(in, "blend", 0, &err);
    if (err)
        blend = 1;

    if (blend) {
        VSNode *subs = vsapi->mapGetNode(out, "clip", 0, NULL);

        blendSubtitles(node, subs, in, out, filter_name, core, vsapi);

        vsapi->freeNode(subs);
    }
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("biz.srsfckn.subtext", "sub",
        "A subtitling filter based on libass and FFmpeg.",
        VS_MAKE_VERSION(7, 0), VAPOURSYNTH_API_VERSION, 0, plugin);

#define COMMON_TEXTFILE_PARAMS \
    "debuglevel:int:opt;" \
    "fontdir:data:opt;" \
    "linespacing:float:opt;" \
    "margins:int[]:opt;" \
    "sar:float:opt;" \
    "style:data:opt;"

#define COMMON_PARAMS \
    "blend:int:opt;" \
    "matrix:int:opt;" \
    "matrix_s:data:opt;" \
    "transfer:int:opt;" \
    "transfer_s:data:opt;" \
    "primaries:int:opt;" \
    "primaries_s:data:opt;"

    vspapi->registerFunction("TextFile",
                 "clip:vnode;"
                 "file:data;"
                 "charset:data:opt;"
                 "scale:float:opt;"
                 COMMON_TEXTFILE_PARAMS
                 COMMON_PARAMS
                 , "clip:vnode;", assRenderCreate, (void *)"TextFile", plugin);
    vspapi->registerFunction("Subtitle",
                 "clip:vnode;"
                 "text:data;"
                 "start:int:opt;"
                 "end:int:opt;"
                 COMMON_TEXTFILE_PARAMS
                 COMMON_PARAMS
                 , "clip:vnode;", assRenderCreate, (void *)"Subtitle", plugin);
    vspapi->registerFunction("ImageFile",
                 "clip:vnode;"
                 "file:data;"
                 "id:int:opt;"
                 "palette:int[]:opt;"
                 "gray:int:opt;"
                 "info:int:opt;"
                 "flatten:int:opt;"
                 COMMON_PARAMS
                 , "clip:vnode;", imageFileCreate, 0, plugin);

#undef COMMON_PARAMS
#undef COMMON_TEXTFILE_PARAMS
}

