#include "hls_muxer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, uint8_t **buf, size_t *size)
{
    FILE *fp;
    long len;
    uint8_t *mem;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    len = ftell(fp);
    if (len <= 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    mem = (uint8_t *)malloc((size_t)len);
    if (mem == NULL) {
        fclose(fp);
        return -1;
    }

    if (fread(mem, 1, (size_t)len, fp) != (size_t)len) {
        free(mem);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *buf = mem;
    *size = (size_t)len;
    return 0;
}

int main(int argc, char **argv)
{
    hls_muxer_t *muxer = NULL;
    hls_muxer_config_t cfg;
    uint8_t *video = NULL;
    size_t video_size = 0;
    uint8_t *audio = NULL;
    size_t audio_size = 0;
    uint64_t pts_video = 0;
    uint64_t pts_audio = 0;
    int i;

    if (argc != 4) {
        fprintf(stderr, "usage: %s <video.au> <aac.adts> <output_dir>\n", argv[0]);
        return 1;
    }

    if (read_file(argv[1], &video, &video_size) != 0) {
        fprintf(stderr, "read video failed\n");
        return 1;
    }

    if (read_file(argv[2], &audio, &audio_size) != 0) {
        free(video);
        fprintf(stderr, "read audio failed\n");
        return 1;
    }

    cfg.output_dir = argv[3];
    cfg.playlist_name = "live.m3u8";
    cfg.segment_prefix = "seg_";
    cfg.target_duration_sec = 2;
    cfg.playlist_length = 6;
    cfg.video_codec = hls_detect_h265_irap(video, video_size) ? HLS_VIDEO_CODEC_H265 : HLS_VIDEO_CODEC_H264;
    cfg.audio_codec = HLS_AUDIO_CODEC_AAC;

    if (hls_muxer_open(&muxer, &cfg) != HLS_OK) {
        free(video);
        free(audio);
        fprintf(stderr, "muxer open failed\n");
        return 1;
    }

    for (i = 0; i < 10; ++i) {
        if (hls_muxer_input_video(muxer,
                                  video,
                                  video_size,
                                  pts_video,
                                  pts_video,
                                  1) != HLS_OK) {
            fprintf(stderr, "write video failed\n");
            break;
        }
        pts_video += 180000;

        if (hls_muxer_input_audio(muxer, audio, audio_size, pts_audio) != HLS_OK) {
            fprintf(stderr, "write audio failed\n");
            break;
        }
        pts_audio += 180000;
    }

    hls_muxer_close(muxer, 1);
    free(video);
    free(audio);
    return 0;
}
