#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <VapourSynth4.h>
#include <VSConstants4.h>
#include <VSHelper4.h>

#include "common.h"


/* The colour space the subtitle bitmaps produced by libass are assumed to be
   in. Only used as the starting point when converting them into the transfer
   function and primaries of the video. */
#define SUBTITLE_TRANSFER_S "709"
#define SUBTITLE_PRIMARIES_S "709"

#define NUM_CHROMA_LOCATIONS (VSC_CHROMA_BOTTOM + 1)

/* This property isn't adapted to. */
#define NOT_ADAPTED (-1)


/* Only the values that zimg can always convert to are used. The rest are
   replaced by the default, which keeps a strangely tagged clip rendering
   instead of failing once the conversion is attempted. Matrices that need a
   fully specified colour space to be usable, such as the constant luminance
   ones, are deliberately left out because the tags needed to make them work
   aren't guaranteed to be there. */
static bool isSupportedMatrix(int matrix) {
    switch (matrix) {
        case VSC_MATRIX_BT709:
        case VSC_MATRIX_FCC:
        case VSC_MATRIX_BT470_BG:
        case VSC_MATRIX_ST170_M:
        case VSC_MATRIX_ST240_M:
        case VSC_MATRIX_YCGCO:
        case VSC_MATRIX_BT2020_NCL:
            return true;
        default:
            return false;
    }
}


static bool isSupportedTransfer(int transfer) {
    switch (transfer) {
        case VSC_TRANSFER_BT709:
        case VSC_TRANSFER_BT470_M:
        case VSC_TRANSFER_BT470_BG:
        case VSC_TRANSFER_BT601:
        case VSC_TRANSFER_ST240_M:
        case VSC_TRANSFER_LINEAR:
        case VSC_TRANSFER_LOG_100:
        case VSC_TRANSFER_LOG_316:
        case VSC_TRANSFER_IEC_61966_2_4:
        case VSC_TRANSFER_IEC_61966_2_1:
        case VSC_TRANSFER_BT2020_10:
        case VSC_TRANSFER_BT2020_12:
        case VSC_TRANSFER_ST2084:
        case VSC_TRANSFER_ST428:
        case VSC_TRANSFER_ARIB_B67:
            return true;
        default:
            return false;
    }
}


static bool isSupportedPrimaries(int primaries) {
    switch (primaries) {
        case VSC_PRIMARIES_BT709:
        case VSC_PRIMARIES_BT470_M:
        case VSC_PRIMARIES_BT470_BG:
        case VSC_PRIMARIES_ST170_M:
        case VSC_PRIMARIES_ST240_M:
        case VSC_PRIMARIES_FILM:
        case VSC_PRIMARIES_BT2020:
        case VSC_PRIMARIES_ST428:
        case VSC_PRIMARIES_ST431_2:
        case VSC_PRIMARIES_ST432_1:
        case VSC_PRIMARIES_EBU3213_E:
            return true;
        default:
            return false;
    }
}


/* MaskedMerge refuses to blend frames that don't have the same colour range,
   and blending only gives the right result when the subtitles were converted
   using the same matrix, transfer function, primaries and chroma location as
   the video. All of those are frame properties, so the conversion can only be
   decided once the frame is known. There are far too many combinations to
   prepare all of them in advance, so a converted clip is created the first
   time a combination is seen and kept for the frames that follow. In practice
   a clip is tagged the same way all the way through and only one is ever
   created. */
struct ConversionKey {
    int matrix;
    int transfer;
    int primaries;
    int range;
    int chroma_location;

    bool operator==(const ConversionKey &other) const {
        return matrix == other.matrix &&
               transfer == other.transfer &&
               primaries == other.primaries &&
               range == other.range &&
               chroma_location == other.chroma_location;
    }
};


struct MatchColorimetryData {
    VSNode *prop_src;

    /* The subtitles, the target format, the target dimensions and whatever
       conversion arguments were given explicitly. */
    VSMap *base_args;
    VSPlugin *resize_plugin;

    std::vector<std::pair<ConversionKey, VSNode *>> conversions;
    std::mutex conversions_mutex;

    bool adapt_matrix;
    bool adapt_transfer;
    bool adapt_primaries;
    bool adapt_chroma_location;

    /* Primaries were given explicitly, which means a transfer function is
       needed on both ends even when nothing is known about the video's. */
    bool primaries_pinned;

    bool unsuitable_format;
    bool unsuitable_dimensions;

    int default_range;
};


