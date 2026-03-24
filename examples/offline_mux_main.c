#include "hls_muxer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t offset;
    size_t size;
} sample_t;

typedef struct {
    sample_t *items;
    size_t count;
    size_t capacity;
} sample_list_t;

typedef struct {
    uint64_t num;
    uint64_t den;
    uint64_t acc;
    uint64_t pts90k;
} pts_gen_t;

static int find_start_code(const uint8_t *data, size_t size, size_t off, size_t *code_off, size_t *code_size);

static int detect_annexb_video_codec(const uint8_t *data, size_t size)
{
    size_t off = 0;

    while (off < size) {
        size_t sc_off;
        size_t sc_size;

        if (!find_start_code(data, size, off, &sc_off, &sc_size)) {
            break;
        }

        if (sc_off + sc_size >= size) {
            break;
        }

        switch (data[sc_off + sc_size] & 0x1f) {
        case 7:
        case 8:
        case 9:
            return HLS_VIDEO_CODEC_H264;
        default:
            break;
        }

        switch ((data[sc_off + sc_size] >> 1) & 0x3f) {
        case 32:
        case 33:
        case 34:
        case 35:
            return HLS_VIDEO_CODEC_H265;
        default:
            break;
        }

        off = sc_off + sc_size + 1;
    }

    if (hls_detect_h264_idr(data, size)) {
        return HLS_VIDEO_CODEC_H264;
    }

    if (hls_detect_h265_irap(data, size)) {
        return HLS_VIDEO_CODEC_H265;
    }

    return HLS_VIDEO_CODEC_NONE;
}

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

static int sample_list_append(sample_list_t *list, size_t offset, size_t size)
{
    sample_t *items;
    size_t new_capacity;

    if (list->count == list->capacity) {
        new_capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        items = (sample_t *)realloc(list->items, new_capacity * sizeof(*items));
        if (items == NULL) {
            return -1;
        }
        list->items = items;
        list->capacity = new_capacity;
    }

    list->items[list->count].offset = offset;
    list->items[list->count].size = size;
    list->count += 1;
    return 0;
}

static void sample_list_free(sample_list_t *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int find_start_code(const uint8_t *data, size_t size, size_t off, size_t *code_off, size_t *code_size)
{
    size_t i;

    for (i = off; i + 3 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                *code_off = i;
                *code_size = 3;
                return 1;
            }
            if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                *code_off = i;
                *code_size = 4;
                return 1;
            }
        }
    }

    return 0;
}

static int split_h264_aus(const uint8_t *data, size_t size, sample_list_t *aus)
{
    size_t off = 0;
    size_t current_au = (size_t)-1;

    while (off < size) {
        size_t sc_off;
        size_t sc_size;
        size_t next_sc_off;
        size_t next_sc_size;
        size_t nal_end;
        uint8_t nal_type;

        if (!find_start_code(data, size, off, &sc_off, &sc_size)) {
            break;
        }

        nal_end = size;
        if (find_start_code(data, size, sc_off + sc_size, &next_sc_off, &next_sc_size)) {
            nal_end = next_sc_off;
        } else {
            next_sc_off = size;
            next_sc_size = 0;
        }

        if (sc_off + sc_size >= size) {
            break;
        }

        nal_type = (uint8_t)(data[sc_off + sc_size] & 0x1f);
        if (nal_type == 9) {
            if (current_au != (size_t)-1) {
                aus->items[current_au].size = sc_off - aus->items[current_au].offset;
            }
            if (sample_list_append(aus, sc_off, nal_end - sc_off) != 0) {
                return -1;
            }
            current_au = aus->count - 1;
        } else if (current_au == (size_t)-1) {
            if (sample_list_append(aus, sc_off, nal_end - sc_off) != 0) {
                return -1;
            }
            current_au = aus->count - 1;
        }

        if (next_sc_size == 0) {
            break;
        }
        off = next_sc_off;
    }

    if (current_au == (size_t)-1) {
        return -1;
    }

    aus->items[current_au].size = size - aus->items[current_au].offset;
    return 0;
}

