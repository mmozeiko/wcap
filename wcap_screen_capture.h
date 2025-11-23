#pragma once

#include "wcap.h"
#include <d3d11.h>
#include <dwmapi.h>

#define WINDOWS_FOUNDATION_UNIVERSALAPICONTRACT_VERSION 0x130000
#include <windows.graphics.capture.h>

typedef struct IGraphicsCaptureItemInterop IGraphicsCaptureItemInterop;

//
// interface
//

typedef struct
{
	// public
	ID3D11Texture2D* Texture;
	uint64_t Time;
	RECT Rect;
	// private
	uint32_t Width;
	uint32_t Height;
	IUnknown* NextFrame;
}
ScreenCaptureFrame;

typedef struct ScreenCapture ScreenCapture;

// return true to continue capture, or false stop any further catpures
typedef bool ScreenCapture_OnFrameCallback(ScreenCapture* Capture, ScreenCaptureFrame* Frame);

typedef struct ScreenCapture
{
	IGraphicsCaptureItemInterop* ItemInterop;
	__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics* FramePoolStatics;
	__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics2* FramePoolStatics2;
	IInspectable* Controller; // actually IDispatcherQueueController

	__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice* Device;
	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem* Item;
	__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool* FramePool;
	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession* Session;

	ScreenCapture_OnFrameCallback* OnFrame;
	__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable OnFrameHandler;
	__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable OnCloseHandler;
	EventRegistrationToken OnFrameToken;
	EventRegistrationToken OnCloseToken;

	__x_ABI_CWindows_CGraphics_CSizeInt32 CurrentSize;
	RECT Rect;
	HWND Window;
	bool OnlyClientArea;

	bool RestoreWindowCornerPreference;
	DWM_WINDOW_CORNER_PREFERENCE WindowCornerPreference;
}
ScreenCapture;

static bool ScreenCapture_IsSupported(void);
static bool ScreenCapture_CanHideMouseCursor(void);
static bool ScreenCapture_CanHideRecordingBorder(void);
static bool ScreenCapture_CanDisableRoundedCorners(void);
static bool ScreenCapture_CanIncludeSecondaryWindows(void);

// if OnFrame is NULL then you need to periodically call GetFrame/ReleaseFrame manually
// if OnFrame is not NULL and CallbackOnThread is false then callback will be invoked from message processing loop on the same thread as Create call
// if OnFrame is not NULL and CallbackOnThread is true, then callback will be invoked on backgroud thread
static void ScreenCapture_Create(ScreenCapture* Capture, ScreenCapture_OnFrameCallback* OnFrame, bool CallbackOnThread);
static void ScreenCapture_Release(ScreenCapture* Capture);

static bool ScreenCapture_CreateForWindow(ScreenCapture* Capture, ID3D11Device* Device, HWND Window, bool OnlyClientArea, bool DisableRoundedCorners);
static bool ScreenCapture_CreateForMonitor(ScreenCapture* Capture, ID3D11Device* Device, HMONITOR Monitor, const RECT* Rect);
static void ScreenCapture_Start(ScreenCapture* Capture, bool WithMouseCursor, bool WithRecordingBorder, bool IncludeSecondaryWindows);
static void ScreenCapture_Stop(ScreenCapture* Capture);

static bool ScreenCapture_GetFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame);
static void ScreenCapture_ReleaseFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame);

//
// implementation
//

#include <dxgi.h>
#include <roapi.h>

#ifndef SCREEN_CAPTURE_BUFFER_COUNT
#define SCREEN_CAPTURE_BUFFER_COUNT 2
#endif

#ifndef SCREEN_CAPTURE_BUFFER_FORMAT
#define SCREEN_CAPTURE_BUFFER_FORMAT DirectXPixelFormat_B8G8R8A8UIntNormalized // same as DXGI_FORMAT_B8G8R8A8_UNORM
#endif

// this really should be just these three includes, but Microsoft decided to not do proper COM headers that support C :(
//#include <dispatcherqueue.h>
//#include <windows.graphics.capture.interop.h>
//#include <windows.graphics.directx.direct3d11.interop.h>

typedef enum {
	DQTAT_COM_NONE = 0,
	DQTAT_COM_ASTA = 1,
	DQTAT_COM_STA = 2,
} DISPATCHERQUEUE_THREAD_APARTMENTTYPE;

typedef enum {
	DQTYPE_THREAD_DEDICATED = 1,
	DQTYPE_THREAD_CURRENT = 2,
} DISPATCHERQUEUE_THREAD_TYPE;

