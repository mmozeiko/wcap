@echo off
setlocal enabledelayedexpansion

if "%PROCESSOR_ARCHITECTURE%" equ "AMD64" (
  set HOST_ARCH=x64
) else if "%PROCESSOR_ARCHITECTURE%" equ "ARM64" (
  set HOST_ARCH=arm64
)

set ARGS=%*
if "%ARGS%" equ "" set ARGS=%HOST_ARCH%

if "%ARGS:x64=%" neq "!ARGS!" (
  set TARGET_ARCH=x64
) else if "%ARGS:arm64=%" neq "!ARGS!" (
  set TARGET_ARCH=arm64
) else (
  set TARGET_ARCH=%HOST_ARCH%
)

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )
  call "!VS!\Common7\Tools\VsDevCmd.bat" -arch=%TARGET_ARCH% -host_arch=%HOST_ARCH% -startdir=none -no_logo || exit /b 1
)

if "%ARGS:debug=%" neq "%ARGS%" (
  set CL=/MTd /Od /Z7 /D_DEBUG /RTC1
  set LINK=/DEBUG
  set FXC=/Od /Zi
  if "%TARGET_ARCH%" equ "x64" set CL=!CL! /fsanitize=address
) else (
  set CL=/GL /O1 /Oi /DNDEBUG /GS-
  set LINK=/LTCG /OPT:REF /OPT:ICF ucrt.lib libvcruntime.lib
  set FXC=/O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv
)

if "%TARGET_ARCH%" equ "arm64" set CL=%CL% /arch:armv8.1
if "%TARGET_ARCH%" equ "x64" set LINK=%LINK% /FIXED /merge:_RDATA=.rdata

call :fxc ResizePassH            || exit /b 1
call :fxc ResizePassV            || exit /b 1
call :fxc ResizeLinearPassH      || exit /b 1
call :fxc ResizeLinearPassV      || exit /b 1
call :fxc ConvertSinglePass      || exit /b 1
call :fxc ConvertPass1           || exit /b 1
call :fxc ConvertPass2           || exit /b 1

for /f %%i in ('call git describe --always --dirty') do set CL=%CL% -DWCAP_GIT_INFO=\"%%i\"

rc.exe /nologo wcap.rc || exit /b 1
cl.exe /nologo /std:c11 /experimental:c11atomics /W3 /WX wcap.c wcap.res /Fewcap-%TARGET_ARCH%.exe /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS || exit /b 1
del *.obj *.res >nul

goto :eof

:fxc
if not exist shaders mkdir shaders
fxc.exe /nologo %FXC% /WX /Ges /T cs_5_0 /E %1 /Fo shaders\%1.dxbc /Fc shaders\%1.asm wcap_shaders.hlsl || exit /b 1
fxc.exe /nologo /compress /Vn %1ShaderBytes /Fo shaders\%1.dcs /Fh shaders\%1.h shaders\%1.dxbc || exit /b 1
goto :eof