static int split_h265_aus(const uint8_t *data, size_t size, sample_list_t *aus)
{
    size_t off = 0;
    size_t current_au = (size_t)-1;

    while (off < size) {
        size_t sc_off;
        size_t sc_size;
        size_t next_sc_off;
        size_t next_sc_size;
        size_t nal_end;
        uint8_t nal_type;

        if (!find_start_code(data, size, off, &sc_off, &sc_size)) {
            break;
        }

        nal_end = size;
        if (find_start_code(data, size, sc_off + sc_size, &next_sc_off, &next_sc_size)) {
            nal_end = next_sc_off;
        } else {
            next_sc_off = size;
            next_sc_size = 0;
        }

        if (sc_off + sc_size + 1 >= size) {
            break;
        }

        nal_type = (uint8_t)((data[sc_off + sc_size] >> 1) & 0x3f);
        if (nal_type == 35) {
            if (current_au != (size_t)-1) {
                aus->items[current_au].size = sc_off - aus->items[current_au].offset;
            }
            if (sample_list_append(aus, sc_off, nal_end - sc_off) != 0) {
                return -1;
            }
            current_au = aus->count - 1;
        } else if (current_au == (size_t)-1) {
            if (sample_list_append(aus, sc_off, nal_end - sc_off) != 0) {
                return -1;
            }
            current_au = aus->count - 1;
        }

        if (next_sc_size == 0) {
            break;
        }
        off = next_sc_off;
    }

    if (current_au == (size_t)-1) {
        return -1;
    }

    aus->items[current_au].size = size - aus->items[current_au].offset;
    return 0;
}

static int split_adts_frames(const uint8_t *data, size_t size, sample_list_t *frames)
{
    size_t off = 0;

    while (off + 7 <= size) {
        hls_adts_info_t info;

        if (!hls_parse_adts(&data[off], size - off, &info)) {
            return -1;
        }

        if (info.frame_length <= 0 || off + (size_t)info.frame_length > size) {
            return -1;
        }

        if (sample_list_append(frames, off, (size_t)info.frame_length) != 0) {
            return -1;
        }

        off += (size_t)info.frame_length;
    }

    return off == size ? 0 : -1;
}

static void pts_gen_init(pts_gen_t *gen, uint64_t num, uint64_t den)
{
    gen->num = num;
    gen->den = den;
    gen->acc = 0;
    gen->pts90k = 0;
}

static uint64_t pts_gen_peek(const pts_gen_t *gen)
{
    return gen->pts90k;
}

static void pts_gen_advance(pts_gen_t *gen)
{
    gen->acc += gen->num;
    gen->pts90k += gen->acc / gen->den;
    gen->acc %= gen->den;
}