typedef struct {
	DWORD                                dwSize;
	DISPATCHERQUEUE_THREAD_TYPE          threadType;
	DISPATCHERQUEUE_THREAD_APARTMENTTYPE apartmentType;
} DispatcherQueueOptions;

struct IGraphicsCaptureItemInteropVtbl
{
	STDMETHOD(QueryInterface)(IGraphicsCaptureItemInterop* This, const GUID* Riid, void** Object);
	STDMETHOD_(ULONG, AddRef)(IGraphicsCaptureItemInterop* This);
	STDMETHOD_(ULONG, Release)(IGraphicsCaptureItemInterop* This);
	STDMETHOD(CreateForWindow)(IGraphicsCaptureItemInterop* This, HWND Window, const GUID* Riid, void** Result);
	STDMETHOD(CreateForMonitor)(IGraphicsCaptureItemInterop* This, HMONITOR Monitor, const GUID* Riid, void** Result);
};

struct IGraphicsCaptureItemInterop
{
	struct IGraphicsCaptureItemInteropVtbl* lpVtbl;
};

typedef struct IDirect3DDxgiInterfaceAccess IDirect3DDxgiInterfaceAccess;

struct IDirect3DDxgiInterfaceAccessVtbl
{
	STDMETHOD(QueryInterface)(IDirect3DDxgiInterfaceAccess* This, const GUID* Riid, void** Object);
	STDMETHOD_(ULONG, AddRef)(IDirect3DDxgiInterfaceAccess* This);
	STDMETHOD_(ULONG, Release)(IDirect3DDxgiInterfaceAccess* This);
	STDMETHOD(GetInterface)(IDirect3DDxgiInterfaceAccess* This, const GUID* Riid, void** Object);
};

struct IDirect3DDxgiInterfaceAccess
{
	struct IDirect3DDxgiInterfaceAccessVtbl* lpVtbl;
};

// from ntdll.dll
extern __declspec(dllimport) LONG WINAPI RtlGetVersion(RTL_OSVERSIONINFOW*);

// from CoreMessaging.dll
extern   __declspec(dllimport) HRESULT WINAPI CreateDispatcherQueueController(DispatcherQueueOptions, IInspectable**);

// from d3d11.dll
extern __declspec(dllimport) HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**);

DEFINE_GUID(IID_IClosable,                           0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e);
DEFINE_GUID(IID_IGraphicsCaptureSession2,            0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e);
DEFINE_GUID(IID_IGraphicsCaptureSession3,            0xf2cdd966, 0x22ae, 0x5ea1, 0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe);
DEFINE_GUID(IID_IGraphicsCaptureSession5,            0x67c0ea62, 0x1f85, 0x5061, 0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb);
DEFINE_GUID(IID_IGraphicsCaptureSession6,            0xd7419236, 0xbe20, 0x5e9f, 0xbc, 0xd6, 0xc4, 0xe9, 0x8f, 0xd6, 0xaf, 0xdc);
DEFINE_GUID(IID_IGraphicsCaptureItemInterop,         0x3628e81b, 0x3cac, 0x4c60, 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56);
DEFINE_GUID(IID_IGraphicsCaptureItem,                0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60);
DEFINE_GUID(IID_IGraphicsCaptureItemHandler,         0xe9c610c0, 0xa68c, 0x5bd9, 0x80, 0x21, 0x85, 0x89, 0x34, 0x6e, 0xee, 0xe2);
DEFINE_GUID(IID_IDirect3D11CaptureFramePoolStatics,  0x7784056a, 0x67aa, 0x4d53, 0xae, 0x54, 0x10, 0x88, 0xd5, 0xa8, 0xca, 0x21);
DEFINE_GUID(IID_IDirect3D11CaptureFramePoolStatics2, 0x589b103f, 0x6bbc, 0x5df5, 0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5);
DEFINE_GUID(IID_IDirect3D11CaptureFramePoolHandler,  0x51a947f7, 0x79cf, 0x5a3e, 0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8);
DEFINE_GUID(IID_IDirect3DDxgiInterfaceAccess,        0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1);

