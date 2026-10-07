#pragma once

#ifdef __APPLE__
#include <cstdio>
#include <cstdlib>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
}

namespace BbMedia {
inline const AVCodec* RequireDecoder(std::string_view path, AVCodecID codec) {
    const AVCodec* decoder = avcodec_find_decoder(codec);
    if (!decoder) {
        std::fprintf(stderr, "macOS AvPlayer: decoder '%s' is unavailable for '%.*s'\n",
                     avcodec_get_name(codec), int(path.size()), path.data());
        std::abort();
    }
    return decoder;
}

inline void RequireDemuxResult(std::string_view path, int result) {
    if (result >= 0) return;
    char error[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(result, error, sizeof(error));
    std::fprintf(stderr, "macOS AvPlayer: cannot demux '%.*s': %s; built demuxers:",
                 int(path.size()), path.data(), error);
    void* cursor = nullptr;
    while (const AVInputFormat* format = av_demuxer_iterate(&cursor)) {
        std::fprintf(stderr, " %s", format->name);
    }
    std::fputc('\n', stderr);
    std::abort();
}
} // namespace BbMedia
#endif
