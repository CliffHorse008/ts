#include "hls_muxer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TS_PACKET_SIZE 188
#define TS_SYNC_BYTE 0x47

#define HLS_MAX_PATH 512
#define HLS_MAX_SEGMENTS 32

#define HLS_PMT_PID 0x1000
#define HLS_VIDEO_PID 0x0100
#define HLS_AUDIO_PID 0x0101
#define HLS_PCR_PID HLS_VIDEO_PID

#ifndef HLS_MUXER_DEBUG_LOG
#define HLS_MUXER_DEBUG_LOG 1
#endif

#if HLS_MUXER_DEBUG_LOG
#define HLS_DEBUGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define HLS_DEBUGF(...) ((void)0)
#endif

typedef struct {
    char file_name[128];
    double duration_sec;
    uint32_t sequence;
} hls_segment_info_t;

struct hls_muxer {
    hls_muxer_config_t cfg;
    FILE *segment_fp;
    FILE *playlist_fp;
    char playlist_path[HLS_MAX_PATH];
    char segment_path[HLS_MAX_PATH];

    uint8_t cc_pat;
    uint8_t cc_pmt;
    uint8_t cc_video;
    uint8_t cc_audio;

    uint64_t segment_start_pts90k;
    uint64_t current_segment_max_pts90k;
    uint64_t first_pts90k;
    uint64_t last_pts90k;
    uint32_t segment_sequence;
    uint32_t media_sequence;
    int has_open_segment;
    int started_streaming;
    int saw_video;
    int saw_audio;
    int got_first_pts;

    hls_segment_info_t segments[HLS_MAX_SEGMENTS];
    uint32_t segment_count;
};

static const int g_adts_sample_rates[] = {
    96000, 88200, 64000, 48000,
    44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,
    7350
};

static int hls_mkdir_if_needed(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    if (mkdir(path, 0755) == 0) {
        return 0;
    }

    return errno == EEXIST ? 0 : -1;
}