static HRESULT STDMETHODCALLTYPE ScreenCapture__FrameQueryInterface(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IDirect3D11CaptureFramePoolHandler) ||
		IsEqualGUID(Riid, &IID_IAgileObject) ||
		IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ScreenCapture__FrameAddRef(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE ScreenCapture__FrameRelease(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE ScreenCapture__FrameInvoke(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectable* This, __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool* Sender, IInspectable* Args)
{
	ScreenCapture* Capture = CONTAINING_RECORD(This, ScreenCapture, OnFrameHandler);

	ScreenCaptureFrame Frame;
	if (ScreenCapture_GetFrame(Capture, &Frame))
	{
		bool ContinueCapture = Capture->OnFrame(Capture, &Frame);
		ScreenCapture_ReleaseFrame(Capture, &Frame);

		if (!ContinueCapture)
		{
			ScreenCapture_Stop(Capture);
		}
	}

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ScreenCapture__CloseQueryInterface(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable* This, REFIID Riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(Riid, &IID_IGraphicsCaptureItemHandler) ||
		IsEqualGUID(Riid, &IID_IAgileObject) ||
		IsEqualGUID(Riid, &IID_IUnknown))
	{
		*Object = This;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ScreenCapture__CloseAddRef(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable* This)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE ScreenCapture__CloseRelease(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable* This)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE ScreenCapture__CloseInvoke(__FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectable* This, __x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem* Sender, IInspectable* Args)
{
	ScreenCapture* Capture = CONTAINING_RECORD(This, ScreenCapture, OnCloseHandler);
	Capture->OnFrame(Capture, NULL);
	return S_OK;
}

static __FITypedEventHandler_2_Windows__CGraphics__CCapture__CDirect3D11CaptureFramePool_IInspectableVtbl ScreenCapture__OnFrameHandlerVtbl =
{
	.QueryInterface = ScreenCapture__FrameQueryInterface,
	.AddRef         = ScreenCapture__FrameAddRef,
	.Release        = ScreenCapture__FrameRelease,
	.Invoke         = ScreenCapture__FrameInvoke,
};

static __FITypedEventHandler_2_Windows__CGraphics__CCapture__CGraphicsCaptureItem_IInspectableVtbl ScreenCapture__OnCloseHandlerVtbl =
{
	.QueryInterface = ScreenCapture__CloseQueryInterface,
	.AddRef         = ScreenCapture__CloseAddRef,
	.Release        = ScreenCapture__CloseRelease,
	.Invoke         = ScreenCapture__CloseInvoke,
};

static RECT ScreenCapture__GetRect(ScreenCapture* Capture, uint32_t Width, uint32_t Height)
{
	if (Capture->Window) // capturing window
	{
		RECT WindowRect;
		if (FAILED(DwmGetWindowAttribute(Capture->Window, DWMWA_EXTENDED_FRAME_BOUNDS, &WindowRect, sizeof(WindowRect))))
		{
			return (RECT) { 0 };
		}

		if (Capture->OnlyClientArea)
		{
			RECT ClientRect;
			GetClientRect(Capture->Window, &ClientRect);

			POINT TopLeft = { 0, 0 };
			ClientToScreen(Capture->Window, &TopLeft);

			RECT Rect;
			Rect.left = max(0, TopLeft.x - WindowRect.left);
			Rect.top = max(0, TopLeft.y - WindowRect.top);
			Rect.right = Rect.left + min((uint32_t)ClientRect.right, Width - Rect.left);
			Rect.bottom = Rect.top + min((uint32_t)ClientRect.bottom, Height - Rect.top);
			return Rect;
		}
		else // whole window size
		{
			return (RECT)
			{
				.left = 0,
				.top = 0,
				.right = WindowRect.right - WindowRect.left,
				.bottom = WindowRect.bottom - WindowRect.top,
			};
		}
	}
	else // capturing monitor, or region on it
	{
		return Capture->Rect;
	}
}

static bool ScreenCapture__CreateFramePool(ScreenCapture* Capture, __x_ABI_CWindows_CGraphics_CSizeInt32 Size, __x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool** FramePool)
{
	if (Capture->FramePoolStatics2)
	{
		return SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics2_CreateFreeThreaded(Capture->FramePoolStatics2, Capture->Device, SCREEN_CAPTURE_BUFFER_FORMAT, SCREEN_CAPTURE_BUFFER_COUNT, Size, FramePool));
	}
	else
	{
		return SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics_Create(Capture->FramePoolStatics, Capture->Device, SCREEN_CAPTURE_BUFFER_FORMAT, SCREEN_CAPTURE_BUFFER_COUNT, Size, FramePool));
	}
}

bool ScreenCapture_IsSupported(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 10 version 1903, May 2019 Update (19H1), build 10.0.18362.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 18362);
}

bool ScreenCapture_CanHideMouseCursor(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 10 version 2004, May 2020 Update (20H1), build 10.0.19041.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 19041);
}

