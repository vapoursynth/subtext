#ifndef SUBTEXT_COMMON_H
#define SUBTEXT_COMMON_H

#include <memory>
#include <optional>
#include <string>

#include <ass/ass.h>

#include <VapourSynth4.h>


struct AssLibraryDeleter {
    void operator()(ASS_Library *library) const { ass_library_done(library); }
};

struct AssRendererDeleter {
    void operator()(ASS_Renderer *renderer) const { ass_renderer_done(renderer); }
};

struct AssTrackDeleter {
    void operator()(ASS_Track *track) const { ass_free_track(track); }
};

using AssLibraryPtr = std::unique_ptr<ASS_Library, AssLibraryDeleter>;
using AssRendererPtr = std::unique_ptr<ASS_Renderer, AssRendererDeleter>;
using AssTrackPtr = std::unique_ptr<ASS_Track, AssTrackDeleter>;

/* Burns subs into clip and stores the result in out. Errors are reported
   through out. */
void blendSubtitles(VSNode *clip, VSNode *subs, const VSMap *in, VSMap *out, const std::string &filter_name, VSCore *core, const VSAPI *vsapi);

/* Reads the file and converts it from charset to UTF-8. */
std::optional<std::string> convertToUtf8(const std::string &file_name, const std::string &charset, std::string &error);

/* Turns a subtitle format libass doesn't understand into an ASS track by
   running it through libavformat and libavcodec. A null user_style keeps the
   style libavcodec comes up with. */
ASS_Track *convertToASS(const std::string &file_name, const std::string &contents, ASS_Library *ass_library, const char *user_style, const std::string &charset, std::string &error);

void VS_CC imageFileCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi);

#endif // SUBTEXT_COMMON_H
