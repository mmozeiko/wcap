Changelog
=========

##### 2025.05.25
 * fix screen capture framerate on Windows 11 24H2

##### 2025.02.10
 * fix border still showing when cannot start region capture
 * fix typo when saving config

##### 2024.10.18
 * add native arm64 Windows support
 * add AV1 codec for encoding - 8-bit and 10-bit main profile

##### 2024.09.15
 * record application local audio for window capture (Windows 10 "20H1" and up)
 * allow to disable yellow capture border indicator (Windows 11)
 * allow to disable rounded window corners for window capture (Windows 11)
 * resize performance improvements when limiting max width/height
 * option to do gamma correct resize
 * option to improve RGB to YUV color conversion
 * hardcode keyboard shorcut names, GetKeyNameText does not work for all keys
 * raised default size limit from 8MB to 25MB
 * renamed "capture rectangle" to "capture region" in config UI
 * bunch of code cleanup, now doing single-TU build

##### 2023.09.21
 * predefined region capture sizes

##### 2023.09.16
 * check that WASAPI device timestamps are usable, otherwise fall back to stream position

##### 2022.11.06
 * remove notifications on start & stop of capture
 * remove Windows 10 version check on startup, code always assumes Windows 10
 * support for 10-bit HEVC encoding

##### 2021.12.21
 * allow to choose integrated vs discrete GPU to use for encoding
 * keep proper encoded video stream time when no new frames are captured

##### 2021.12.18
 * fixed wrong audio timestamps when encoding audio
 * fixed wrong d3d11 calls to copy texture when size is odd
 * fixed hanging when encoding audio & video with too many dropped frames

##### 2021.12.08
 * fixed compute shaders to work on older D3D11 hardware

##### 2021.12.05
 * allow to selected limited vs full range for YUV conversion

##### 2021.12.04
 * improved resizing and YUV conversion quality
 * improved performance for drawing background for rectangle selection
 * fix crash when child window is in foreground for window capture

##### 2021.10.17
 * allow to configure keyboad shortcuts

##### 2021.10.04
 * option to encode video with HEVC codec
 * option to encode audio with FLAC codec
 * allow limit file length or size
 * allow to choose output folder location
 * customize audio codec channels & samplerate

##### 2021.09.25
 * allow to capture fixed position rectangle area on screen
 * prevent config dialog to be open multiple times

##### 2021.09.20
 * added WASAPI loopback recording
 * audio is encoded using AAC codec
 * fix crash when capturing toolbar window

##### 2021.09.19
 * initial release
