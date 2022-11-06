#pragma once

#define UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_DEPRECATE

#include <initguid.h>
#include <windows.h>
#include <intrin.h>

#define WCAP_TITLE L"wcap"
#define WCAP_URL   L"https://github.com/mmozeiko/wcap"

#ifdef _DEBUG
#define Assert(Cond) do { if (!(Cond)) __debugbreak(); } while (0)
#else
#define Assert(Cond) (void)(Cond)
#endif
#define HR(hr) do { HRESULT _hr = (hr); Assert(SUCCEEDED(_hr)); } while (0)

// calculates ceil(X * Num / Den)
#define MUL_DIV_ROUND_UP(X, Num, Den) (((X) * (Num) - 1) / (Den) + 1)

// MF works with 100nsec units
#define MF_UNITS_PER_SECOND 10000000ULL

#include <stdio.h>
#define StrFormat(Buffer, ...) _snwprintf(Buffer, _countof(Buffer), __VA_ARGS__)
