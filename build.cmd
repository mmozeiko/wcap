@echo off
setlocal enabledelayedexpansion

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
  if "!VS!" equ "" (
    echo ERROR: Visual Studio installation not found
    exit /b 1
  )  
  call "!VS!\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1
)

if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
  echo ERROR: please run this from MSVC x64 native tools command prompt, 32-bit target is not supported!
  exit /b 1
)

if "%1" equ "debug" (
  set CL=/MTd /Od /Zi /D_DEBUG /RTC1 /Fdwcap.pdb /fsanitize=address
  set LINK=/DEBUG
  set FXC=/Od /Zi
) else (
  set CL=/GL /O1 /Oi /DNDEBUG /GS-
  set LINK=/LTCG /OPT:REF /OPT:ICF ucrt.lib libvcruntime.lib
  set FXC=/O3 /Qstrip_reflect /Qstrip_debug /Qstrip_priv
)

call :fxc ResizePassH            || exit /b 1
call :fxc ResizePassV            || exit /b 1
call :fxc ResizeLinearPassH      || exit /b 1
call :fxc ResizeLinearPassV      || exit /b 1
call :fxc ResizeSinglePass       || exit /b 1
call :fxc ResizeSingleLinearPass || exit /b 1
call :fxc ConvertSinglePass      || exit /b 1
call :fxc ConvertPass1           || exit /b 1
call :fxc ConvertPass2           || exit /b 1

rc.exe /nologo wcap.rc || exit /b 1
cl.exe /nologo /W3 /WX wcap.c wcap.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata || exit /b 1
del *.obj *.res >nul

goto :eof

:fxc
if not exist shaders mkdir shaders
fxc.exe /nologo %FXC% /WX /Ges /T cs_5_0 /E %1 /Fo shaders\%1.dxbc /Fc shaders\%1.asm wcap_shaders.hlsl || exit /b 1
fxc.exe /nologo /compress /Vn %1ShaderBytes /Fo shaders\%1.dcs /Fh shaders\%1.h shaders\%1.dxbc || exit /b 1
goto :eof
