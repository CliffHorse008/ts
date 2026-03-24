#ifndef HLS_MUXER_H
#define HLS_MUXER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HLS_VIDEO_CODEC_NONE = 0,
    HLS_VIDEO_CODEC_H264 = 1,
    HLS_VIDEO_CODEC_H265 = 2
} hls_video_codec_t;

typedef enum {
    HLS_AUDIO_CODEC_NONE = 0,
    HLS_AUDIO_CODEC_AAC = 1,
    HLS_AUDIO_CODEC_MP3 = 2,
    HLS_AUDIO_CODEC_G711A = 3,
    HLS_AUDIO_CODEC_G711U = 4
} hls_audio_codec_t;

typedef enum {
    HLS_OK = 0,
    HLS_ERR_ARG = -1,
    HLS_ERR_IO = -2,
    HLS_ERR_STATE = -3,
    HLS_ERR_UNSUPPORTED = -4
} hls_result_t;

typedef struct {
    int profile;
    int sampling_frequency_index;
    int sample_rate;
    int channel_config;
    int samples_per_frame;
    int frame_length;
} hls_adts_info_t;

typedef enum {
    HLS_MUXER_EVENT_SEGMENT_READY = 0,
    HLS_MUXER_EVENT_PLAYLIST_UPDATED = 1
} hls_muxer_event_type_t;

typedef void (*hls_muxer_event_cb)(void *opaque,
                                   hls_muxer_event_type_t event,
                                   const char *path);

typedef struct {
    const char *output_dir;
    const char *playlist_name;
    const char *segment_prefix;
    uint32_t target_duration_sec;
    uint32_t playlist_length;
    hls_video_codec_t video_codec;
    hls_audio_codec_t audio_codec;
    hls_muxer_event_cb on_event;
    void *event_opaque;
} hls_muxer_config_t;

typedef struct hls_muxer hls_muxer_t;

hls_result_t hls_muxer_open(hls_muxer_t **muxer, const hls_muxer_config_t *config);
void hls_muxer_close(hls_muxer_t *muxer, int end_list);

hls_result_t hls_muxer_input_video(hls_muxer_t *muxer,
                                   const uint8_t *data,
                                   size_t size,
                                   uint64_t pts90k,
                                   uint64_t dts90k,
                                   int keyframe);

hls_result_t hls_muxer_input_audio(hls_muxer_t *muxer,
                                   const uint8_t *data,
                                   size_t size,
                                   uint64_t pts90k);

int hls_detect_h264_idr(const uint8_t *data, size_t size);
int hls_detect_h265_irap(const uint8_t *data, size_t size);
int hls_parse_adts(const uint8_t *data, size_t size, hls_adts_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
