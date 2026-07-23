#ifndef SUBTEXT_FFMPEG_H
#define SUBTEXT_FFMPEG_H

#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}


struct AVFormatContextDeleter {
    void operator()(AVFormatContext *fctx) const { avformat_close_input(&fctx); }
};

struct AVCodecContextDeleter {
    void operator()(AVCodecContext *avctx) const { avcodec_free_context(&avctx); }
};

struct AVPacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};

using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;


/* Opening replaces the context and nulls the pointer on failure, which needs a
   little dance to stay compatible with unique_ptr. */
inline int openInput(AVFormatContextPtr &fctx, const char *url) {
    AVFormatContext *opened = fctx.release();
    int ret = avformat_open_input(&opened, url, nullptr, nullptr);
    fctx.reset(opened);

    return ret;
}


/* Turns an AVERROR into something printable. */
inline std::string avErrorString(int ret) {
    char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };

    if (av_strerror(ret, av_error, AV_ERROR_MAX_STRING_SIZE))
        return "unknown error " + std::to_string(ret);

    return av_error;
}

#endif // SUBTEXT_FFMPEG_H
