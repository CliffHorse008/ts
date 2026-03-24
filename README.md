# Minimal HLS Muxer

一个面向嵌入式的最小 HLS/TS 框架，纯 C 实现，不依赖第三方库。

## 当前能力

- 输入 H.264 Annex-B 访问单元
- 输入 H.265 Annex-B 访问单元
- 输入 AAC ADTS 音频帧
- 输入 G711A 原始音频帧
- 输入 G711U 原始音频帧
- MPEG-TS 复用
- 每隔 2 秒在关键帧处切一片 `.ts`
- 默认在内存中生成 `.ts` / `m3u8`
- 可选调试落盘，实时更新直播 `m3u8`

## 设计边界

- 不做解码，不做转码
- 不依赖文件系统监控、线程库、容器库
- 上层负责提供已经切好的视频访问单元和音频帧
- 上层负责提供 `PTS/DTS`，单位固定为 `90kHz`
- 默认输出保存在内存里，通过回调通知上层取用并上传/分发
- 仅在 `debug_write_files = 1` 时才会落盘到 `output_dir`

## 核心接口

头文件见 [include/hls_muxer.h](/home/public/code/ts/include/hls_muxer.h)。

典型流程：

1. `hls_muxer_open()`
2. 连续调用 `hls_muxer_input_video()` / `hls_muxer_input_audio()`
3. `hls_muxer_close()`

如需让上层在内容更新后立刻取用，可在 `hls_muxer_config_t` 中填写：

- `debug_write_files`
- `on_event`
- `event_opaque`

当前会触发两类事件：

- `HLS_MUXER_EVENT_SEGMENT_READY`：一个 `.ts` 已经生成完毕
- `HLS_MUXER_EVENT_PLAYLIST_UPDATED`：`m3u8` 已重写完成

事件回调里会直接带上：

- `name`
- `data`
- `size`
- `sequence`

触发顺序是先 `.ts`，后 `m3u8`，这样上层在收到 playlist 更新通知时，相关 segment 已可上传。

如果设置了 `debug_write_files = 1`：

- `segment` 会额外写到 `output_dir/<name>`
- `playlist` 会额外写到 `output_dir/<playlist_name>`

## 视频切片策略

- 当收到关键帧且当前分片时长达到 `target_duration_sec` 时切片
- H.264 关键帧检测：NAL type `5`
- H.265 关键帧检测：IRAP NAL type `16..21`

这意味着如果编码器 GOP 很长，实际分片时长可能大于 2 秒。直播场景建议编码器关键帧周期也设置为 2 秒。

## 音频

- AAC：输入带 ADTS 头的完整 AAC 帧，TS 内按 AAC PES 写入
- G711A / G711U：输入原始 G711 帧，TS 内按 private stream PES 写入，并在 PMT 中带 `ALAW` / `ULAW` registration descriptor

## AAC

- 支持 ADTS 头解析辅助函数 `hls_parse_adts()`
- 当前 TS 复用直接写入 AAC ADTS 帧，适合最小实现

## G711

- 适合 IPC/嵌入式实时音频场景
- 建议上层按固定时长喂帧，例如 `20ms` 一帧
- 例如 8kHz 单声道下：
  - G711 每采样 8bit
  - `20ms` 一帧大小通常是 `160 bytes`
  - 相邻音频帧 `PTS` 增量通常是 `1800`（`90000 * 20 / 1000`）

需要注意：G711 在 HLS 播放器上的兼容性明显不如 AAC。这个实现解决的是“TS/HLS 最小封装输出”，不是保证所有播放器都能播。

## 编译

```sh
cmake -S . -B build
cmake --build build
```

## 测试

```sh
ctest --test-dir build --output-on-failure
```

当前包含一个集成测试：使用仓库内硬编码的 H.264 IDR AU 和 AAC ADTS 帧，验证 `m3u8` 与 `.ts` 分片是否按预期生成。

## H.265 + AAC 一键验证

仓库内保存了一份本地生成的样本：

- `testdata/h265_aac/sample_h265.hevc`
- `testdata/h265_aac/sample_aac.aac`

一键执行验证：

```sh
./scripts/verify_saved_h265_aac.sh
```

这个命令会：

- 构建 `offline_mux`
- 用保存好的 H.265 Annex-B 和 AAC ADTS 样本生成 HLS
- 用 `ffprobe` 检查 `m3u8`
- 用 `ffmpeg` 完整解复用和解码，确认输出有效

## 示例

```sh
./build/demo input_video.au input_audio.aac out
```

示例程序为了便于本地查看结果，显式开启了 `debug_write_files = 1`。生产接入时，如果你只想把内存中的 `.ts` / `m3u8` 上传给别的系统，不需要打开这个选项。

## 建议的上层接入方式

- 视频：编码器每输出一个 AU，就调用一次 `hls_muxer_input_video()`
- 音频：每输出一个 AAC ADTS 帧，或一个 G711A/G711U 原始帧，就调用一次 `hls_muxer_input_audio()`
- 时间戳：统一换算成 `90kHz`
- 关键帧：建议上层直接传入，避免重复扫描码流；如果暂时拿不到，可以给 `hls_muxer_input_video()` 传 `keyframe = -1`，内部会自动检测

## 后续可扩展点

- 为 PMT 增加 AAC `AudioSpecificConfig` 描述
- 增加 MP3 的更完整 PMT 描述
- 增加旧分片自动删除
- 增加断电恢复与文件原子更新
- 增加多路节目或独立音视频 PID 配置
