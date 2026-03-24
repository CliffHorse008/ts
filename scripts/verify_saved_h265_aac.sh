#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
OUTPUT_DIR="${BUILD_DIR}/verify_saved_h265_aac"
VIDEO_SAMPLE="${ROOT_DIR}/testdata/h265_aac/sample_h265.hevc"
AUDIO_SAMPLE="${ROOT_DIR}/testdata/h265_aac/sample_aac.aac"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --target offline_mux

rm -rf "${OUTPUT_DIR}"
"${BUILD_DIR}/offline_mux" "${VIDEO_SAMPLE}" "${AUDIO_SAMPLE}" "${OUTPUT_DIR}" 25 1

ffprobe -hide_banner -loglevel error "${OUTPUT_DIR}/live.m3u8" >/dev/null
ffmpeg -v error -i "${OUTPUT_DIR}/live.m3u8" -f null - >/dev/null

echo "OK: verified H.265 + AAC HLS output"
echo "Playlist: ${OUTPUT_DIR}/live.m3u8"