bool ScreenCapture_CanHideRecordingBorder(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 11 21H2, build 10.0.22000.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 22000);
}

bool ScreenCapture_CanDisableRoundedCorners(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 11 21H2, build 10.0.22000.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 22000);
}

bool ScreenCapture_CanIncludeSecondaryWindows(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 11 24H2, build 10.0.26100.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 26100);
}

void ScreenCapture_Create(ScreenCapture* Capture, ScreenCapture_OnFrameCallback* OnFrame, bool CallbackOnThread)
{
	HR(RoInitialize(RO_INIT_SINGLETHREADED));

	typedef struct
	{
		DWORD Flags;
		DWORD Length;
		DWORD Padding1;
		DWORD Padding2;
		LPCWCHAR Ptr;
	}
	StaticHSTRING;

#define STATIC_HSTRING(Str) (HSTRING)&(StaticHSTRING){ 1, ARRAYSIZE(Str) - 1, 0, 0, Str }
	HSTRING GraphicsCaptureItem = STATIC_HSTRING(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
	HSTRING Direct3D11CaptureFramePool = STATIC_HSTRING(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
#undef STATIC_HSTRING

	HR(RoGetActivationFactory(GraphicsCaptureItem, &IID_IGraphicsCaptureItemInterop, (void**)&Capture->ItemInterop));

	if (OnFrame && CallbackOnThread)
	{
		HR(RoGetActivationFactory(Direct3D11CaptureFramePool, &IID_IDirect3D11CaptureFramePoolStatics2, (void**)&Capture->FramePoolStatics2));
		Capture->FramePoolStatics = NULL;
	}
	else
	{
		HR(RoGetActivationFactory(Direct3D11CaptureFramePool, &IID_IDirect3D11CaptureFramePoolStatics, (void**)&Capture->FramePoolStatics));
		Capture->FramePoolStatics2 = NULL;
	}

	if (OnFrame && !CallbackOnThread)
	{
		// create dispatcher queue that will call callbacks on main thread as part of message loop
		DispatcherQueueOptions Options =
		{
			.dwSize = sizeof(Options),
			.threadType = DQTYPE_THREAD_CURRENT,
			.apartmentType = DQTAT_COM_NONE,
		};
		HR(CreateDispatcherQueueController(Options, &Capture->Controller));
	}

	Capture->OnFrameHandler.lpVtbl = &ScreenCapture__OnFrameHandlerVtbl;
	Capture->OnCloseHandler.lpVtbl = &ScreenCapture__OnCloseHandlerVtbl;
	Capture->OnFrame = OnFrame;
}

void ScreenCapture_Release(ScreenCapture* Capture)
{
	if (Capture->Controller)
	{
		IInspectable_Release(Capture->Controller);
	}
	if (Capture->FramePoolStatics)
	{
		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics_Release(Capture->FramePoolStatics);
	}
	else
	{
		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePoolStatics2_Release(Capture->FramePoolStatics2);
	}
	Capture->ItemInterop->lpVtbl->Release(Capture->ItemInterop);

	RoUninitialize();
}

bool ScreenCapture_CreateForWindow(ScreenCapture* Capture, ID3D11Device* Device, HWND Window, bool OnlyClientArea, bool DisableRoundedCorners)
{
	IDXGIDevice* DxgiDevice;
	HR(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (LPVOID*)&DxgiDevice));
	HR(CreateDirect3D11DeviceFromDXGIDevice(DxgiDevice, (IInspectable**)&Capture->Device));
	IDXGIDevice_Release(DxgiDevice);

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem* Item;
	if (SUCCEEDED(Capture->ItemInterop->lpVtbl->CreateForWindow(Capture->ItemInterop, Window, &IID_IGraphicsCaptureItem, (void**)&Item)))
	{
		__x_ABI_CWindows_CGraphics_CSizeInt32 Size;
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_get_Size(Item, &Size));

		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool* FramePool;
		if (ScreenCapture__CreateFramePool(Capture, Size, &FramePool))
		{
			Capture->Item = Item;
			Capture->FramePool = FramePool;
			Capture->OnlyClientArea = OnlyClientArea;
			Capture->Window = Window;

			RECT Rect = ScreenCapture__GetRect(Capture, Size.Width, Size.Height);
			Capture->CurrentSize.Width = Rect.right - Rect.left;
			Capture->CurrentSize.Height = Rect.bottom - Rect.top;
			Capture->Rect = Rect;

			Capture->RestoreWindowCornerPreference = false;
			if (DisableRoundedCorners)
			{
				if (SUCCEEDED(DwmGetWindowAttribute(Window, DWMWA_WINDOW_CORNER_PREFERENCE, &Capture->WindowCornerPreference, sizeof(Capture->WindowCornerPreference))))
				{
					DWM_WINDOW_CORNER_PREFERENCE Preference = DWMWCP_DONOTROUND;
					if (SUCCEEDED(DwmSetWindowAttribute(Window, DWMWA_WINDOW_CORNER_PREFERENCE, &Preference, sizeof(Preference))))
					{
						Capture->RestoreWindowCornerPreference = true;
					}
				}
			}

			return true;
		}
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_Release(Item);
	}

	__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice_Release(Capture->Device);
	return false;
}

