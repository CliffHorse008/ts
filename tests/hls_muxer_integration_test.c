#include "hls_muxer.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TS_PACKET_SIZE 188
#define TEST_H264_AU_SIZE 320
#define TEST_AAC_FRAME_SIZE 127
#define TEST_MAX_EVENTS 16

typedef struct {
    hls_muxer_event_type_t event;
    char name[512];
    size_t size;
} event_record_t;

typedef struct {
    event_record_t items[TEST_MAX_EVENTS];
    size_t count;
} event_log_t;

static void build_test_h264_idr_au(uint8_t *buf, size_t size)
{
    static const uint8_t prefix[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e, 0xe9, 0x01, 0x40, 0x7b, 0x20,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2,
        0x00, 0x00, 0x00, 0x01, 0x65
    };
    size_t i;

    memset(buf, 0x5a, size);
    memcpy(buf, prefix, sizeof(prefix));

    for (i = sizeof(prefix); i < size; ++i) {
        /* Keep the payload deterministic while avoiding accidental Annex-B start codes. */
        buf[i] = (uint8_t)(0x11 + (i % 199));
        if (buf[i] == 0x00 || buf[i] == 0x01) {
            buf[i] = 0x67;
        }
    }
}

static void build_test_aac_adts_frame(uint8_t *buf, size_t size)
{
    size_t i;

    memset(buf, 0, size);
    buf[0] = 0xff;
    buf[1] = 0xf1;
    buf[2] = 0x50;
    buf[3] = 0x80;
    buf[4] = (uint8_t)((size >> 3) & 0xff);
    buf[5] = (uint8_t)(((size & 0x07) << 5) | 0x1f);
    buf[6] = 0xfc;

    for (i = 7; i < size; ++i) {
        buf[i] = (uint8_t)(0x30 + (i % 97));
    }
}

static int remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);
    if (dir == NULL) {
        return errno == ENOENT ? 0 : -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        char child[512];
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) {
            closedir(dir);
            return -1;
        }

        if (stat(child, &st) != 0) {
            closedir(dir);
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (remove_tree(child) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (unlink(child) != 0) {
            closedir(dir);
            return -1;
        }
    }

    closedir(dir);
    return rmdir(path);
}

static int ensure_clean_dir(const char *path)
{
    if (remove_tree(path) != 0 && errno != ENOENT) {
        return -1;
    }

    return mkdir(path, 0755);
}

static long file_size(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }

    return (long)st.st_size;
}

static int read_text_file(const char *path, char *buf, size_t buf_size)
{
    FILE *fp;
    size_t nread;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    nread = fread(buf, 1, buf_size - 1, fp);
    if (ferror(fp)) {
        fclose(fp);
        return -1;
    }

    buf[nread] = '\0';
    fclose(fp);
    return 0;
}

static int count_occurrences(const char *haystack, const char *needle)
{
    int count = 0;
    const char *p = haystack;

    while ((p = strstr(p, needle)) != NULL) {
        count += 1;
        p += strlen(needle);
    }

    return count;
}

static int playlist_contains_expected_entries(const char *playlist_path)
{
    char buf[4096];

    if (read_text_file(playlist_path, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "failed to read playlist: %s\n", playlist_path);
        return 0;
    }

    if (strstr(buf, "#EXTM3U\n") == NULL ||
        strstr(buf, "#EXT-X-TARGETDURATION:2\n") == NULL ||
        strstr(buf, "#EXT-X-MEDIA-SEQUENCE:0\n") == NULL ||
        strstr(buf, "#EXT-X-ENDLIST\n") == NULL) {
        fprintf(stderr, "playlist missing required headers\n");
        return 0;
    }

    if (count_occurrences(buf, "#EXTINF:2.000,\n") != 3) {
        fprintf(stderr, "playlist has unexpected segment duration entries\n");
        return 0;
    }

    if (strstr(buf, "seg_00000.ts\n") == NULL ||
        strstr(buf, "seg_00001.ts\n") == NULL ||
        strstr(buf, "seg_00002.ts\n") == NULL) {
        fprintf(stderr, "playlist missing expected segment names\n");
        return 0;
    }

    return 1;
}

static int segment_has_sync_packets(const char *segment_path)
{
    FILE *fp;
    uint8_t buf[TS_PACKET_SIZE * 2];
    size_t nread;

    fp = fopen(segment_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "failed to open segment: %s\n", segment_path);
        return 0;
    }

    nread = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    if (nread < sizeof(buf)) {
        fprintf(stderr, "segment too small: %s\n", segment_path);
        return 0;
    }

    if (buf[0] != 0x47 || buf[TS_PACKET_SIZE] != 0x47) {
        fprintf(stderr, "segment missing TS sync bytes: %s\n", segment_path);
        return 0;
    }

    return 1;
}

static void test_event_cb(void *opaque,
                          const hls_muxer_event_t *event)
{
    event_log_t *log = (event_log_t *)opaque;

    if (log == NULL || event == NULL || event->name == NULL || log->count >= TEST_MAX_EVENTS) {
        return;
    }

    log->items[log->count].event = event->type;
    log->items[log->count].size = event->size;
    snprintf(log->items[log->count].name, sizeof(log->items[log->count].name), "%s", event->name);
    log->count += 1;
}

