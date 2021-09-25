wcap
====

Simple and efficient screen recording utility for Windows.

Get latest binary here: [wcap.exe][]

 * press <kbd>Ctrl + PrintScreen</kbd> to start recording monitor (where mouse cursor currently is positioned)
 * press <kbd>Ctrl + Win + PrintScreen</kbd> to start recording currently active window
 * press <kbd>Ctrl + Shift + PrintScreen</kbd> to select & record fixed position area on current monitor
 * press any of previous combinations to stop recording
 * right or double-click on tray icon to change settings

Info
====

wcap uses [Windows.Graphics.Capture][wgc] API available since **Windows 10 version 1903, May 2019 Update (19H1)** to capture
contents of window or whole monitor. Captured texture is submitted to Media Foundation to encode video to mp4 file with
hardware accelerated H264 codec. Using capture from compositor and hardware accelerated encoder allows it to consume very
little CPU and memory.

You can choose in settings to capture only client area or full size of window - client area will not include title bar and
borders for standard windows style. Recorded video size is determined by initial window size.

Video is encoded with H264 codec using Media Foundation API. By default hardware encoder is preferred, but you can disable
it in settings - then video will be encoded using [Microsoft Media Foundation H264][MSMFH264] software encoder. It will be
automatically used in case GPU/driver does not provide hardware accelerated encoder. You might want to explicitly use
software encoder on older GPU's as their hardware encoder quality is not so great.

Audio is captured using [WASAPI loopback recording][] and encoded using [Microsoft Media Foundation AAC][MSMFAAC] encoder.

Recorded mp4 file can be set to use fragmented mp4 format in settings. Fragmented mp4 file does not require "finalizing" it.
Which means that in case application, or GPU drvier crashes or you run out of disk space, the partial mp4 file will be valid
for playback. The disadvantage of fragmented mp4 file is that it is a bit larger than normal mp4 format, and seeking is slower.

You can use settings dialog to restrict max resolution of video - captured image from monitor or window will be scaled down
to keep aspect ratio if you set any of max width/height settings to non-zero value. Similarly framerate of capture can be
reduced to limit maximum amount of frames per second. Setting it to zero will use compositor framerate which is typically
monitor refresh rate. Lower video framerate will give higher quality video for same bitrate and reduced GPU usage. If you
notice too many dropped frames during recording, try reducing video resolution and framerate.

Capture of mouse cursor in video can be disabled when using Windows 10 version 2004, May 2020 Update (20H1) or newer.

Creating gif from mp4
=====================

If you want to create gif file out of recorded mp4 file, you can use following .bat file:

    ffmpeg.exe -hide_banner -nostdin -loglevel fatal -stats -y -i %1 -filter_complex "[0]fps=15,split[v0][v1];[v0]palettegen=stats_mode=full[p];[v1][p]paletteuse" %~n1.gif

And to use new palette every frame to have more colors, but larger filesize:

    ffmpeg.exe -hide_banner -nostdin -loglevel fatal -stats -y -i %1 -filter_complex "[0]fps=15,split[v0][v1];[v0]palettegen=stats_mode=single[p];[v1][p]paletteuse=new=1" %~n1.gif

Put this line in `make_gif.bat` file, place [ffmpeg][] executable next to it and then simply drag & drop .mp4 file on top of it.
Change `fps=15` to desired gif fps (or remove to use original video fps). Check the [paletteuse][] filter arguments for
different dither methods.

Building
========

To build the binary from source code, have [Visual Studio 2019][VS2019] installed, and simply run `build.cmd`.

Future plans
============

 * Allow to set max file size or duration of recording
 * Maybe automatically handle default audio device changes when recording audio?
 * Maybe allow to choose HEVC codec? Could be useful for recording in HDR 10-bit format

License
=======

This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this software, either in source code form or as
a compiled binary, for any purpose, commercial or non-commercial, and by any means.

[wcap.exe]: https://raw.githubusercontent.com/wiki/mmozeiko/wcap/wcap.exe
[wgc]: https://blogs.windows.com/windowsdeveloper/2019/09/16/new-ways-to-do-screen-capture/
[MSMFH264]: https://docs.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder
[VS2019]: https://visualstudio.microsoft.com/vs/
[WASAPI loopback recording]: https://docs.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
[MSMFAAC]: https://docs.microsoft.com/en-us/windows/win32/medfound/aac-encoder
[ffmpeg]: https://ffmpeg.org/
[paletteuse]: https://ffmpeg.org/ffmpeg-filters.html#paletteuse
