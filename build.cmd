@echo off
setlocal

where /Q cl.exe || (
  set __VSCMD_ARG_NO_LOGO=1
  call "%VS2019INSTALLDIR%\VC\Auxiliary\Build\vcvarsall.bat" amd64 || exit /b 1
)

if "%Platform%" neq "x64" (
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

rc.exe /nologo wcap.rc
cl.exe /nologo /MP *.c /Fewcap.exe wcap.res /link /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:wcap.manifest /SUBSYSTEM:WINDOWS /FIXED /merge:_RDATA=.rdata
del *.obj *.res >nul
