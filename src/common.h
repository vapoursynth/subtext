#ifndef SUBTEXT_COMMON_H
#define SUBTEXT_COMMON_H

#include <cstddef>

#include <VapourSynth4.h>

void blendSubtitles(VSNode *clip, VSNode *subs, const VSMap *in, VSMap *out, const char *filter_name, char *error, size_t error_size, VSCore *core, const VSAPI *vsapi);

#endif // SUBTEXT_COMMON_H