/* Creates the clip that converts the subtitles the way key describes. With
   convert_colorspace false the transfer function and primaries are left alone
   and the matrix falls back to the default. That combination is the one that
   was used before any of this was decided per frame, so it always works. */
static VSNode *createConversion(MatchColorimetryData *d, const ConversionKey &key, bool convert_colorspace, std::string &error, const VSAPI *vsapi) {
    /* The subtitles are already exactly what is needed. */
    if (!d->unsuitable_format && !d->unsuitable_dimensions &&
        key.matrix == NOT_ADAPTED && key.transfer == NOT_ADAPTED &&
        key.primaries == NOT_ADAPTED && key.range == NOT_ADAPTED &&
        key.chroma_location == NOT_ADAPTED)
        return vsapi->mapGetNode(d->base_args, "clip", 0, nullptr);

    VSMap *args = vsapi->createMap();
    vsapi->copyMap(d->base_args, args);

    if (d->adapt_matrix)
        vsapi->mapSetInt(args, "matrix", convert_colorspace ? key.matrix : VSC_MATRIX_BT709, maReplace);

    bool convert_transfer = convert_colorspace && d->adapt_transfer && key.transfer != NOT_ADAPTED;
    bool convert_primaries = convert_colorspace && d->adapt_primaries && key.primaries != NOT_ADAPTED;

    /* Converting primaries means going through linear light, which can't be
       done without knowing the transfer function at both ends. Converting the
       transfer function alone doesn't need the primaries. */
    if (d->adapt_transfer && (convert_transfer || convert_primaries || d->primaries_pinned)) {
        vsapi->mapSetData(args, "transfer_in_s", SUBTITLE_TRANSFER_S, -1, dtUtf8, maReplace);
        vsapi->mapSetInt(args, "transfer", convert_transfer ? key.transfer : VSC_TRANSFER_BT709, maReplace);
    }

    if (convert_primaries) {
        vsapi->mapSetData(args, "primaries_in_s", SUBTITLE_PRIMARIES_S, -1, dtUtf8, maReplace);
        vsapi->mapSetInt(args, "primaries", key.primaries, maReplace);
    }

    if (key.range != NOT_ADAPTED)
        vsapi->mapSetData(args, "range_s", key.range == VSC_RANGE_LIMITED ? "limited" : "full", -1, dtUtf8, maReplace);

    if (key.chroma_location != NOT_ADAPTED)
        vsapi->mapSetInt(args, "chromaloc", key.chroma_location, maReplace);

    VSMap *ret = vsapi->invoke(d->resize_plugin, "Bicubic", args);
    vsapi->freeMap(args);

    if (vsapi->mapGetError(ret)) {
        error = vsapi->mapGetError(ret);
        vsapi->freeMap(ret);

        return nullptr;
    }

    VSNode *node = vsapi->mapGetNode(ret, "clip", 0, nullptr);
    vsapi->freeMap(ret);

    return node;
}


static VSNode *getConversion(MatchColorimetryData *d, const ConversionKey &key, std::string &error, const VSAPI *vsapi) {
    std::lock_guard<std::mutex> lock(d->conversions_mutex);

    for (size_t i = 0; i < d->conversions.size(); i++)
        if (d->conversions[i].first == key)
            return d->conversions[i].second;

    VSNode *node = createConversion(d, key, true, error, vsapi);

    /* Not every combination of tags can be converted between. Rather than
       failing the whole render, fall back to matching only the things that are
       known to work. */
    if (!node) {
        std::string ignored;
        node = createConversion(d, key, false, ignored, vsapi);
    }

    if (!node)
        return nullptr;

    error.clear();
    d->conversions.push_back(std::make_pair(key, node));

    return node;
}


