#include "gpu/shim/bbport_media_checks.h"
#include <cassert>
#include <cerrno>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

template <typename Action>
static std::string aborted_output(Action action) {
    int pipe_fds[2];
    assert(pipe(pipe_fds) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pipe_fds[0]);
        assert(dup2(pipe_fds[1], STDERR_FILENO) >= 0);
        close(pipe_fds[1]);
        action();
        _exit(99);
    }
    close(pipe_fds[1]);
    std::string output;
    char buffer[1024];
    ssize_t bytes;
    while ((bytes = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) output.append(buffer, bytes);
    assert(bytes == 0);
    close(pipe_fds[0]);
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    return output;
}

int main() {
    for (AVCodecID codec : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_AAC}) {
        assert(BbMedia::RequireDecoder("supported.mp4", codec));
    }
    assert(!avcodec_find_decoder(AV_CODEC_ID_VP9));
    const std::string decoder_error = aborted_output([] {
        BbMedia::RequireDecoder("unsupported-video.webm", AV_CODEC_ID_VP9);
    });
    assert(decoder_error.find("vp9") != std::string::npos);
    assert(decoder_error.find("unsupported-video.webm") != std::string::npos);

    const std::string demux_error = aborted_output([] {
        AVFormatContext* context = nullptr;
        const int result = avformat_open_input(&context, "/dev/null", nullptr, nullptr);
        assert(result < 0);
        BbMedia::RequireDemuxResult("unrecognized-movie.bin", result);
    });
    assert(demux_error.find("unrecognized-movie.bin") != std::string::npos);
    assert(demux_error.find("built demuxers:") != std::string::npos);
    assert(demux_error.find("mov") != std::string::npos);
    assert(demux_error.find("mpegts") != std::string::npos);
    std::puts("Darwin media codec and demux failure tests passed");
}
