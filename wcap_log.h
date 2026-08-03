#pragma once

#include "wcap.h"
#include <stdio.h>
#include <stdlib.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi")
#include <stdlib.h>   // _countof
#include <shlwapi.h>  // PathRenameExtensionW
#pragma comment(lib, "shlwapi.lib")

//
// Simple thread-safe file + debug console logging for diagnosing hangs.
// Log file is created next to the executable as "wcap-log.txt".
// Each line is flushed immediately so data survives a hang/crash.
//

typedef enum
{
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
} LogLevel;

static CRITICAL_SECTION gLogLock;
static FILE*            gLogFile;
static LARGE_INTEGER    gLogFreq;
static LARGE_INTEGER    gLogStart;
static BOOL             gLogInitialized;

static void Log__Init(void)
{
	if (gLogInitialized) return;

	InitializeCriticalSection(&gLogLock);
	QueryPerformanceFrequency(&gLogFreq);
	QueryPerformanceCounter(&gLogStart);

	WCHAR Path[MAX_PATH];
	GetModuleFileNameW(NULL, Path, _countof(Path));
	PathRenameExtensionW(Path, L"-log.txt");

	// truncate on each launch so the file doesn't grow forever
	gLogFile = _wfopen(Path, L"wb");
	if (gLogFile)
	{
		// write UTF-8 BOM
		fputs("\xEF\xBB\xBF", gLogFile);
		setvbuf(gLogFile, NULL, _IONBF, 0); // unbuffered - flush on every write
	}

	gLogInitialized = TRUE;
}

static void Log__Shutdown(void)
{
	if (!gLogInitialized) return;

	EnterCriticalSection(&gLogLock);
	if (gLogFile)
	{
		fclose(gLogFile);
		gLogFile = NULL;
	}
	LeaveCriticalSection(&gLogLock);

	DeleteCriticalSection(&gLogLock);
	gLogInitialized = FALSE;
}

static const char* Log__LevelStr(LogLevel Level)
{
	switch (Level)
	{
	case LOG_INFO:  return "INFO ";
	case LOG_WARN:  return "WARN ";
	case LOG_ERROR: return "ERROR";
	default:        return "?????";
	}
}

// returns milliseconds since log init (program start)
static double Log__ElapsedMs(void)
{
	LARGE_INTEGER Now;
	QueryPerformanceCounter(&Now);
	return (double)(Now.QuadPart - gLogStart.QuadPart) / (double)gLogFreq.QuadPart * 1000.0;
}

static void Log__Write(LogLevel Level, const char* File, int Line, const char* Fmt, ...)
{
	if (!gLogInitialized) return;

	char    MsgBuf[1024];
	wchar_t WideBuf[1200];

	// format user message
	va_list Args;
	va_start(Args, Fmt);
	int MsgLen = vsnprintf(MsgBuf, sizeof(MsgBuf), Fmt, Args);
	va_end(Args);
	if (MsgLen < 0) MsgLen = 0;
	if (MsgLen >= (int)sizeof(MsgBuf)) MsgLen = sizeof(MsgBuf) - 1;
	MsgBuf[MsgLen] = 0;

	// extract just the filename from the full path for brevity
	const char* ShortFile = File;
	for (const char* P = File; *P; P++)
	{
		if (*P == '\\' || *P == '/') ShortFile = P + 1;
	}

	double Ms = Log__ElapsedMs();
	DWORD  Tid = GetCurrentThreadId();

	EnterCriticalSection(&gLogLock);

	// write to file
	if (gLogFile)
	{
		fprintf(gLogFile, "[%10.3f] [%s] [tid:%05u] %-14s : %s\n",
			Ms, Log__LevelStr(Level), Tid, ShortFile, MsgBuf);
	}

	LeaveCriticalSection(&gLogLock);

	// also emit to debug console (live view with DebugView / debugger)
	_snwprintf(WideBuf, _countof(WideBuf), L"[%10.3f] [%S] [tid:%05u] %-14S : %S\n",
		Ms, Log__LevelStr(Level), Tid, ShortFile, MsgBuf);
	OutputDebugStringW(WideBuf);
}

// convenience macros - pass __FILE__ and __LINE__ automatically
#define LOG_INFO(...)  Log__Write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  Log__Write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) Log__Write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)

// helper: log an HRESULT failure with the hex code
#define LOG_HR(Msg, hr) Log__Write(LOG_ERROR, __FILE__, __LINE__, "%s failed: 0x%08lX", Msg, (unsigned long)(hr))
