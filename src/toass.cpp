
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <iconv.h>

#include "common.h"
#include "ffmpeg.h"

extern "C" {
#include <libavutil/mathematics.h>
}


struct MemoryFile {
    const char *data;
    int64_t total_size;
    int64_t current_position;


    static int64_t seek(void *opaque, int64_t offset, int whence);

    static int readPacket(void *opaque, uint8_t *buf, int bytes_to_read);
};


int64_t MemoryFile::seek(void *opaque, int64_t offset, int whence) {
    if (whence & AVSEEK_FORCE)
        whence &= ~AVSEEK_FORCE;

    MemoryFile *mf = (MemoryFile *)opaque;

    if (whence == AVSEEK_SIZE) {
        return mf->total_size;
    } else if (whence == SEEK_SET) {
    } else if (whence == SEEK_CUR) {
        offset += mf->current_position;
    } else if (whence == SEEK_END) {
        offset += mf->total_size;
    } else {
        return -1;
    }

    if (offset < 0)
        return AVERROR(EINVAL);

    mf->current_position = offset;
    return mf->current_position;
}



// A custom AVIOContext is never freed by avformat_close_input() or by
// avformat_open_input()'s failure path, so it has to be taken care of by hand.
struct AVIOHolder {
    AVIOContext *ctx;
    uint8_t *buffer;

    AVIOHolder() : ctx(nullptr), buffer(nullptr) { }

    ~AVIOHolder() {
        if (ctx) {
            // The original buffer may have been replaced along the way.
            av_freep(&ctx->buffer);
            avio_context_free(&ctx);
        } else {
            av_freep(&buffer);
        }
    }
};


int MemoryFile::readPacket(void *opaque, uint8_t *buf, int bytes_to_read) {
    MemoryFile *mf = (MemoryFile *)opaque;

    if (mf->current_position >= mf->total_size)
        return AVERROR_EOF;

    int bytes_read = static_cast<int>(std::min<int64_t>(bytes_to_read, mf->total_size - mf->current_position));

    memcpy(buf, mf->data + mf->current_position, bytes_read);

    mf->current_position += bytes_read;

    return bytes_read;
}