int main(int argc, char **argv)
{
    hls_muxer_t *muxer = NULL;
    hls_muxer_config_t cfg;
    hls_adts_info_t adts_info;
    sample_list_t video_aus;
    sample_list_t audio_frames;
    pts_gen_t video_pts;
    pts_gen_t audio_pts;
    uint8_t *video = NULL;
    uint8_t *audio = NULL;
    size_t video_size = 0;
    size_t audio_size = 0;
    size_t vi = 0;
    size_t ai = 0;
    int fps_num;
    int fps_den;
    int video_codec;
    int rc = 1;

    memset(&video_aus, 0, sizeof(video_aus));
    memset(&audio_frames, 0, sizeof(audio_frames));

    if (argc != 6) {
        fprintf(stderr, "usage: %s <video.h26x> <audio.aac> <output_dir> <fps_num> <fps_den>\n", argv[0]);
        return 1;
    }

    fps_num = atoi(argv[4]);
    fps_den = atoi(argv[5]);
    if (fps_num <= 0 || fps_den <= 0) {
        fprintf(stderr, "invalid fps\n");
        return 1;
    }

    if (read_file(argv[1], &video, &video_size) != 0 || read_file(argv[2], &audio, &audio_size) != 0) {
        fprintf(stderr, "read input files failed\n");
        goto done;
    }

    if (!hls_parse_adts(audio, audio_size, &adts_info)) {
        fprintf(stderr, "parse first adts frame failed\n");
        goto done;
    }

    video_codec = detect_annexb_video_codec(video, video_size);
    if (video_codec == HLS_VIDEO_CODEC_H264) {
        if (split_h264_aus(video, video_size, &video_aus) != 0) {
            fprintf(stderr, "split h264 access units failed\n");
            goto done;
        }
    } else if (video_codec == HLS_VIDEO_CODEC_H265) {
        if (split_h265_aus(video, video_size, &video_aus) != 0) {
            fprintf(stderr, "split h265 access units failed\n");
            goto done;
        }
    } else {
        fprintf(stderr, "detect video codec failed\n");
        goto done;
    }

    if (split_adts_frames(audio, audio_size, &audio_frames) != 0) {
        fprintf(stderr, "split adts frames failed\n");
        goto done;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.output_dir = argv[3];
    cfg.playlist_name = "live.m3u8";
    cfg.segment_prefix = "seg_";
    cfg.target_duration_sec = 2;
    cfg.playlist_length = 6;
    cfg.video_codec = (hls_video_codec_t)video_codec;
    cfg.audio_codec = HLS_AUDIO_CODEC_AAC;
    cfg.debug_write_files = 1;

    if (hls_muxer_open(&muxer, &cfg) != HLS_OK) {
        fprintf(stderr, "hls_muxer_open failed\n");
        goto done;
    }

    pts_gen_init(&video_pts, 90000ULL * (uint64_t)(unsigned int)fps_den, (uint64_t)(unsigned int)fps_num);
    pts_gen_init(&audio_pts,
                 90000ULL * (uint64_t)(unsigned int)adts_info.samples_per_frame,
                 (uint64_t)(unsigned int)adts_info.sample_rate);

    while (vi < video_aus.count || ai < audio_frames.count) {
        int use_video = 0;

        if (vi < video_aus.count && ai < audio_frames.count) {
            use_video = pts_gen_peek(&video_pts) <= pts_gen_peek(&audio_pts);
        } else if (vi < video_aus.count) {
            use_video = 1;
        }

        if (use_video) {
            const sample_t *au = &video_aus.items[vi];
            int keyframe;

            if (video_codec == HLS_VIDEO_CODEC_H265) {
                keyframe = hls_detect_h265_irap(&video[au->offset], au->size);
            } else {
                keyframe = hls_detect_h264_idr(&video[au->offset], au->size);
            }

            if (hls_muxer_input_video(muxer,
                                      &video[au->offset],
                                      au->size,
                                      pts_gen_peek(&video_pts),
                                      pts_gen_peek(&video_pts),
                                      keyframe) != HLS_OK) {
                fprintf(stderr, "hls_muxer_input_video failed at sample %zu\n", vi);
                goto done;
            }

            pts_gen_advance(&video_pts);
            vi += 1;
        } else {
            const sample_t *frame = &audio_frames.items[ai];

            if (hls_muxer_input_audio(muxer,
                                      &audio[frame->offset],
                                      frame->size,
                                      pts_gen_peek(&audio_pts)) != HLS_OK) {
                fprintf(stderr, "hls_muxer_input_audio failed at frame %zu\n", ai);
                goto done;
            }

            pts_gen_advance(&audio_pts);
            ai += 1;
        }
    }

    hls_muxer_close(muxer, 1);
    muxer = NULL;
    rc = 0;

done:
    if (muxer != NULL) {
        hls_muxer_close(muxer, 1);
    }
    sample_list_free(&video_aus);
    sample_list_free(&audio_frames);
    free(video);
    free(audio);
    return rc;
}