static uint32_t hls_crc32_mpeg(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xffffffffu;
    size_t i;

    for (i = 0; i < size; ++i) {
        int bit;
        crc ^= (uint32_t)data[i] << 24;
        for (bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000u) {
                crc = (crc << 1) ^ 0x04c11db7u;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

static void hls_write_pts(uint8_t *dst, uint8_t fb, uint64_t pts90k)
{
    uint64_t v = pts90k & 0x1ffffffffULL;
    uint64_t fb_bits = (((uint64_t)fb) & 0x0fULL) << 4;

    dst[0] = (uint8_t)(fb_bits | (((v >> 30) & 0x07ULL) << 1) | 1ULL);
    dst[1] = (uint8_t)(v >> 22);
    dst[2] = (uint8_t)((((v >> 15) & 0x7fULL) << 1) | 1ULL);
    dst[3] = (uint8_t)(v >> 7);
    dst[4] = (uint8_t)((((v >> 0) & 0x7fULL) << 1) | 1ULL);
}

static void hls_write_pcr(uint8_t *dst, uint64_t pcr90k)
{
    uint64_t base = pcr90k & 0x1ffffffffULL;

    dst[0] = (uint8_t)(base >> 25);
    dst[1] = (uint8_t)(base >> 17);
    dst[2] = (uint8_t)(base >> 9);
    dst[3] = (uint8_t)(base >> 1);
    dst[4] = (uint8_t)(((base & 1) << 7) | 0x7e);
    dst[5] = 0x00;
}

static uint8_t hls_stream_type_video(hls_video_codec_t codec)
{
    switch (codec) {
    case HLS_VIDEO_CODEC_H264:
        return 0x1b;
    case HLS_VIDEO_CODEC_H265:
        return 0x24;
    default:
        return 0x00;
    }
}

static uint8_t hls_stream_type_audio(hls_audio_codec_t codec)
{
    switch (codec) {
    case HLS_AUDIO_CODEC_AAC:
        return 0x0f;
    case HLS_AUDIO_CODEC_MP3:
        return 0x03;
    case HLS_AUDIO_CODEC_G711A:
    case HLS_AUDIO_CODEC_G711U:
        return 0x06;
    default:
        return 0x00;
    }
}

static uint8_t hls_audio_stream_id(hls_audio_codec_t codec)
{
    switch (codec) {
    case HLS_AUDIO_CODEC_G711A:
    case HLS_AUDIO_CODEC_G711U:
        return 0xbd;
    case HLS_AUDIO_CODEC_AAC:
    case HLS_AUDIO_CODEC_MP3:
    default:
        return 0xc0;
    }
}

static int hls_write_audio_descriptor(uint8_t *dst, hls_audio_codec_t codec)
{
    if (codec == HLS_AUDIO_CODEC_G711A) {
        dst[0] = 0x05;
        dst[1] = 4;
        dst[2] = 'A';
        dst[3] = 'L';
        dst[4] = 'A';
        dst[5] = 'W';
        return 6;
    }

    if (codec == HLS_AUDIO_CODEC_G711U) {
        dst[0] = 0x05;
        dst[1] = 4;
        dst[2] = 'U';
        dst[3] = 'L';
        dst[4] = 'A';
        dst[5] = 'W';
        return 6;
    }

    return 0;
}

static void hls_notify_event(hls_muxer_t *m,
                             hls_muxer_event_type_t event,
                             const char *path)
{
    if (m->cfg.on_event != NULL && path != NULL) {
        m->cfg.on_event(m->cfg.event_opaque, event, path);
    }
}

static hls_result_t hls_playlist_write(hls_muxer_t *m, int end_list)
{
    FILE *fp;
    uint32_t i;
    uint32_t target = m->cfg.target_duration_sec;

    fp = fopen(m->playlist_path, "wb");
    if (fp == NULL) {
        return HLS_ERR_IO;
    }

    for (i = 0; i < m->segment_count; ++i) {
        uint32_t dur = (uint32_t)(m->segments[i].duration_sec + 0.999);
        if (dur > target) {
            target = dur;
        }
    }

    fprintf(fp, "#EXTM3U\n");
    fprintf(fp, "#EXT-X-VERSION:3\n");
    fprintf(fp, "#EXT-X-TARGETDURATION:%u\n", target ? target : 2);
    fprintf(fp, "#EXT-X-MEDIA-SEQUENCE:%u\n", m->media_sequence);

    for (i = 0; i < m->segment_count; ++i) {
        fprintf(fp, "#EXTINF:%.3f,\n", m->segments[i].duration_sec);
        fprintf(fp, "%s\n", m->segments[i].file_name);
    }

    if (end_list) {
        fprintf(fp, "#EXT-X-ENDLIST\n");
    }

    fclose(fp);
    hls_notify_event(m, HLS_MUXER_EVENT_PLAYLIST_UPDATED, m->playlist_path);
    return HLS_OK;
}

static hls_result_t hls_open_segment(hls_muxer_t *m, uint64_t start_pts90k)
{
    if (snprintf(m->segment_path,
                 sizeof(m->segment_path),
                 "%s/%s%05u.ts",
                 m->cfg.output_dir,
                 m->cfg.segment_prefix,
                 m->segment_sequence) >= (int)sizeof(m->segment_path)) {
        return HLS_ERR_ARG;
    }

    m->segment_fp = fopen(m->segment_path, "wb");
    if (m->segment_fp == NULL) {
        return HLS_ERR_IO;
    }

    m->segment_start_pts90k = start_pts90k;
    m->current_segment_max_pts90k = start_pts90k;
    m->has_open_segment = 1;
    return HLS_OK;
}

static void hls_append_segment_info(hls_muxer_t *m, double duration_sec)
{
    hls_segment_info_t info;

    memset(&info, 0, sizeof(info));
    snprintf(info.file_name, sizeof(info.file_name), "%s%05u.ts",
             m->cfg.segment_prefix, m->segment_sequence);
    info.duration_sec = duration_sec;
    info.sequence = m->segment_sequence;

    if (m->segment_count < m->cfg.playlist_length) {
        m->segments[m->segment_count++] = info;
    } else {
        memmove(&m->segments[0], &m->segments[1],
                sizeof(m->segments[0]) * (m->cfg.playlist_length - 1));
        m->segments[m->cfg.playlist_length - 1] = info;
        m->media_sequence = m->segments[0].sequence;
    }

    if (m->segment_count == 1) {
        m->media_sequence = info.sequence;
    }
}

static hls_result_t hls_close_segment(hls_muxer_t *m, int end_list)
{
    double duration_sec;

    if (!m->has_open_segment) {
        return HLS_OK;
    }

    if (m->segment_fp != NULL) {
        fclose(m->segment_fp);
        m->segment_fp = NULL;
    }

    hls_notify_event(m, HLS_MUXER_EVENT_SEGMENT_READY, m->segment_path);

    duration_sec = (double)(m->current_segment_max_pts90k - m->segment_start_pts90k) / 90000.0;
    if (duration_sec <= 0.0) {
        duration_sec = (double)m->cfg.target_duration_sec;
    }

    hls_append_segment_info(m, duration_sec);
    m->segment_sequence += 1;
    m->has_open_segment = 0;
    return hls_playlist_write(m, end_list);
}

static void hls_write_pat(hls_muxer_t *m)
{
    uint8_t pkt[TS_PACKET_SIZE];
    uint8_t sec[1024];
    uint32_t crc;
    size_t sec_len;

    memset(pkt, 0xff, sizeof(pkt));
    memset(sec, 0, sizeof(sec));

    sec[0] = 0x00;
    sec[1] = 0xb0;
    sec[2] = 0x0d;
    sec[3] = 0x00;
    sec[4] = 0x01;
    sec[5] = 0xc1;
    sec[6] = 0x00;
    sec[7] = 0x00;
    sec[8] = 0x00;
    sec[9] = 0x01;
    sec[10] = (uint8_t)(0xe0 | ((HLS_PMT_PID >> 8) & 0x1f));
    sec[11] = (uint8_t)(HLS_PMT_PID & 0xff);
    crc = hls_crc32_mpeg(sec, 12);
    sec[12] = (uint8_t)(crc >> 24);
    sec[13] = (uint8_t)(crc >> 16);
    sec[14] = (uint8_t)(crc >> 8);
    sec[15] = (uint8_t)crc;
    sec_len = 16;

    pkt[0] = TS_SYNC_BYTE;
    pkt[1] = 0x40;
    pkt[2] = 0x00;
    pkt[3] = 0x10 | (m->cc_pat++ & 0x0f);
    pkt[4] = 0x00;
    memcpy(&pkt[5], sec, sec_len);

    fwrite(pkt, 1, sizeof(pkt), m->segment_fp);
}

static void hls_write_pmt(hls_muxer_t *m)
{
    uint8_t pkt[TS_PACKET_SIZE];
    uint8_t sec[1024];
    int pos = 0;
    int desc_len;
    uint32_t crc;
    int pcr_pid;

    memset(pkt, 0xff, sizeof(pkt));
    memset(sec, 0, sizeof(sec));

    pcr_pid = m->cfg.video_codec != HLS_VIDEO_CODEC_NONE ? HLS_PCR_PID : HLS_AUDIO_PID;

    sec[pos++] = 0x02;
    sec[pos++] = 0xb0;
    sec[pos++] = 0x00;
    sec[pos++] = 0x00;
    sec[pos++] = 0x01;
    sec[pos++] = 0xc1;
    sec[pos++] = 0x00;
    sec[pos++] = 0x00;
    sec[pos++] = (uint8_t)(0xe0 | ((pcr_pid >> 8) & 0x1f));
    sec[pos++] = (uint8_t)(pcr_pid & 0xff);
    sec[pos++] = 0xf0;
    sec[pos++] = 0x00;

    if (m->cfg.video_codec != HLS_VIDEO_CODEC_NONE) {
        sec[pos++] = hls_stream_type_video(m->cfg.video_codec);
        sec[pos++] = (uint8_t)(0xe0 | ((HLS_VIDEO_PID >> 8) & 0x1f));
        sec[pos++] = (uint8_t)(HLS_VIDEO_PID & 0xff);
        sec[pos++] = 0xf0;
        sec[pos++] = 0x00;
    }

    if (m->cfg.audio_codec != HLS_AUDIO_CODEC_NONE) {
        sec[pos++] = hls_stream_type_audio(m->cfg.audio_codec);
        sec[pos++] = (uint8_t)(0xe0 | ((HLS_AUDIO_PID >> 8) & 0x1f));
        sec[pos++] = (uint8_t)(HLS_AUDIO_PID & 0xff);
        desc_len = hls_write_audio_descriptor(&sec[pos + 2], m->cfg.audio_codec);
        sec[pos++] = (uint8_t)(0xf0 | ((desc_len >> 8) & 0x0f));
        sec[pos++] = (uint8_t)(desc_len & 0xff);
        pos += desc_len;
    }

    sec[1] = (uint8_t)(0xb0 | (((pos + 4 - 3) >> 8) & 0x0f));
    sec[2] = (uint8_t)((pos + 4 - 3) & 0xff);

    crc = hls_crc32_mpeg(sec, (size_t)pos);
    sec[pos++] = (uint8_t)(crc >> 24);
    sec[pos++] = (uint8_t)(crc >> 16);
    sec[pos++] = (uint8_t)(crc >> 8);
    sec[pos++] = (uint8_t)crc;

    pkt[0] = TS_SYNC_BYTE;
    pkt[1] = (uint8_t)(0x40 | ((HLS_PMT_PID >> 8) & 0x1f));
    pkt[2] = (uint8_t)(HLS_PMT_PID & 0xff);
    pkt[3] = 0x10 | (m->cc_pmt++ & 0x0f);
    pkt[4] = 0x00;
    memcpy(&pkt[5], sec, (size_t)pos);

    fwrite(pkt, 1, sizeof(pkt), m->segment_fp);
}

static void hls_write_tables(hls_muxer_t *m)
{
    hls_write_pat(m);
    hls_write_pmt(m);
}

static hls_result_t hls_rotate_if_needed(hls_muxer_t *m, uint64_t pts90k, int keyframe)
{
    uint64_t elapsed90k;
    hls_result_t rc;

    if (!m->has_open_segment) {
        if (!m->started_streaming &&
            m->cfg.video_codec != HLS_VIDEO_CODEC_NONE &&
            !keyframe) {
            return HLS_OK;
        }

        rc = hls_open_segment(m, pts90k);
        if (rc != HLS_OK) {
            return rc;
        }
        m->started_streaming = 1;
        hls_write_tables(m);
        return HLS_OK;
    }

    elapsed90k = pts90k - m->segment_start_pts90k;
    if (keyframe && elapsed90k >= (uint64_t)m->cfg.target_duration_sec * 90000ULL) {
        rc = hls_close_segment(m, 0);
        if (rc != HLS_OK) {
            return rc;
        }
        rc = hls_open_segment(m, pts90k);
        if (rc != HLS_OK) {
            return rc;
        }
        hls_write_tables(m);
    }

    return HLS_OK;
}

static hls_result_t hls_write_ts_payload(hls_muxer_t *m,
                                         uint16_t pid,
                                         uint8_t *cc,
                                         const uint8_t *payload,
                                         size_t payload_len,
                                         int payload_unit_start,
                                         int insert_pcr,
                                         uint64_t pcr90k)
{
    size_t off = 0;

    while (off < payload_len || (payload_len == 0 && off == 0)) {
        uint8_t pkt[TS_PACKET_SIZE];
        size_t remain = payload_len - off;
        size_t max_payload = 184;
        size_t copy_len;
        int adapt = 1;
        int payload_start = payload_unit_start && off == 0;
        size_t idx = 4;

        memset(pkt, 0xff, sizeof(pkt));
        pkt[0] = TS_SYNC_BYTE;
        pkt[1] = (uint8_t)(((payload_start ? 0x40 : 0x00)) | ((pid >> 8) & 0x1f));
        pkt[2] = (uint8_t)(pid & 0xff);

        if (insert_pcr && off == 0) {
            size_t stuffing = remain < 176 ? 176 - remain : 0;

            pkt[3] = 0x30 | (*cc & 0x0f);
            pkt[4] = (uint8_t)(7 + stuffing);
            pkt[5] = 0x10;
            hls_write_pcr(&pkt[6], pcr90k);
            if (stuffing > 0) {
                memset(&pkt[12], 0xff, stuffing);
            }
            idx = 12 + stuffing;
        } else if (remain < 184) {
            size_t stuffing = 184 - remain;
            if (stuffing == 1) {
                pkt[3] = 0x30 | (*cc & 0x0f);
                pkt[4] = 0x00;
                idx = 5;
            } else {
                pkt[3] = 0x30 | (*cc & 0x0f);
                pkt[4] = (uint8_t)(stuffing - 1);
                pkt[5] = 0x00;
                if (stuffing > 2) {
                    memset(&pkt[6], 0xff, stuffing - 2);
                }
                idx = 4 + stuffing;
            }
        } else {
            adapt = 0;
            pkt[3] = 0x10 | (*cc & 0x0f);
        }

        if (adapt == 0) {
            max_payload = 184;
        } else if (insert_pcr && off == 0) {
            max_payload = 176;
        } else {
            max_payload = TS_PACKET_SIZE - idx;
        }

        copy_len = remain < max_payload ? remain : max_payload;
        if (copy_len > 0) {
            memcpy(&pkt[idx], &payload[off], copy_len);
        }

        if (off == 0) {
            uint8_t b0 = copy_len > 0 ? payload[0] : 0;
            uint8_t b1 = copy_len > 1 ? payload[1] : 0;
            uint8_t b2 = copy_len > 2 ? payload[2] : 0;
            uint8_t b3 = copy_len > 3 ? payload[3] : 0;

            HLS_DEBUGF("[hls] ts pid=0x%04x cc=%u start=%d insert_pcr=%d payload_len=%zu remain=%zu idx=%zu copy_len=%zu bytes=%02x %02x %02x %02x\n",
                       (unsigned int)pid,
                       (unsigned int)(*cc & 0x0f),
                       payload_start,
                       insert_pcr,
                       payload_len,
                       remain,
                       idx,
                       copy_len,
                       (unsigned int)b0,
                       (unsigned int)b1,
                       (unsigned int)b2,
                       (unsigned int)b3);
        }

        fwrite(pkt, 1, sizeof(pkt), m->segment_fp);
        *cc = (uint8_t)((*cc + 1) & 0x0f);
        off += copy_len;

        if (payload_len == 0) {
            break;
        }
    }

    return HLS_OK;
}

static hls_result_t hls_write_pes(hls_muxer_t *m,
                                  uint16_t pid,
                                  uint8_t *cc,
                                  uint8_t stream_id,
                                  const uint8_t *es,
                                  size_t es_size,
                                  uint64_t pts90k,
                                  uint64_t dts90k,
                                  int write_dts,
                                  int write_pcr)
{
    uint8_t hdr[32];
    size_t hdr_len = 0;
    size_t pes_packet_length;
    uint8_t pts_dts_flags;
    hls_result_t rc;

    hdr[hdr_len++] = 0x00;
    hdr[hdr_len++] = 0x00;
    hdr[hdr_len++] = 0x01;
    hdr[hdr_len++] = stream_id;

    pes_packet_length = es_size + 8 + (write_dts ? 5 : 0);
    if (pes_packet_length > 0xffff) {
        hdr[hdr_len++] = 0x00;
        hdr[hdr_len++] = 0x00;
    } else {
        hdr[hdr_len++] = (uint8_t)(pes_packet_length >> 8);
        hdr[hdr_len++] = (uint8_t)(pes_packet_length & 0xff);
    }

    hdr[hdr_len++] = 0x80;
    pts_dts_flags = write_dts ? 0xc0 : 0x80;
    hdr[hdr_len++] = pts_dts_flags;
    hdr[hdr_len++] = (uint8_t)(write_dts ? 10 : 5);
    hls_write_pts(&hdr[hdr_len], write_dts ? 0x03 : 0x02, pts90k);
    hdr_len += 5;
    if (write_dts) {
        hls_write_pts(&hdr[hdr_len], 0x01, dts90k);
        hdr_len += 5;
    }

    HLS_DEBUGF("[hls] pes pid=0x%04x sid=0x%02x es_size=%zu hdr_len=%zu pts=%llu dts=%llu write_dts=%d write_pcr=%d hdr=%02x %02x %02x %02x %02x %02x %02x %02x\n",
               (unsigned int)pid,
               (unsigned int)stream_id,
               es_size,
               hdr_len,
               (unsigned long long)pts90k,
               (unsigned long long)dts90k,
               write_dts,
               write_pcr,
               (unsigned int)hdr[0],
               (unsigned int)hdr[1],
               (unsigned int)hdr[2],
               (unsigned int)hdr[3],
               (unsigned int)hdr[4],
               (unsigned int)hdr[5],
               (unsigned int)hdr[6],
               (unsigned int)hdr[7]);

    rc = hls_write_ts_payload(m, pid, cc, hdr, hdr_len, 1, write_pcr, dts90k);
    if (rc != HLS_OK) {
        return rc;
    }

    return hls_write_ts_payload(m, pid, cc, es, es_size, 0, 0, 0);
}

static int hls_find_start_code(const uint8_t *data, size_t size, size_t off, size_t *code_off, size_t *code_size)
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

int hls_detect_h264_idr(const uint8_t *data, size_t size)
{
    size_t off = 0;

    while (off < size) {
        size_t sc_off;
        size_t sc_size;
        uint8_t nal_type;

        if (!hls_find_start_code(data, size, off, &sc_off, &sc_size)) {
            break;
        }

        if (sc_off + sc_size >= size) {
            break;
        }

        nal_type = data[sc_off + sc_size] & 0x1f;
        if (nal_type == 5) {
            return 1;
        }

        off = sc_off + sc_size + 1;
    }

    return 0;
}

int hls_detect_h265_irap(const uint8_t *data, size_t size)
{
    size_t off = 0;

    while (off < size) {
        size_t sc_off;
        size_t sc_size;
        uint8_t nal_type;

        if (!hls_find_start_code(data, size, off, &sc_off, &sc_size)) {
            break;
        }

        if (sc_off + sc_size + 1 >= size) {
            break;
        }

        nal_type = (uint8_t)((data[sc_off + sc_size] >> 1) & 0x3f);
        if (nal_type >= 16 && nal_type <= 21) {
            return 1;
        }

        off = sc_off + sc_size + 2;
    }

    return 0;
}

int hls_parse_adts(const uint8_t *data, size_t size, hls_adts_info_t *info)
{
    int sf_index;

    if (data == NULL || size < 7 || info == NULL) {
        return 0;
    }

    if (data[0] != 0xff || (data[1] & 0xf0) != 0xf0) {
        return 0;
    }

    sf_index = (data[2] >> 2) & 0x0f;
    if (sf_index < 0 || sf_index >= (int)(sizeof(g_adts_sample_rates) / sizeof(g_adts_sample_rates[0]))) {
        return 0;
    }

    memset(info, 0, sizeof(*info));
    info->profile = ((data[2] >> 6) & 0x03) + 1;
    info->sampling_frequency_index = sf_index;
    info->sample_rate = g_adts_sample_rates[sf_index];
    info->channel_config = ((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03);
    info->samples_per_frame = 1024;
    info->frame_length = ((data[3] & 0x03) << 11) | (data[4] << 3) | ((data[5] >> 5) & 0x07);
    return 1;
}

hls_result_t hls_muxer_open(hls_muxer_t **muxer, const hls_muxer_config_t *config)
{
    hls_muxer_t *m;

    if (muxer == NULL || config == NULL || config->output_dir == NULL ||
        config->playlist_name == NULL || config->segment_prefix == NULL) {
        return HLS_ERR_ARG;
    }

    if (config->playlist_length == 0 || config->playlist_length > HLS_MAX_SEGMENTS) {
        return HLS_ERR_ARG;
    }

    if (config->target_duration_sec == 0) {
        return HLS_ERR_ARG;
    }

    if (config->video_codec == HLS_VIDEO_CODEC_NONE &&
        config->audio_codec == HLS_AUDIO_CODEC_NONE) {
        return HLS_ERR_ARG;
    }

    if (config->audio_codec != HLS_AUDIO_CODEC_NONE &&
        config->audio_codec != HLS_AUDIO_CODEC_AAC &&
        config->audio_codec != HLS_AUDIO_CODEC_MP3 &&
        config->audio_codec != HLS_AUDIO_CODEC_G711A &&
        config->audio_codec != HLS_AUDIO_CODEC_G711U) {
        return HLS_ERR_UNSUPPORTED;
    }

    if (hls_mkdir_if_needed(config->output_dir) != 0) {
        return HLS_ERR_IO;
    }

    m = (hls_muxer_t *)calloc(1, sizeof(*m));
    if (m == NULL) {
        return HLS_ERR_IO;
    }

    m->cfg = *config;
    m->segment_sequence = 0;
    m->media_sequence = 0;

    if (snprintf(m->playlist_path,
                 sizeof(m->playlist_path),
                 "%s/%s",
                 config->output_dir,
                 config->playlist_name) >= (int)sizeof(m->playlist_path)) {
        free(m);
        return HLS_ERR_ARG;
    }

    *muxer = m;
    return hls_playlist_write(m, 0);
}

void hls_muxer_close(hls_muxer_t *muxer, int end_list)
{
    if (muxer == NULL) {
        return;
    }

    (void)hls_close_segment(muxer, end_list);
    free(muxer);
}

hls_result_t hls_muxer_input_video(hls_muxer_t *m,
                                   const uint8_t *data,
                                   size_t size,
                                   uint64_t pts90k,
                                   uint64_t dts90k,
                                   int keyframe)
{
    hls_result_t rc;

    if (m == NULL || data == NULL || size == 0 || m->cfg.video_codec == HLS_VIDEO_CODEC_NONE) {
        return HLS_ERR_ARG;
    }

    if (keyframe < 0) {
        if (m->cfg.video_codec == HLS_VIDEO_CODEC_H264) {
            keyframe = hls_detect_h264_idr(data, size);
        } else if (m->cfg.video_codec == HLS_VIDEO_CODEC_H265) {
            keyframe = hls_detect_h265_irap(data, size);
        } else {
            keyframe = 0;
        }
    }

    if (!m->started_streaming && !keyframe) {
        HLS_DEBUGF("[hls] drop video before first keyframe size=%zu pts=%llu dts=%llu\n",
                   size,
                   (unsigned long long)pts90k,
                   (unsigned long long)dts90k);
        return HLS_OK;
    }

    rc = hls_rotate_if_needed(m, pts90k, keyframe);
    if (rc != HLS_OK) {
        return rc;
    }

    if (!m->got_first_pts) {
        m->first_pts90k = pts90k;
        m->got_first_pts = 1;
    }

    HLS_DEBUGF("[hls] input video size=%zu pts=%llu dts=%llu keyframe=%d cc=%u\n",
               size,
               (unsigned long long)pts90k,
               (unsigned long long)dts90k,
               keyframe,
               (unsigned int)(m->cc_video & 0x0f));

    rc = hls_write_pes(m,
                       HLS_VIDEO_PID,
                       &m->cc_video,
                       0xe0,
                       data,
                       size,
                       pts90k,
                       dts90k,
                       pts90k != dts90k,
                       1);
    if (rc != HLS_OK) {
        return rc;
    }

    m->saw_video = 1;
    m->last_pts90k = pts90k;
    if (pts90k > m->current_segment_max_pts90k) {
        m->current_segment_max_pts90k = pts90k;
    }
    return HLS_OK;
}

hls_result_t hls_muxer_input_audio(hls_muxer_t *m,
                                   const uint8_t *data,
                                   size_t size,
                                   uint64_t pts90k)
{
    hls_result_t rc;

    if (m == NULL || data == NULL || size == 0 || m->cfg.audio_codec == HLS_AUDIO_CODEC_NONE) {
        return HLS_ERR_ARG;
    }

    if (!m->started_streaming && m->cfg.video_codec != HLS_VIDEO_CODEC_NONE) {
        HLS_DEBUGF("[hls] drop audio before first keyframe size=%zu pts=%llu\n",
                   size,
                   (unsigned long long)pts90k);
        return HLS_OK;
    }

    rc = hls_rotate_if_needed(m, pts90k, m->cfg.video_codec == HLS_VIDEO_CODEC_NONE);
    if (rc != HLS_OK) {
        return rc;
    }

    if (!m->got_first_pts) {
        m->first_pts90k = pts90k;
        m->got_first_pts = 1;
    }

    HLS_DEBUGF("[hls] input audio size=%zu pts=%llu cc=%u\n",
               size,
               (unsigned long long)pts90k,
               (unsigned int)(m->cc_audio & 0x0f));

    rc = hls_write_pes(m,
                       HLS_AUDIO_PID,
                       &m->cc_audio,
                       hls_audio_stream_id(m->cfg.audio_codec),
                       data,
                       size,
                       pts90k,
                       pts90k,
                       0,
                       m->cfg.video_codec == HLS_VIDEO_CODEC_NONE);
    if (rc != HLS_OK) {
        return rc;
    }

    m->saw_audio = 1;
    m->last_pts90k = pts90k;
    if (pts90k > m->current_segment_max_pts90k) {
        m->current_segment_max_pts90k = pts90k;
    }
    return HLS_OK;
}