bool ScreenCapture_CreateForMonitor(ScreenCapture* Capture, ID3D11Device* Device, HMONITOR Monitor, const RECT* Rect)
{
	IDXGIDevice* DxgiDevice;
	HR(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (LPVOID*)&DxgiDevice));
	HR(CreateDirect3D11DeviceFromDXGIDevice(DxgiDevice, (IInspectable**)&Capture->Device));
	IDXGIDevice_Release(DxgiDevice);

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem* Item;
	if (SUCCEEDED(Capture->ItemInterop->lpVtbl->CreateForMonitor(Capture->ItemInterop, Monitor, &IID_IGraphicsCaptureItem, (void**)&Item)))
	{
		__x_ABI_CWindows_CGraphics_CSizeInt32 Size;
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_get_Size(Item, &Size));

		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool* FramePool;
		if (ScreenCapture__CreateFramePool(Capture, Size, &FramePool))
		{
			Capture->Item = Item;
			Capture->FramePool = FramePool;
			Capture->Window = NULL;
			Capture->CurrentSize = Size;
			Capture->Rect = Rect ? *Rect : (RECT) { 0, 0, Size.Width, Size.Height };

			Capture->RestoreWindowCornerPreference = false;
			return true;
		}
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_Release(Item);
	}

	__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice_Release(Capture->Device);
	return false;
}

void ScreenCapture_Start(ScreenCapture* Capture, bool WithMouseCursor, bool WithRecordingBorder, bool IncludeSecondaryWindows)
{
	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession* Session;
	HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_CreateCaptureSession(Capture->FramePool, Capture->Item, &Session));

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession2* Session2;
	if (SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_QueryInterface(Session, &IID_IGraphicsCaptureSession2, (void**)&Session2)))
	{
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession2_put_IsCursorCaptureEnabled(Session2, (boolean)WithMouseCursor);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession2_Release(Session2);
	}

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession3* Session3;
	if (SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_QueryInterface(Session, &IID_IGraphicsCaptureSession3, (void**)&Session3)))
	{
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession3_put_IsBorderRequired(Session3, (boolean)WithRecordingBorder);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession3_Release(Session3);
	}

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession5* Session5;
	if (SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_QueryInterface(Session, &IID_IGraphicsCaptureSession5, (void**)&Session5)))
	{
		// TimeSpan value is in 100ns units
		// min duration supported by API is 1msec, this means max supported framerate possible is 1000 fps
		__x_ABI_CWindows_CFoundation_CTimeSpan Duration = { MF_UNITS_PER_SECOND / 1000 };
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession5_put_MinUpdateInterval(Session5, Duration);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession5_Release(Session5);
	}

	__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession6* Session6;
	if (SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_QueryInterface(Session, &IID_IGraphicsCaptureSession6, (void**)&Session6)))
	{
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession6_put_IncludeSecondaryWindows(Session6, (boolean)IncludeSecondaryWindows);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession6_Release(Session6);
	}

	if (Capture->OnFrame)
	{
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_add_FrameArrived(Capture->FramePool, &Capture->OnFrameHandler, &Capture->OnFrameToken));
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_add_Closed(Capture->Item, &Capture->OnCloseHandler, &Capture->OnCloseToken));
	}

	HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_StartCapture(Session));
	Capture->Session = Session;
}

