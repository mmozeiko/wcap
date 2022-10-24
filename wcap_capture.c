#include "wcap_capture.h"
#include <dwmapi.h>
#include <roapi.h>

#define CAPTURE_BUFFER_COUNT 2

typedef enum {
	DQTAT_COM_NONE = 0,
	DQTAT_COM_ASTA = 1,
	DQTAT_COM_STA = 2
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

#define IUnknown_Parent(_type) \
	HRESULT(STDMETHODCALLTYPE* QueryInterface)(_type* self, const GUID* riid, void** object); \
	ULONG(STDMETHODCALLTYPE* AddRef)(_type* self); \
	ULONG(STDMETHODCALLTYPE* Release)(_type* self)

#define IInspectable_Parent(_type) \
	IUnknown_Parent(_type); \
	void* GetIids; \
	void* GetRuntimeClassName; \
	void* GetTrustLevel

typedef struct IInspectable                       IInspectable;
typedef struct IClosable                          IClosable;
typedef struct IGraphicsCaptureSession            IGraphicsCaptureSession;
typedef struct ITypedEventHandler                 ITypedEventHandler;
typedef struct IGraphicsCaptureSession2           IGraphicsCaptureSession2;
typedef struct IGraphicsCaptureItemInterop        IGraphicsCaptureItemInterop;
typedef struct IGraphicsCaptureItem               IGraphicsCaptureItem;
typedef struct IDirect3D11CaptureFramePoolStatics IDirect3D11CaptureFramePoolStatics;
typedef struct IDirect3D11CaptureFramePool        IDirect3D11CaptureFramePool;
typedef struct IDirect3D11CaptureFrame            IDirect3D11CaptureFrame;
typedef struct IDirect3DDevice                    IDirect3DDevice;
typedef struct IDirect3DSurface                   IDirect3DSurface;
typedef struct IDirect3DDxgiInterfaceAccess       IDirect3DDxgiInterfaceAccess;

// don't really care about contents of IDispatcherQueueController
typedef IInspectable IDispatcherQueueController;

struct IClosableVtbl {
	IInspectable_Parent(IClosable);
	HRESULT(STDMETHODCALLTYPE* Close)(IClosable* this);
};

struct ITypedEventHandlerVtbl {
	IUnknown_Parent(ITypedEventHandler);
	HRESULT(STDMETHODCALLTYPE* Invoke)(ITypedEventHandler* this, IInspectable* sender, IInspectable* args);
};

struct IGraphicsCaptureSessionVtbl {
	IInspectable_Parent(IGraphicsCaptureSession);
	HRESULT(STDMETHODCALLTYPE* StartCapture)(IGraphicsCaptureSession* this);
};

struct IGraphicsCaptureSession2Vtbl {
	IInspectable_Parent(IGraphicsCaptureSession2);
	HRESULT(STDMETHODCALLTYPE* get_IsCursorCaptureEnabled)(IGraphicsCaptureSession2* this, char* value);
	HRESULT(STDMETHODCALLTYPE* put_IsCursorCaptureEnabled)(IGraphicsCaptureSession2* this, char value);
};

struct IGraphicsCaptureItemInteropVtbl {
	IUnknown_Parent(IGraphicsCaptureItemInterop);
	HRESULT(STDMETHODCALLTYPE* CreateForWindow)(IGraphicsCaptureItemInterop* this, HWND window, const GUID* riid, void** result);
	HRESULT(STDMETHODCALLTYPE* CreateForMonitor)(IGraphicsCaptureItemInterop* this, HMONITOR monitor, const GUID* riid, void** result);
};

struct IGraphicsCaptureItemVtbl {
	IInspectable_Parent(IGraphicsCaptureItem);
	void* get_DisplayName;
	HRESULT(STDMETHODCALLTYPE* get_Size)(IGraphicsCaptureItem* this, SIZE* size);
	HRESULT(STDMETHODCALLTYPE* add_Closed)(IGraphicsCaptureItem* this, ITypedEventHandler* handler, UINT64* token);
	HRESULT(STDMETHODCALLTYPE* remove_Closed)(IGraphicsCaptureItem* this, UINT64 token);
};

struct IDirect3D11CaptureFramePoolStaticsVtbl {
	IInspectable_Parent(IDirect3D11CaptureFramePoolStatics);
	HRESULT(STDMETHODCALLTYPE* Create)(IDirect3D11CaptureFramePoolStatics* this, IDirect3DDevice* device, DXGI_FORMAT pixelFormat, INT32 numberOfBuffers, SIZE size, IDirect3D11CaptureFramePool** result);
};

struct IDirect3D11CaptureFramePoolVtbl {
	IInspectable_Parent(IDirect3D11CaptureFramePool);
	HRESULT(STDMETHODCALLTYPE* Recreate)(IDirect3D11CaptureFramePool* this, IDirect3DDevice* device, DXGI_FORMAT pixelFormat, INT32 numberOfBuffers, SIZE size);
	HRESULT(STDMETHODCALLTYPE* TryGetNextFrame)(IDirect3D11CaptureFramePool* this, IDirect3D11CaptureFrame** result);
	HRESULT(STDMETHODCALLTYPE* add_FrameArrived)(IDirect3D11CaptureFramePool* this, ITypedEventHandler* handler, UINT64* token);
	HRESULT(STDMETHODCALLTYPE* remove_FrameArrived)(IDirect3D11CaptureFramePool* this, UINT64 token);
	HRESULT(STDMETHODCALLTYPE* CreateCaptureSession)(IDirect3D11CaptureFramePool* this, IGraphicsCaptureItem* item, IGraphicsCaptureSession** result);
};

struct IDirect3D11CaptureFrameVtbl {
	IInspectable_Parent(IDirect3D11CaptureFrame);
	HRESULT(STDMETHODCALLTYPE* get_Surface)(IDirect3D11CaptureFrame* this, IDirect3DSurface** value);
	HRESULT(STDMETHODCALLTYPE* get_SystemRelativeTime)(IDirect3D11CaptureFrame* this, UINT64* value);
	HRESULT(STDMETHODCALLTYPE* get_ContentSize)(IDirect3D11CaptureFrame* this, SIZE* size);
};

struct IDirect3DDeviceVtbl {
	IInspectable_Parent(IDirect3DDevice);
	void* Trim;
};

struct IDirect3DSurfaceVtbl {
	IInspectable_Parent(IDirect3DSurface);
	void* get_Description;
};

struct IDirect3DDxgiInterfaceAccessVtbl {
	IUnknown_Parent(IDirect3DDxgiInterfaceAccess);
	HRESULT(STDMETHODCALLTYPE* GetInterface)(IDirect3DDxgiInterfaceAccess* this, const GUID* riid, void** object);
};


#define VTBL(name) struct name { const struct name ## Vtbl* vtbl; }
VTBL(IClosable);
VTBL(IGraphicsCaptureSession);
VTBL(IGraphicsCaptureSession2);
VTBL(IGraphicsCaptureItemInterop);
VTBL(IGraphicsCaptureItem);
VTBL(IDirect3D11CaptureFramePoolStatics);
VTBL(IDirect3D11CaptureFramePool);
VTBL(IDirect3D11CaptureFrame);
VTBL(IDirect3DDevice);
VTBL(IDirect3DSurface);
VTBL(IDirect3DDxgiInterfaceAccess);
#undef VTBL

DEFINE_GUID(IID_IClosable,                          0x30d5a829, 0x7fa4, 0x4026, 0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e);
DEFINE_GUID(IID_IGraphicsCaptureSession2,           0x2c39ae40, 0x7d2e, 0x5044, 0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e);
DEFINE_GUID(IID_IGraphicsCaptureItemInterop,        0x3628e81b, 0x3cac, 0x4c60, 0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56);
DEFINE_GUID(IID_IGraphicsCaptureItem,               0x79c3f95b, 0x31f7, 0x4ec2, 0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60);
DEFINE_GUID(IID_IGraphicsCaptureItemHandler,        0xe9c610c0, 0xa68c, 0x5bd9, 0x80, 0x21, 0x85, 0x89, 0x34, 0x6e, 0xee, 0xe2);
DEFINE_GUID(IID_IDirect3D11CaptureFramePoolStatics, 0x7784056a, 0x67aa, 0x4d53, 0xae, 0x54, 0x10, 0x88, 0xd5, 0xa8, 0xca, 0x21);
DEFINE_GUID(IID_IDirect3D11CaptureFramePoolHandler, 0x51a947f7, 0x79cf, 0x5a3e, 0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8);
DEFINE_GUID(IID_IDirect3DDxgiInterfaceAccess,       0xa9b3d012, 0x3df2, 0x4ee3, 0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1);

typedef struct {
	DWORD Flags;
	DWORD Length;
	DWORD Padding1;
	DWORD Padding2;
	LPCWCHAR Ptr;
} StaticHSTRING;

#define STATIC_HSTRING(name, str) static HSTRING name = (HSTRING)&(StaticHSTRING){ 1, sizeof(str) - 1, 0, 0, L## str }
STATIC_HSTRING(GraphicsCaptureItemName,        "Windows.Graphics.Capture.GraphicsCaptureItem"       );
STATIC_HSTRING(Direct3D11CaptureFramePoolName, "Windows.Graphics.Capture.Direct3D11CaptureFramePool");
#undef STATIC_HSTRING

extern __declspec(dllimport) LONG WINAPI RtlGetVersion(RTL_OSVERSIONINFOW*);
extern __declspec(dllimport) HRESULT WINAPI CreateDispatcherQueueController(DispatcherQueueOptions, IDispatcherQueueController**);
extern __declspec(dllimport) HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**);

