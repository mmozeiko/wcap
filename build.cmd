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
  set LINK=/DEBUG libucrtd.lib libvcruntimed.lib
) else (
  set CL=/GL /O2 /DNDEBUG /GS-
  set LINK=/LTCG /OPT:REF /OPT:ICF libvcruntime.lib
)

fxc.exe /nologo /T cs_5_0 /E Resize  /O3 /WX /Fh wcap_resize_shader.h  /Vn ResizeShaderBytes  /Qstrip_reflect /Qstrip_debug /Qstrip_priv wcap_shaders.hlsl
fxc.exe /nologo /T cs_5_0 /E Convert /O3 /WX /Fh wcap_convert_shader.h /Vn ConvertShaderBytes /Qstrip_reflect /Qstrip_debug /Qstrip_priv wcap_shaders.hlsl

rc.exe /nologo wcap.rc
cl.exe /nologo /MP *.c /Fewcap.exe wcap.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata
del *.obj *.res >nul
