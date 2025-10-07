#pragma once

#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <initguid.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <intrin.h>
#include <stdatomic.h>

#define WCAP_TITLE L"wcap"
#define WCAP_URL   L"https://github.com/mmozeiko/wcap"

// helper to widen narrow string literals to wide at compile time
#define WCAP_WIDEN2(x) L##x
#define WCAP_WIDEN(x) WCAP_WIDEN2(x)

#if defined(WCAP_GIT_INFO)
#	define WCAP_CONFIG_TITLE WCAP_WIDEN("wcap, " __DATE__ " [" WCAP_GIT_INFO "]")
#else
#	define WCAP_CONFIG_TITLE WCAP_WIDEN("wcap, " __DATE__)
#endif

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// caclulates ceil(X / Y)
#define DIV_ROUND_UP(X, Y) ( ((X) + (Y) - 1) / (Y) )

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)