static int event_log_matches(const event_log_t *log, const char *output_dir)
{
    char segment_name[128];
    size_t i;

    (void)output_dir;

    if (log == NULL) {
        return 0;
    }

    if (log->count != 7) {
        fprintf(stderr, "unexpected event count: %zu\n", log->count);
        return 0;
    }

    if (log->items[0].event != HLS_MUXER_EVENT_PLAYLIST_UPDATED ||
        strcmp(log->items[0].name, "live.m3u8") != 0 ||
        log->items[0].size == 0) {
        fprintf(stderr, "unexpected initial playlist event\n");
        return 0;
    }

    for (i = 0; i < 3; ++i) {
        if (snprintf(segment_name, sizeof(segment_name), "seg_%05zu.ts", i) >= (int)sizeof(segment_name)) {
            return 0;
        }

        if (log->items[1 + i * 2].event != HLS_MUXER_EVENT_SEGMENT_READY ||
            strcmp(log->items[1 + i * 2].name, segment_name) != 0 ||
            log->items[1 + i * 2].size == 0) {
            fprintf(stderr, "unexpected segment event at index %zu\n", 1 + i * 2);
            return 0;
        }

        if (log->items[2 + i * 2].event != HLS_MUXER_EVENT_PLAYLIST_UPDATED ||
            strcmp(log->items[2 + i * 2].name, "live.m3u8") != 0 ||
            log->items[2 + i * 2].size == 0) {
            fprintf(stderr, "unexpected playlist event at index %zu\n", 2 + i * 2);
            return 0;
        }
    }

    return 1;
}

static int run_muxer_smoke_test(const char *output_dir)
{
    char playlist_path[512];
    char segment_path[512];
    hls_muxer_t *muxer = NULL;
    hls_muxer_config_t cfg;
    event_log_t event_log;
    hls_adts_info_t adts_info;
    uint8_t h264_idr_au[TEST_H264_AU_SIZE];
    uint8_t aac_adts_frame[TEST_AAC_FRAME_SIZE];
    uint64_t pts90k = 0;
    int i;

    build_test_h264_idr_au(h264_idr_au, sizeof(h264_idr_au));
    build_test_aac_adts_frame(aac_adts_frame, sizeof(aac_adts_frame));
    memset(&event_log, 0, sizeof(event_log));

    if (!hls_detect_h264_idr(h264_idr_au, sizeof(h264_idr_au))) {
        fprintf(stderr, "hardcoded H.264 access unit was not detected as IDR\n");
        return 1;
    }

    if (!hls_parse_adts(aac_adts_frame, sizeof(aac_adts_frame), &adts_info)) {
        fprintf(stderr, "hardcoded AAC frame did not parse as ADTS\n");
        return 1;
    }

    if (adts_info.sample_rate != 44100 || adts_info.channel_config != 2) {
        fprintf(stderr, "unexpected ADTS info: sample_rate=%d channels=%d\n",
                adts_info.sample_rate, adts_info.channel_config);
        return 1;
    }

    if (ensure_clean_dir(output_dir) != 0) {
        fprintf(stderr, "failed to prepare output dir: %s\n", output_dir);
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.output_dir = output_dir;
    cfg.playlist_name = "live.m3u8";
    cfg.segment_prefix = "seg_";
    cfg.target_duration_sec = 2;
    cfg.playlist_length = 6;
    cfg.video_codec = HLS_VIDEO_CODEC_H264;
    cfg.audio_codec = HLS_AUDIO_CODEC_AAC;
    cfg.debug_write_files = 1;
    cfg.on_event = test_event_cb;
    cfg.event_opaque = &event_log;

    if (hls_muxer_open(&muxer, &cfg) != HLS_OK) {
        fprintf(stderr, "hls_muxer_open failed\n");
        return 1;
    }

    for (i = 0; i < 3; ++i) {
        if (hls_muxer_input_video(muxer,
                                  h264_idr_au,
                                  sizeof(h264_idr_au),
                                  pts90k,
                                  pts90k,
                                  -1) != HLS_OK) {
            fprintf(stderr, "hls_muxer_input_video failed at frame %d\n", i);
            hls_muxer_close(muxer, 0);
            return 1;
        }

        if (hls_muxer_input_audio(muxer,
                                  aac_adts_frame,
                                  sizeof(aac_adts_frame),
                                  pts90k) != HLS_OK) {
            fprintf(stderr, "hls_muxer_input_audio failed at frame %d\n", i);
            hls_muxer_close(muxer, 0);
            return 1;
        }

        pts90k += 180000;
    }

    hls_muxer_close(muxer, 1);

    if (snprintf(playlist_path, sizeof(playlist_path), "%s/live.m3u8", output_dir) >= (int)sizeof(playlist_path)) {
        fprintf(stderr, "playlist path too long\n");
        return 1;
    }

    if (!playlist_contains_expected_entries(playlist_path)) {
        return 1;
    }

    if (!event_log_matches(&event_log, output_dir)) {
        return 1;
    }

    for (i = 0; i < 3; ++i) {
        if (snprintf(segment_path, sizeof(segment_path), "%s/seg_%05d.ts", output_dir, i) >= (int)sizeof(segment_path)) {
            fprintf(stderr, "segment path too long\n");
            return 1;
        }

        if (file_size(segment_path) <= TS_PACKET_SIZE * 2) {
            fprintf(stderr, "segment file size too small: %s\n", segment_path);
            return 1;
        }

        if (!segment_has_sync_packets(segment_path)) {
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
        return 1;
    }

    return run_muxer_smoke_test(argv[1]);
}
