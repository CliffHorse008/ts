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
- 实时更新直播 `m3u8`

## 设计边界

- 不做解码，不做转码
- 不依赖文件系统监控、线程库、容器库
- 上层负责提供已经切好的视频访问单元和音频帧
- 上层负责提供 `PTS/DTS`，单位固定为 `90kHz`

## 核心接口

头文件见 [include/hls_muxer.h](/home/public/code/ts/include/hls_muxer.h)。

典型流程：

1. `hls_muxer_open()`
2. 连续调用 `hls_muxer_input_video()` / `hls_muxer_input_audio()`
3. `hls_muxer_close()`

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

## 示例

```sh
./build/demo input_video.au input_audio.aac out
```

示例程序只是演示 API 调用方式，不是完整生产级喂流器。

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