static const VSFrame *VS_CC matchColorimetryGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    MatchColorimetryData *d = (MatchColorimetryData *)instanceData;

    (void)core;

    if (activationReason == arInitial) {
        vsapi->requestFrameFilter(n, d->prop_src, frameCtx);
    } else if (activationReason == arAllFramesReady && !frameData[0]) {
        const VSFrame *src = vsapi->getFrameFilter(n, d->prop_src, frameCtx);
        const VSMap *props = vsapi->getFramePropertiesRO(src);
        int err;

        ConversionKey key = { NOT_ADAPTED, NOT_ADAPTED, NOT_ADAPTED, NOT_ADAPTED, NOT_ADAPTED };

        if (d->adapt_matrix) {
            key.matrix = vsapi->mapGetIntSaturated(props, "_Matrix", 0, &err);
            if (err || !isSupportedMatrix(key.matrix))
                key.matrix = VSC_MATRIX_BT709;
        }

        if (d->adapt_transfer) {
            key.transfer = vsapi->mapGetIntSaturated(props, "_Transfer", 0, &err);
            if (err || !isSupportedTransfer(key.transfer))
                key.transfer = NOT_ADAPTED;
        }

        if (d->adapt_primaries) {
            key.primaries = vsapi->mapGetIntSaturated(props, "_Primaries", 0, &err);
            if (err || !isSupportedPrimaries(key.primaries))
                key.primaries = NOT_ADAPTED;
        }

        key.range = vsapi->mapGetIntSaturated(props, "_Range", 0, &err);
        if (err)
            key.range = d->default_range;
        else if (key.range != VSC_RANGE_LIMITED)
            /* Anything that isn't limited is treated as full, the same way
               MaskedMerge does it. */
            key.range = VSC_RANGE_FULL;

        if (d->adapt_chroma_location) {
            key.chroma_location = vsapi->mapGetIntSaturated(props, "_ChromaLocation", 0, &err);
            if (err || key.chroma_location < 0 || key.chroma_location >= NUM_CHROMA_LOCATIONS)
                key.chroma_location = 0;
        }

        vsapi->freeFrame(src);

        /* The subtitles are already exactly what is needed. */
        if (!d->unsuitable_format && !d->unsuitable_dimensions &&
            key.transfer == NOT_ADAPTED && key.primaries == NOT_ADAPTED &&
            key.range != VSC_RANGE_LIMITED) {
            key.matrix = NOT_ADAPTED;
            key.range = NOT_ADAPTED;
        }

        std::string error;
        VSNode *node = getConversion(d, key, error, vsapi);

        if (!node) {
            vsapi->setFilterError(("MatchColorimetry: " + error).c_str(), frameCtx);

            return nullptr;
        }

        frameData[0] = node;

        vsapi->requestFrameFilter(n, node, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        return vsapi->getFrameFilter(n, (VSNode *)frameData[0], frameCtx);
    }

    return nullptr;
}


static void VS_CC matchColorimetryFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    MatchColorimetryData *d = (MatchColorimetryData *)instanceData;

    (void)core;

    vsapi->freeNode(d->prop_src);
    vsapi->freeMap(d->base_args);

    for (size_t i = 0; i < d->conversions.size(); i++)
        vsapi->freeNode(d->conversions[i].second);

    delete d;
}


/* Converts subs to the format and dimensions of clip, following the colour
   range, chroma location, matrix, transfer function and primaries of each
   individual clip frame. The reference to subs is consumed. Returns NULL and
   fills in error on failure. */