static RECT Capture__GetRect(Capture* Capture, LONG Width, LONG Height)
{
	if (Capture->Window) // capturing only one window
	{
		RECT WindowRect;
		if (FAILED(DwmGetWindowAttribute(Capture->Window, DWMWA_EXTENDED_FRAME_BOUNDS, &WindowRect, sizeof(WindowRect))))
		{
			return (RECT){ 0 };
		}

		if (Capture->OnlyClientArea) // need only client area of window
		{
			RECT ClientRect;
			GetClientRect(Capture->Window, &ClientRect);

			POINT TopLeft = { 0, 0 };
			ClientToScreen(Capture->Window, &TopLeft);

			RECT Rect;
			Rect.left = max(0, TopLeft.x - WindowRect.left);
			Rect.top = max(0, TopLeft.y - WindowRect.top);
			Rect.right = Rect.left + min(ClientRect.right, Width - Rect.left);
			Rect.bottom = Rect.top + min(ClientRect.bottom, Height - Rect.top);
			return Rect;
		}
		else // need whole window size
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
	else // capturing whole monitor, or rectangle on it
	{
		return Capture->Rect;
	}
}

static HRESULT STDMETHODCALLTYPE Capture__QueryInterface(ITypedEventHandler* this, const GUID* riid, void** object)
{
	if (object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(riid, &IID_IGraphicsCaptureItemHandler) ||
	    IsEqualGUID(riid, &IID_IDirect3D11CaptureFramePoolHandler) ||
	    IsEqualGUID(riid, &IID_IAgileObject) ||
	    IsEqualGUID(riid, &IID_IUnknown))
	{
		*object = this;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Capture__AddRef(ITypedEventHandler* this)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Capture__Release(ITypedEventHandler* this)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Capture__OnClosed(ITypedEventHandler* this, IInspectable* sender, IInspectable* args)
{
	Capture* Capture = CONTAINING_RECORD(this, struct Capture, OnCloseHandler);
	Capture->CloseCallback();
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Capture__OnFrame(ITypedEventHandler* this, IInspectable* sender, IInspectable* args)
{
	Capture* Capture = CONTAINING_RECORD(this, struct Capture, OnFrameHandler);

	// hmm.. sometimes it succeeds, but returns NULL frame???
	IDirect3D11CaptureFrame* Frame;
	if (SUCCEEDED(Capture->FramePool->vtbl->TryGetNextFrame(Capture->FramePool, &Frame)) && Frame != NULL)
	{
		SIZE Size;
		UINT64 Time;
		HR(Frame->vtbl->get_SystemRelativeTime(Frame, &Time));
		HR(Frame->vtbl->get_ContentSize(Frame, &Size));

		IDirect3DSurface* Surface;
		HR(Frame->vtbl->get_Surface(Frame, &Surface));

		IDirect3DDxgiInterfaceAccess* Access;
		HR(Surface->vtbl->QueryInterface(Surface, &IID_IDirect3DDxgiInterfaceAccess, (LPVOID*)&Access));
		Surface->vtbl->Release(Surface);

		ID3D11Texture2D* Texture;
		HR(Access->vtbl->GetInterface(Access, &IID_ID3D11Texture2D, (LPVOID*)&Texture));
		Access->vtbl->Release(Access);

		D3D11_TEXTURE2D_DESC Desc;
		ID3D11Texture2D_GetDesc(Texture, &Desc);

		RECT Rect = Capture__GetRect(Capture, min(Size.cx, (LONG)Desc.Width), min(Size.cy, (LONG)Desc.Height));
		if (Rect.right > Rect.left && Rect.bottom > Rect.top)
		{
			// call callback only when window has non-zero size, which will happen when it is minimzed
			Capture->FrameCallback(Texture, Rect, Time);
		}
		ID3D11Texture2D_Release(Texture);
		Frame->vtbl->Release(Frame);

		if (Capture->CurrentSize.cx != Size.cx || Capture->CurrentSize.cy != Size.cy)
		{
			Capture->CurrentSize = Size;
			HR(Capture->FramePool->vtbl->Recreate(Capture->FramePool, Capture->Device, DXGI_FORMAT_B8G8R8A8_UNORM, CAPTURE_BUFFER_COUNT, Size));
		}
	}

	return S_OK;
}

static const struct ITypedEventHandlerVtbl Capture__CloseHandlerVtbl = {
	&Capture__QueryInterface,
	&Capture__AddRef,
	&Capture__Release,
	&Capture__OnClosed,
};

static const struct ITypedEventHandlerVtbl Capture__FrameHandlerVtbl = {
	&Capture__QueryInterface,
	&Capture__AddRef,
	&Capture__Release,
	&Capture__OnFrame,
};

BOOL Capture_IsSupported(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 10 version 1903, May 2019 Update (19H1), build 10.0.18362.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 18362);
}

BOOL Capture_CanHideMouseCursor(void)
{
	RTL_OSVERSIONINFOW Version = { sizeof(Version) };
	RtlGetVersion(&Version);

	// available since Windows 10 version 2004, May 2020 Update (20H1), build 10.0.19041.0
	return Version.dwMajorVersion > 10 || (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 19041);
}

void Capture_Init(Capture* Capture, CaptureCloseCallback* CloseCallback, CaptureFrameCallback* FrameCallback)
{
	HR(RoInitialize(RO_INIT_SINGLETHREADED));

	HR(RoGetActivationFactory(GraphicsCaptureItemName, &IID_IGraphicsCaptureItemInterop, (LPVOID*)&Capture->ItemInterop));
	HR(RoGetActivationFactory(Direct3D11CaptureFramePoolName, &IID_IDirect3D11CaptureFramePoolStatics, (LPVOID*)&Capture->FramePoolStatics));

	Capture->OnCloseHandler.vtbl = &Capture__CloseHandlerVtbl;
	Capture->OnFrameHandler.vtbl = &Capture__FrameHandlerVtbl;

	// create dispatcher queue that will call callbacks on main thread as part of message loop
	DispatcherQueueOptions Options =
	{
		.dwSize = sizeof(Options),
		.threadType = DQTYPE_THREAD_CURRENT,
		.apartmentType = DQTAT_COM_NONE,
	};

	// don't really care about object itself, as long as it is created on main thread
	IDispatcherQueueController* Controller;
	HR(CreateDispatcherQueueController(Options, &Controller));

	Capture->CloseCallback = CloseCallback;
	Capture->FrameCallback = FrameCallback;
}

BOOL Capture_CreateForWindow(Capture* Capture, ID3D11Device* Device, HWND Window, BOOL OnlyClientArea)
{
	IDXGIDevice* DxgiDevice;
	HR(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (LPVOID*)&DxgiDevice));
	HR(CreateDirect3D11DeviceFromDXGIDevice(DxgiDevice, (IInspectable**)&Capture->Device));
	IDXGIDevice_Release(DxgiDevice);

	IGraphicsCaptureItem* Item;
	if (SUCCEEDED(Capture->ItemInterop->vtbl->CreateForWindow(Capture->ItemInterop, Window, &IID_IGraphicsCaptureItem, (LPVOID*)&Item)))
	{
		SIZE Size;
		HR(Item->vtbl->get_Size(Item, &Size));

		IDirect3D11CaptureFramePool * FramePool;
		if (SUCCEEDED(Capture->FramePoolStatics->vtbl->Create(Capture->FramePoolStatics, Capture->Device, DXGI_FORMAT_B8G8R8A8_UNORM, CAPTURE_BUFFER_COUNT, Size, &FramePool)))
		{
			Capture->Item = Item;
			Capture->FramePool = FramePool;
			Capture->OnlyClientArea = OnlyClientArea;
			Capture->Window = Window;

			RECT Rect = Capture__GetRect(Capture, Size.cx, Size.cy);
			Capture->CurrentSize.cx = Rect.right - Rect.left;
			Capture->CurrentSize.cy = Rect.bottom - Rect.top;
			Capture->Rect = Rect;

			return TRUE;
		}
		Item->vtbl->Release(Item);
	}

	Capture->Device->vtbl->Release(Capture->Device);
	Capture->Device = NULL;

	return FALSE;
}

BOOL Capture_CreateForMonitor(Capture* Capture, ID3D11Device* Device, HMONITOR Monitor, RECT* Rect)
{
	IDXGIDevice* DxgiDevice;
	HR(ID3D11Device_QueryInterface(Device, &IID_IDXGIDevice, (LPVOID*)&DxgiDevice));
	HR(CreateDirect3D11DeviceFromDXGIDevice(DxgiDevice, (IInspectable**)&Capture->Device));
	IDXGIDevice_Release(DxgiDevice);

	IGraphicsCaptureItem* Item;
	if (SUCCEEDED(Capture->ItemInterop->vtbl->CreateForMonitor(Capture->ItemInterop, Monitor, &IID_IGraphicsCaptureItem, (LPVOID*)&Item)))
	{
		SIZE Size;
		HR(Item->vtbl->get_Size(Item, &Size));

		IDirect3D11CaptureFramePool* FramePool;
		if (SUCCEEDED(Capture->FramePoolStatics->vtbl->Create(Capture->FramePoolStatics, Capture->Device, DXGI_FORMAT_B8G8R8A8_UNORM, 2, Size, &FramePool)))
		{
			Capture->Item = Item;
			Capture->FramePool = FramePool;
			Capture->OnlyClientArea = FALSE;
			Capture->Window = NULL;
			Capture->CurrentSize = Size;
			Capture->Rect = Rect ? *Rect : (RECT) { 0, 0, Size.cx, Size.cy };
			return TRUE;
		}
		Item->vtbl->Release(Item);
	}

	Capture->Device->vtbl->Release(Capture->Device);
	Capture->Device = NULL;

	return FALSE;
}

void Capture_Start(Capture* Capture, BOOL WithMouseCursor)
{
	IGraphicsCaptureSession* Session;
	HR(Capture->FramePool->vtbl->CreateCaptureSession(Capture->FramePool, Capture->Item, &Session));

	IGraphicsCaptureSession2* Session2;
	if (SUCCEEDED(Session->vtbl->QueryInterface(Session, &IID_IGraphicsCaptureSession2, (LPVOID*)&Session2)))
	{
		Session2->vtbl->put_IsCursorCaptureEnabled(Session2, (char)WithMouseCursor);
		Session2->vtbl->Release(Session2);
	}

	HR(Capture->Item->vtbl->add_Closed(Capture->Item, &Capture->OnCloseHandler, &Capture->OnCloseToken));
	HR(Capture->FramePool->vtbl->add_FrameArrived(Capture->FramePool, &Capture->OnFrameHandler, &Capture->OnFrameToken));
	HR(Session->vtbl->StartCapture(Session));

	Capture->Session = Session;
}

void Capture_Stop(Capture* Capture)
{
	if (Capture->Session)
	{
		IClosable* Closable;

		HR(Capture->FramePool->vtbl->remove_FrameArrived(Capture->FramePool, Capture->OnFrameToken));
		HR(Capture->Item->vtbl->remove_Closed(Capture->Item, Capture->OnCloseToken));

		HR(Capture->Session->vtbl->QueryInterface(Capture->Session, &IID_IClosable, (LPVOID*)&Closable));
		Closable->vtbl->Close(Closable);
		Closable->vtbl->Release(Closable);

		HR(Capture->FramePool->vtbl->QueryInterface(Capture->FramePool, &IID_IClosable, (LPVOID*)&Closable));
		Closable->vtbl->Close(Closable);
		Closable->vtbl->Release(Closable);

		Capture->Session->vtbl->Release(Capture->Session);
		Capture->Session = NULL;
	}

	if (Capture->Item)
	{
		Capture->FramePool->vtbl->Release(Capture->FramePool);
		Capture->Item->vtbl->Release(Capture->Item);
		Capture->Item = NULL;
	}

	if (Capture->Device)
	{
		Capture->Device->vtbl->Release(Capture->Device);
		Capture->Device = NULL;
	}
}