ASS_Track *convertToASS(const std::string &file_name, const std::string &contents, ASS_Library *ass_library, const char *user_style, const std::string &charset, std::string &error) {
    av_log_set_level(AV_LOG_PANIC); /// would be good to have a parameter for this

    MemoryFile memory_file = { };
    memory_file.data = contents.data();
    memory_file.total_size = contents.size();

    AVFormatContextPtr fctx;
    AVIOHolder avio;

    try {
        fctx.reset(avformat_alloc_context());
        if (!fctx)
            throw std::string("failed to allocate AVFormatContext.");

        const int avio_buffer_size = 4 * 1024;

        avio.buffer = (uint8_t *)av_malloc(avio_buffer_size);
        if (!avio.buffer)
            throw std::string("failed to allocate buffer for AVIOContext.");

        avio.ctx = avio_alloc_context(avio.buffer, avio_buffer_size, 0, &memory_file, MemoryFile::readPacket, nullptr, MemoryFile::seek);
        if (!avio.ctx)
            throw std::string("failed to allocate AVIOContext.");

        fctx->pb = avio.ctx;
        // Says that the AVIOContext belongs to us, which is what keeps closing
        // the AVFormatContext from touching it.
        fctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    } catch (const std::string &e) {
        error = e;

        return nullptr;
    }

    int ret = 0;

    try {
        ret = openInput(fctx, file_name.c_str());
        if (ret < 0)
            throw std::string("avformat_open_input failed: ");

        ret = avformat_find_stream_info(fctx.get(), nullptr);
        if (ret < 0)
            throw std::string("avformat_find_stream_info failed: ");
    } catch (const std::string &e) {
        error = e + avErrorString(ret) + ".";

        return nullptr;
    }

    if (fctx->nb_streams == 0) {
        error = "no streams found.";

        return nullptr;
    }

    int stream_index = 0;

    enum AVCodecID codec_id = fctx->streams[stream_index]->codecpar->codec_id;

    AVCodecContextPtr avctx;

    ret = 0;

    try {
        const AVCodecDescriptor *descriptor = avcodec_descriptor_get(codec_id);
        if (!descriptor)
            throw std::string("failed to identify the codec of the first stream.");

        if (descriptor->type != AVMEDIA_TYPE_SUBTITLE || !(descriptor->props & AV_CODEC_PROP_TEXT_SUB))
            throw std::string("file is not a text subtitle.");

        const AVCodec *decoder = avcodec_find_decoder(codec_id);
        if (!decoder)
            throw std::string("failed to find decoder for '") + avcodec_get_name(codec_id) + "'.";

        avctx.reset(avcodec_alloc_context3(decoder));
        if (!avctx)
            throw std::string("failed to allocate AVCodecContext.");

        int extradata_size = fctx->streams[stream_index]->codecpar->extradata_size;
        if (extradata_size) {
            avctx->extradata_size = extradata_size;
            avctx->extradata = (uint8_t *)av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (!avctx->extradata)
                throw std::string("failed to allocate extradata.");

            memcpy(avctx->extradata, fctx->streams[stream_index]->codecpar->extradata, extradata_size);
        }

        ret = avcodec_open2(avctx.get(), decoder, NULL);
        if (ret < 0)
            throw std::string("failed to open AVCodecContext.");

        if (!avctx->subtitle_header_size)
            throw std::string("no subtitle header found in AVCodecContext.");
    } catch (const std::string &e) {
        error = e;

        return nullptr;
    }

    // Packet timestamps arrive in the demuxer's time base, which varies between
    // subtitle formats (milliseconds for SRT, centiseconds for JACOsub, frames
    // for MicroDVD, ...). avcodec_decode_subtitle2() also uses this to convert
    // packet durations into display durations in milliseconds.
    avctx->pkt_timebase = fctx->streams[stream_index]->time_base;


    std::string ass_file;

    if (user_style) {
        std::string subtitle_header((char *)avctx->subtitle_header, avctx->subtitle_header_size);

        size_t style_position = subtitle_header.find("Style:");

        if (style_position == std::string::npos) {
            error = "subtitle header in AVCodecContext has no styles.";

            return nullptr;
        }

        size_t events_position = subtitle_header.find("[Events]");

        if (events_position == std::string::npos) {
            error = "subtitle header in AVCodecContext has no [Events] section.";

            return nullptr;
        }

        size_t events_size = avctx->subtitle_header_size - events_position;

        ass_file.append((char *)avctx->subtitle_header, style_position);
        ass_file.append("Style: Default,");
        ass_file.append(user_style);
        ass_file.append("\n");
        ass_file.append((char *)avctx->subtitle_header + events_position, events_size);
    } else {
        ass_file.append((char *)avctx->subtitle_header, avctx->subtitle_header_size);
    }


    AVPacketPtr packet(av_packet_alloc());
    if (!packet) {
        error = "failed to allocate AVPacket.";

        return nullptr;
    }

    AssTrackPtr track(ass_read_memory(ass_library, ass_file.data(), ass_file.size(), nullptr));
    if (!track) {
        error = "libass failed to parse the subtitle header. Is '" + file_name + "' really a subtitle file?";

        return nullptr;
    }

    AVSubtitle avsub;

    int total_events = 0;
    int failed_decoding = 0;
    int read_ret;

    while ((read_ret = av_read_frame(fctx.get(), packet.get())) == 0) {
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet.get());
            continue;
        }

        total_events++;

        // An event that can't be placed in time is useless.
        if (packet->pts == AV_NOPTS_VALUE) {
            av_packet_unref(packet.get());
            failed_decoding++;
            continue;
        }

        int got_avsub = 0;

        ret = avcodec_decode_subtitle2(avctx.get(), &avsub, &got_avsub, packet.get());
        if (ret < 0 || !got_avsub) {
            av_packet_unref(packet.get());
            failed_decoding++;
            continue;
        }

        // start_display_time and end_display_time are in ms relative to the pts.
        int64_t start_ms = av_rescale_q(packet->pts, fctx->streams[stream_index]->time_base, av_make_q(1, 1000));

        for (unsigned i = 0; i < avsub.num_rects; i++) {
            AVSubtitleRect *rect = avsub.rects[i];

            if (rect->type != SUBTITLE_ASS || !rect->ass || !rect->ass[0])
                continue;

            ass_process_chunk(track.get(), rect->ass, static_cast<int>(strlen(rect->ass)), start_ms + avsub.start_display_time, avsub.end_display_time - avsub.start_display_time);
        }

        avsubtitle_free(&avsub);
        av_packet_unref(packet.get());
    }

    if (read_ret != AVERROR_EOF) {
        error = "av_read_frame failed: " + avErrorString(read_ret) + ".";

        return nullptr;
    }

    if (failed_decoding > 0) {
        error = "failed to decode " + std::to_string(failed_decoding) + " events out of " + std::to_string(total_events) + ". Are you sure '" + charset + "' is the correct charset?";

        return nullptr;
    }

    return track.release();
}