static VSNode *matchColorimetry(VSNode *clip, VSNode *subs, const VSMap *in, int unsuitable_format, int unsuitable_dimensions, std::string &error, VSCore *core, const VSAPI *vsapi) {
    const VSVideoInfo *clip_vi = vsapi->getVideoInfo(clip);
    int err;

    MatchColorimetryData *d = new MatchColorimetryData();

    d->prop_src = vsapi->addNodeRef(clip);
    d->resize_plugin = vsapi->getPluginByID(VSH_RESIZE_PLUGIN_ID, core);
    d->unsuitable_format = !!unsuitable_format;
    d->unsuitable_dimensions = !!unsuitable_dimensions;
    /* Same fallback as MaskedMerge uses for frames without the property. */
    d->default_range = (clip_vi->format.colorFamily == cfYUV || clip_vi->format.colorFamily == cfGray) ? VSC_RANGE_LIMITED : VSC_RANGE_FULL;

    d->base_args = vsapi->createMap();
    vsapi->mapConsumeNode(d->base_args, "clip", subs, maReplace);

    if (unsuitable_format)
        vsapi->mapSetInt(d->base_args, "format", vsapi->queryVideoFormatID(clip_vi->format.colorFamily, clip_vi->format.sampleType, clip_vi->format.bitsPerSample, clip_vi->format.subSamplingW, clip_vi->format.subSamplingH, core), maReplace);

    if (unsuitable_dimensions) {
        vsapi->mapSetInt(d->base_args, "width", clip_vi->width, maReplace);
        vsapi->mapSetInt(d->base_args, "height", clip_vi->height, maReplace);
    }

    /* Anything given explicitly is used for every frame, everything else
       follows the frame properties of the clip. */
    int matrix = vsapi->mapGetIntSaturated(in, "matrix", 0, &err);
    if (!err)
        vsapi->mapSetInt(d->base_args, "matrix", matrix, maReplace);
    const char *matrix_s = vsapi->mapGetData(in, "matrix_s", 0, &err);
    if (!err)
        vsapi->mapSetData(d->base_args, "matrix_s", matrix_s, -1, dtUtf8, maReplace);

    int transfer = vsapi->mapGetIntSaturated(in, "transfer", 0, &err);
    if (!err)
        vsapi->mapSetInt(d->base_args, "transfer", transfer, maReplace);
    const char *transfer_s = vsapi->mapGetData(in, "transfer_s", 0, &err);
    if (!err)
        vsapi->mapSetData(d->base_args, "transfer_s", transfer_s, -1, dtUtf8, maReplace);

    int primaries = vsapi->mapGetIntSaturated(in, "primaries", 0, &err);
    if (!err)
        vsapi->mapSetInt(d->base_args, "primaries", primaries, maReplace);
    const char *primaries_s = vsapi->mapGetData(in, "primaries_s", 0, &err);
    if (!err)
        vsapi->mapSetData(d->base_args, "primaries_s", primaries_s, -1, dtUtf8, maReplace);

    bool matrix_given = vsapi->mapGetType(in, "matrix") != ptUnset || vsapi->mapGetType(in, "matrix_s") != ptUnset;
    bool transfer_given = vsapi->mapGetType(in, "transfer") != ptUnset || vsapi->mapGetType(in, "transfer_s") != ptUnset;
    bool primaries_given = vsapi->mapGetType(in, "primaries") != ptUnset || vsapi->mapGetType(in, "primaries_s") != ptUnset;

    /* Converting into a requested transfer function or set of primaries only
       works when the subtitles have both of them to start from. */
    if (transfer_given || primaries_given)
        vsapi->mapSetData(d->base_args, "transfer_in_s", SUBTITLE_TRANSFER_S, -1, dtUtf8, maReplace);

    if (primaries_given) {
        vsapi->mapSetData(d->base_args, "primaries_in_s", SUBTITLE_PRIMARIES_S, -1, dtUtf8, maReplace);

        if (!transfer_given)
            vsapi->mapSetData(d->base_args, "transfer_s", SUBTITLE_TRANSFER_S, -1, dtUtf8, maReplace);
    }

    /* A matrix is only needed, and only allowed, when there is chroma to
       encode. */
    bool needs_matrix = !!unsuitable_format && clip_vi->format.colorFamily != cfRGB;

    d->adapt_matrix = needs_matrix && !matrix_given;
    d->adapt_transfer = !transfer_given;
    d->adapt_primaries = !primaries_given;
    d->primaries_pinned = primaries_given;
    d->adapt_chroma_location = !!unsuitable_format &&
                               clip_vi->format.colorFamily == cfYUV &&
                               (clip_vi->format.subSamplingW || clip_vi->format.subSamplingH);

    /* A matrix has to be given when converting RGB to YUV, so the fallback
       conversion needs one even when the matrix isn't adapted to. */
    if (needs_matrix && !matrix_given)
        vsapi->mapSetData(d->base_args, "matrix_s", "709", -1, dtUtf8, maReplace);

    /* The colour range always has to be matched, so there is no combination of
       arguments that makes the per frame filter unnecessary. */

    VSFilterDependency deps[] = {{ clip, rpStrictSpatial }};

    VSNode *node = vsapi->createVideoFilter2("MatchColorimetry", vsapi->getVideoInfo(clip), matchColorimetryGetFrame, matchColorimetryFree, fmParallelRequests, deps, 1, d, core);

    if (!node) {
        error = "failed to create the colorimetry matching filter.";
        matchColorimetryFree(d, core, vsapi);

        return nullptr;
    }

    return node;
}