void ScreenCapture_Stop(ScreenCapture* Capture)
{
	__x_ABI_CWindows_CFoundation_CIClosable* Closable;

	if (Capture->OnFrameToken.value)
	{
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_remove_FrameArrived(Capture->FramePool, Capture->OnFrameToken));
		Capture->OnFrameToken.value = 0;
	}

	if (Capture->OnCloseToken.value)
	{
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_remove_Closed(Capture->Item, Capture->OnCloseToken));
		Capture->OnCloseToken.value = 0;
	}

	if (Capture->Session)
	{
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_QueryInterface(Capture->Session, &IID_IClosable, (void**)&Closable));
		HR(__x_ABI_CWindows_CFoundation_CIClosable_Close(Closable));
		__x_ABI_CWindows_CFoundation_CIClosable_Release(Closable);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureSession_Release(Capture->Session);

		Capture->Session = NULL;
	}

	if (Capture->Item)
	{
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_QueryInterface(Capture->FramePool, &IID_IClosable, (void**)&Closable));
		HR(__x_ABI_CWindows_CFoundation_CIClosable_Close(Closable));
		__x_ABI_CWindows_CFoundation_CIClosable_Release(Closable);
		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_Release(Capture->FramePool);
		__x_ABI_CWindows_CGraphics_CCapture_CIGraphicsCaptureItem_Release(Capture->Item);
		Capture->Item = NULL;
	}

	if (Capture->Device)
	{
		__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DDevice_Release(Capture->Device);
		Capture->Device = NULL;
	}

	if (Capture->RestoreWindowCornerPreference)
	{
		DwmSetWindowAttribute(Capture->Window, DWMWA_WINDOW_CORNER_PREFERENCE, &Capture->WindowCornerPreference, sizeof(Capture->WindowCornerPreference));
	}
}

bool ScreenCapture_GetFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame* NextFrame;
	if (SUCCEEDED(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_TryGetNextFrame(Capture->FramePool, &NextFrame)) && NextFrame != NULL)
	{
		__x_ABI_CWindows_CFoundation_CTimeSpan Time;
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame_get_SystemRelativeTime(NextFrame, &Time));

		__x_ABI_CWindows_CGraphics_CSizeInt32 Size;
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame_get_ContentSize(NextFrame, &Size));

		__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DSurface* Surface;
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame_get_Surface(NextFrame, &Surface));

		IDirect3DDxgiInterfaceAccess* Access;
		HR(__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DSurface_QueryInterface(Surface, &IID_IDirect3DDxgiInterfaceAccess, (void**)&Access));
		__x_ABI_CWindows_CGraphics_CDirectX_CDirect3D11_CIDirect3DSurface_Release(Surface);

		ID3D11Texture2D* Texture;
		HR(Access->lpVtbl->GetInterface(Access, &IID_ID3D11Texture2D, (void**)&Texture));
		Access->lpVtbl->Release(Access);

		D3D11_TEXTURE2D_DESC TextureDesc;
		ID3D11Texture2D_GetDesc(Texture, &TextureDesc);

		RECT Rect = ScreenCapture__GetRect(Capture, min((uint32_t)Size.Width, TextureDesc.Width), min((uint32_t)Size.Height, TextureDesc.Height));
		if (Rect.right > Rect.left && Rect.bottom > Rect.top)
		{
			*Frame = (ScreenCaptureFrame)
			{
				.Texture = Texture,
				.Time = Time.Duration,
				.Rect = Rect,
				.Width = Size.Width,
				.Height = Size.Height,
				.NextFrame = (IUnknown*)NextFrame,
			};
			return true;
		}
		__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFrame_Release(NextFrame);
	}

	return false;
}

void ScreenCapture_ReleaseFrame(ScreenCapture* Capture, ScreenCaptureFrame* Frame)
{
	ID3D11Texture2D_Release(Frame->Texture);
	IUnknown_Release(Frame->NextFrame);

	if (Capture->CurrentSize.Width != Frame->Width || Capture->CurrentSize.Height != Frame->Height)
	{
		Capture->CurrentSize = (__x_ABI_CWindows_CGraphics_CSizeInt32){ Frame->Width, Frame->Height };
		HR(__x_ABI_CWindows_CGraphics_CCapture_CIDirect3D11CaptureFramePool_Recreate(Capture->FramePool, Capture->Device, SCREEN_CAPTURE_BUFFER_FORMAT, SCREEN_CAPTURE_BUFFER_COUNT, Capture->CurrentSize));
	}
}