void blendSubtitles(VSNode *clip, VSNode *subs, const VSMap *in, VSMap *out, const std::string &filter_name, VSCore *core, const VSAPI *vsapi) {
    std::string error;
    auto fail = [&](const std::string &message) {
        vsapi->mapSetError(out, (filter_name + ": " + message).c_str());
    };
    VSPlugin *std_plugin = vsapi->getPluginByID(VSH_STD_PLUGIN_ID, core);
    VSPlugin *resize_plugin = vsapi->getPluginByID(VSH_RESIZE_PLUGIN_ID, core);

    VSMap *args, *ret;

    args = vsapi->createMap();
    vsapi->mapSetNode(args, "clip", subs, maReplace);
    ret = vsapi->invoke(std_plugin, "PropToClip", args);
    vsapi->freeMap(args);
    if (vsapi->mapGetError(ret)) {
        /* PropToClip renders the first frame to find out what the alpha looks
           like, so this fails when the subtitles themselves can't be rendered. */
        fail(vsapi->mapGetError(ret));
        vsapi->freeMap(ret);
        return;
    }

    VSNode *alpha = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);


    args = vsapi->createMap();
    vsapi->mapSetNode(args, "clip", subs, maReplace);
    vsapi->mapSetNode(args, "alpha", alpha, maReplace);

    ret = vsapi->invoke(std_plugin, "PreMultiply", args);
    vsapi->freeMap(args);
    if (vsapi->mapGetError(ret)) {
        fail(vsapi->mapGetError(ret));
        vsapi->freeMap(ret);
        vsapi->freeNode(alpha);
        return;
    }

    subs = vsapi->mapGetNode(ret, "clip", 0, NULL);
    vsapi->freeMap(ret);

    const VSVideoInfo *clip_vi = vsapi->getVideoInfo(clip);
    const VSVideoInfo *subs_vi = vsapi->getVideoInfo(subs);
    const VSVideoInfo *alpha_vi = vsapi->getVideoInfo(alpha);

    int unsuitable_format = !vsh::isSameVideoFormat(&clip_vi->format, &subs_vi->format);
    int unsuitable_dimensions =
            clip_vi->width != subs_vi->width ||
            clip_vi->height != subs_vi->height;

    subs = matchColorimetry(clip, subs, in, unsuitable_format, unsuitable_dimensions, error, core, vsapi);
    if (!subs) {
        fail(error);
        vsapi->freeNode(alpha);
        return;
    }


    unsuitable_format =
            clip_vi->format.bitsPerSample != alpha_vi->format.bitsPerSample ||
            clip_vi->format.sampleType != alpha_vi->format.sampleType;

    if (unsuitable_format || unsuitable_dimensions) {
        args = vsapi->createMap();
        vsapi->mapConsumeNode(args, "clip", alpha, maReplace);

        if (unsuitable_format) {
            uint32_t alpha_format = vsapi->queryVideoFormatID(alpha_vi->format.colorFamily,
                                                                 clip_vi->format.sampleType,
                                                                 clip_vi->format.bitsPerSample,
                                                                 alpha_vi->format.subSamplingW,
                                                                 alpha_vi->format.subSamplingH,
                                                                 core);
            if (!alpha_format) {
                fail("Failed to register format with "
                     "color family " + std::to_string(alpha_vi->format.colorFamily) + ", "
                     "sample type " + std::to_string(clip_vi->format.sampleType) + ", "
                     "bit depth " + std::to_string(clip_vi->format.bitsPerSample) + ", "
                     "horizontal subsampling " + std::to_string(alpha_vi->format.subSamplingW) + ", "
                     "vertical subsampling " + std::to_string(alpha_vi->format.subSamplingH) + ".");
                vsapi->freeNode(subs);
                vsapi->freeMap(args);
                return;
            }

            vsapi->mapSetInt(args, "format", alpha_format, maReplace);
        }

        if (unsuitable_dimensions) {
            vsapi->mapSetInt(args, "width", clip_vi->width, maReplace);
            vsapi->mapSetInt(args, "height", clip_vi->height, maReplace);
        }

        ret = vsapi->invoke(resize_plugin, "Bicubic", args);
        vsapi->freeMap(args);
        if (vsapi->mapGetError(ret)) {
            fail(vsapi->mapGetError(ret));
            vsapi->freeMap(ret);
            vsapi->freeNode(subs);
            return;
        }

        alpha = vsapi->mapGetNode(ret, "clip", 0, NULL);
        vsapi->freeMap(ret);
    }

    args = vsapi->createMap();
    vsapi->mapSetNode(args, "clipa", clip, maReplace);
    vsapi->mapConsumeNode(args, "clipb", subs, maReplace);
    vsapi->mapConsumeNode(args, "mask", alpha, maReplace);
    vsapi->mapSetInt(args, "premultiplied", 1, maReplace);

    ret = vsapi->invoke(std_plugin, "MaskedMerge", args);
    vsapi->freeMap(args);
    if (vsapi->mapGetError(ret)) {
        fail(vsapi->mapGetError(ret));
        vsapi->freeMap(ret);
        return;
    }

    vsapi->mapConsumeNode(out, "clip", vsapi->mapGetNode(ret, "clip", 0, NULL), maReplace);
    vsapi->freeMap(ret);
}
