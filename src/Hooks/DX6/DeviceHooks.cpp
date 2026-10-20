#include "DeviceHooks.hpp"
#include "Mapper.hpp"
#include "Hooks/WindowHooks.hpp"

#include <cstring>
#include <new>
#include <print>
#include <vector>

namespace DX6
{
	static CD3DFramework g_framework {};
	static HRESULT(CON_CDECL* g_originalPresentFrame)() = nullptr;

	namespace
	{
		struct DirectDrawOutput
		{
			const char* name;
			GUID guid {};
			bool found = false;
		};

		BOOL CALLBACK findOutput(GUID* guid, LPSTR, LPSTR, void* context, HMONITOR monitor)
		{
			auto& output = *static_cast<DirectDrawOutput*>(context);
			MONITORINFOEXA info { sizeof(info) };
			if (guid && GetMonitorInfoA(monitor, &info) && std::strcmp(info.szDevice, output.name) == 0)
			{
				output.guid = *guid;
				output.found = true;
				return DDENUMRET_CANCEL;
			}
			return DDENUMRET_OK;
		}

		HRESULT createDirectDraw(GUID* guid)
		{
			LPDIRECTDRAW draw = nullptr;
			HRESULT hr = DirectDrawCreate(guid, &draw, nullptr);
			if (SUCCEEDED(hr))
			{
				hr = draw->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&g_framework.pDD));
				draw->Release();
			}
			return hr;
		}

		bool useBorderless()
		{
			if (g_framework.pDD)
			{
				HRESULT hr = g_framework.pDD->RestoreDisplayMode();
				if (FAILED(hr) && hr != DDERR_NOEXCLUSIVEMODE)
					return false;
				if (FAILED(g_framework.pDD->SetCooperativeLevel(g_framework.hWnd, DDSCL_NORMAL)))
					return false;
				g_framework.pDD->Release();
				g_framework.pDD = nullptr;
			}
			g_framework.bIsFullscreen = 0;
			// A default normal-mode object supports Windows' multi-monitor emulation.
			if (FAILED(createDirectDraw(nullptr)) || FAILED(g_framework.pDD->SetCooperativeLevel(g_framework.hWnd, DDSCL_NORMAL)))
				return false;
			WindowHooks::positionWindow(g_framework.hWnd, Settings::Borderless, g_framework.dwRenderWidth, g_framework.dwRenderHeight);
			std::println("[DX6]: Using borderless on {}.", WindowHooks::monitorInfo().szDevice);
			return true;
		}

		struct DibInfo
		{
			BITMAPINFOHEADER header {};
			DWORD masks[3] {};
		};

		HRESULT copyDib(const DDSURFACEDESC2& surface, DibInfo& info, std::vector<uint8_t>& pixels)
		{
			const auto& format = surface.ddpfPixelFormat;
			const DWORD bits = format.dwRGBBitCount;
			if ((format.dwFlags & (DDPF_RGB | DDPF_FOURCC)) != DDPF_RGB || (bits != 16 && bits != 24 && bits != 32))
				return DDERR_INVALIDPIXELFORMAT;
			if (bits == 24 && (format.dwRBitMask != 0xff0000 || format.dwGBitMask != 0xff00 || format.dwBBitMask != 0xff))
				return DDERR_INVALIDPIXELFORMAT;
			if (! surface.lpSurface || ! surface.dwWidth || ! surface.dwHeight || surface.dwWidth > INT32_MAX || surface.dwHeight > INT32_MAX || ! (surface.dwFlags & DDSD_PITCH))
				return DDERR_INVALIDPARAMS;

			const uint64_t rowBytes = uint64_t(surface.dwWidth) * (bits / 8);
			const uint64_t stride = (rowBytes + 3) & ~uint64_t(3);
			const uint64_t bytes = stride * surface.dwHeight;
			const int64_t pitch = surface.lPitch;
			const uint64_t pitchBytes = pitch < 0 ? -pitch : pitch;
			if (pitchBytes < rowBytes || bytes > pixels.max_size() || pitchBytes * (surface.dwHeight - 1) + rowBytes > PTRDIFF_MAX)
				return DDERR_INVALIDPARAMS;
			try
			{
				pixels.resize(static_cast<size_t>(bytes));
			}
			catch (const std::bad_alloc&)
			{
				return E_OUTOFMEMORY;
			}
			for (DWORD y = 0; y < surface.dwHeight; ++y)
				std::memcpy(pixels.data() + size_t(y * stride), static_cast<const uint8_t*>(surface.lpSurface) + ptrdiff_t(y * pitch), size_t(rowBytes));

			info = {};
			info.header.biSize = sizeof(BITMAPINFOHEADER);
			info.header.biWidth = LONG(surface.dwWidth);
			info.header.biHeight = -LONG(surface.dwHeight);
			info.header.biPlanes = 1;
			info.header.biBitCount = WORD(bits);
			info.header.biCompression = bits == 24 ? BI_RGB : BI_BITFIELDS;
			info.masks[0] = format.dwRBitMask;
			info.masks[1] = format.dwGBitMask;
			info.masks[2] = format.dwBBitMask;
			return DD_OK;
		}

		HRESULT presentDib(HDC dc, const RECT& client)
		{
			DDSURFACEDESC2 surface {};
			surface.dwSize = sizeof(surface);
			HRESULT hr = g_framework.pddsBackBuffer->Lock(nullptr, &surface, DDLOCK_READONLY | DDLOCK_WAIT, nullptr);
			if (FAILED(hr))
				return hr;
			DibInfo info;
			// One staging buffer for the game's single presentation thread.
			static std::vector<uint8_t> pixels;
			hr = copyDib(surface, info, pixels);
			HRESULT unlocked = g_framework.pddsBackBuffer->Unlock(nullptr);
			if (FAILED(unlocked))
				return unlocked;
			if (FAILED(hr))
				return hr;

			// Release the video-memory lock before GDI reads the owned pixel copy.
			int lines = StretchDIBits(dc, 0, 0, client.right, client.bottom, 0, 0, surface.dwWidth, surface.dwHeight, pixels.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, SRCCOPY);
			RECT clip {};
			return lines > 0 || (lines == 0 && GetClipBox(dc, &clip) == NULLREGION) ? DD_OK : E_FAIL;
		}

		HRESULT presentGdi(const RECT& client)
		{
			HDC dc = GetDC(g_framework.hWnd);
			if (! dc)
				return E_FAIL;
			RECT clip {};
			int region = GetClipBox(dc, &clip);
			HRESULT hr = DD_OK;
			if (region == ERROR)
				hr = E_FAIL;
			else if (region != NULLREGION)
				hr = SetStretchBltMode(dc, COLORONCOLOR) ? presentDib(dc, client) : E_FAIL;
			if (! ReleaseDC(g_framework.hWnd, dc) && SUCCEEDED(hr))
				hr = E_FAIL;
			return hr;
		}

		HRESULT presentWindowed()
		{
			RECT client {};
			if (! GetClientRect(g_framework.hWnd, &client))
				return E_FAIL;
			if (IsIconic(g_framework.hWnd) || IsRectEmpty(&client))
				return DD_OK;
			RECT destination = client;
			if (! ClientToScreen(g_framework.hWnd, reinterpret_cast<POINT*>(&destination)) || ! ClientToScreen(g_framework.hWnd, reinterpret_cast<POINT*>(&destination.right)))
				return E_FAIL;
			RECT source { 0, 0, g_framework.dwRenderWidth, g_framework.dwRenderHeight };
			HRESULT hr = g_framework.pddsFrontBuffer->Blt(&destination, g_framework.pddsBackBuffer, &source, DDBLT_WAIT, nullptr);
			if (hr == DDERR_INVALIDRECT || hr == DDERR_UNSUPPORTED || hr == DDERR_NOBLTHW || hr == DDERR_NOSTRETCHHW)
				return presentGdi(client);
			return hr;
		}
	}

	HRESULT CON_CDECL hook_PresentFrame()
	{
		if (g_framework.bIsFullscreen || ! g_framework.hWnd || ! g_framework.pddsFrontBuffer || ! g_framework.pddsBackBuffer)
			return g_originalPresentFrame();
		HRESULT hr = presentWindowed();
		static HRESULT previousResult = DD_OK;
		if (FAILED(hr) && hr != previousResult)
			std::println("[DX6]: Presentation failed: 0x{:08X}.", uint32_t(hr));
		previousResult = hr;
		return hr;
	}

	bool DeviceHooks::initialize(HWND hWnd, Settings::WindowStyle windowStyle, int32_t width, int32_t height)
	{
		g_framework.hWnd = hWnd;
		g_framework.bIsFullscreen = (windowStyle == Settings::WindowStyle::Fullscreen) ? 1 : 0;
		g_framework.dwRenderWidth = width;
		g_framework.dwRenderHeight = height;

		// Create DirectDraw
		bool result = createDirectDrawInterface();
		if (! result)
		{
			std::println("[DX6]: createDirectDrawInterface failed.");
			goto CLEANUP_LBL;
		}

		// Create Direct3D interface
		result = createD3DInterface();
		if (! result)
		{
			std::println("[DX6]: createD3DInterface failed.");
			goto CLEANUP_LBL;
		}

		// Select device if specified
		result = selectDevice();
		if (! result)
		{
			std::println("[DX6]: selectDevice failed.");
			goto CLEANUP_LBL;
		}

		// Create surfaces
		result = createSurfaces(width, height);
		if (! result)
		{
			std::println("[DX6]: createSurfaces failed.");
			goto CLEANUP_LBL;
		}

		// Create Z-buffer if we have a device
		result = createZBuffer();
		if (! result)
		{
			std::println("[DX6]: createZBuffer failed.");
			goto CLEANUP_LBL;
		}

		// Create device
		result = createDevice();
		if (! result)
		{
			std::println("[DX6]: createDevice failed.");
			goto CLEANUP_LBL;
		}

		// Create viewport
		result = createViewport();
		if (! result)
		{
			std::println("[DX6]: createViewport failed.");
			goto CLEANUP_LBL;
		}

		// Setup slot 0 with back buffer info
		g_framework.slots[0].valid = 1;
		g_framework.slots[0].width = width;
		g_framework.slots[0].height = height;
		g_framework.slots[0].surface1 = g_framework.pddsBackBuffer;
		g_framework.slots[0].surface2 = g_framework.pddsZBuffer;

		g_framework.initialized = 1;
		return result;

	CLEANUP_LBL:
		cleanup();

		return result;
	}

	bool DeviceHooks::createDirectDrawInterface()
	{
		HRESULT hr;
		const auto monitor = WindowHooks::monitorInfo();
		DirectDrawOutput output { monitor.szDevice };
		if (g_framework.bIsFullscreen && ! (monitor.dwFlags & MONITORINFOF_PRIMARY))
		{
			hr = DirectDrawEnumerateExA(findOutput, &output, DDENUM_ATTACHEDSECONDARYDEVICES);
			if (FAILED(hr) || ! output.found)
				return useBorderless();
		}

		// Create DirectDraw
		hr = createDirectDraw(output.found ? &output.guid : nullptr);
		if (FAILED(hr))
			return output.found ? useBorderless() : false;

		// Set cooperative level
		DWORD coopFlags = g_framework.bIsFullscreen ? (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT) : DDSCL_NORMAL;

		hr = g_framework.pDD->SetCooperativeLevel(g_framework.hWnd, coopFlags);

		if (g_framework.bIsFullscreen && FAILED(hr))
			return useBorderless();
		if (FAILED(hr))
			return false;
		if (g_framework.bIsFullscreen)
		{
			hr = g_framework.pDD->SetDisplayMode(g_framework.dwRenderWidth, g_framework.dwRenderHeight, g_settings.use32BitColors ? 32 : 16, 0, 0);
			DDSURFACEDESC2 mode {};
			mode.dwSize = sizeof(mode);
			if (SUCCEEDED(hr))
				hr = g_framework.pDD->GetDisplayMode(&mode);
			RECT window {};
			const auto currentMonitor = WindowHooks::monitorInfo();
			if (FAILED(hr) || mode.dwWidth != DWORD(g_framework.dwRenderWidth) || mode.dwHeight != DWORD(g_framework.dwRenderHeight) || std::strcmp(currentMonitor.szDevice, monitor.szDevice) != 0
				|| ! GetWindowRect(g_framework.hWnd, &window) || ! PtInRect(&currentMonitor.rcMonitor, { window.left, window.top }))
			{
				// Validate the result: accepting a mode request does not guarantee it was applied.
				return useBorderless();
			}
		}

		return true;
	}

	bool DeviceHooks::createD3DInterface()
	{
		HRESULT hr = g_framework.pDD->QueryInterface(IID_IDirect3D3, (void**)&g_framework.pD3D);
		return SUCCEEDED(hr);
	}

	bool DeviceHooks::selectDevice()
	{
		D3DFINDDEVICESEARCH search = { 0 };
		D3DFINDDEVICERESULT result = { 0 };

		search.dwSize = sizeof(D3DFINDDEVICESEARCH);
		search.dwFlags = D3DFDS_GUID;
		search.guid = IID_IDirect3DHALDevice;

		result.dwSize = sizeof(D3DFINDDEVICERESULT);

		HRESULT hr = g_framework.pD3D->FindDevice(&search, &result);
		if (FAILED(hr))
			return false;

		// Copy device description (hardware if available, otherwise software)
		if (result.ddHwDesc.dwFlags)
		{
			memcpy(&g_framework.ddDeviceDesc, &result.ddHwDesc, sizeof(D3DDEVICEDESC));
			g_framework.dwDeviceMemType = DDSCAPS_VIDEOMEMORY;
		}
		else
		{
			memcpy(&g_framework.ddDeviceDesc, &result.ddSwDesc, sizeof(D3DDEVICEDESC));
			g_framework.dwDeviceMemType = DDSCAPS_SYSTEMMEMORY;
		}

		// Enumerate Z-buffer formats
		memset(&g_framework.ddpfZBuffer, 0, sizeof(DDPIXELFORMAT));
		g_framework.ddpfZBuffer.dwFlags = DDPF_ZBUFFER;

		g_framework.pD3D->EnumZBufferFormats(IID_IDirect3DHALDevice, DeviceHooks::enumZBufferCallback, &g_framework.ddpfZBuffer);

		if (g_framework.ddpfZBuffer.dwSize != sizeof(DDPIXELFORMAT))
			return false;

		return true;
	}

	HRESULT CALLBACK DeviceHooks::enumZBufferCallback(LPDDPIXELFORMAT format, LPVOID context)
	{
		LPDDPIXELFORMAT target = (LPDDPIXELFORMAT)context;

		if (! format || ! target)
			return D3DENUMRET_OK;

		// Only accept matching Z-buffer types
		if (format->dwFlags != target->dwFlags)
			return D3DENUMRET_OK;

		// Prefer 16-bit Z-buffers
		if (format->dwZBufferBitDepth == 16)
		{
			memcpy(target, format, sizeof(DDPIXELFORMAT));
			return D3DENUMRET_CANCEL; // Found what we want
		}

		// Accept any Z-buffer if we don't have one yet
		if (target->dwSize == 0)
			memcpy(target, format, sizeof(DDPIXELFORMAT));

		return D3DENUMRET_OK;
	}

	bool DeviceHooks::createSurfaces(int32_t width, int32_t height)
	{
		DDSURFACEDESC2 desc;
		HRESULT hr;

		if (g_framework.bIsFullscreen)
		{
			// Create fullscreen primary surface with flip chain
			initSurfaceDesc(&desc, DDSD_CAPS | DDSD_BACKBUFFERCOUNT, 0);
			desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
			desc.dwBackBufferCount = 1;

			hr = g_framework.pDD->CreateSurface(&desc, &g_framework.pddsFrontBuffer, nullptr);

			if (FAILED(hr))
				return false;

			// Get back buffer
			DDSCAPS2 caps = { 0 };
			caps.dwCaps = DDSCAPS_BACKBUFFER;
			hr = g_framework.pddsFrontBuffer->GetAttachedSurface(&caps, &g_framework.pddsBackBuffer);

			if (FAILED(hr))
				return false;

			// Setup rects
			SetRect(&g_framework.rcScreenRect, 0, 0, width, height);
		}
		else
		{
			// Windowed mode - create primary surface
			initSurfaceDesc(&desc, DDSD_CAPS, 0);
			desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

			hr = g_framework.pDD->CreateSurface(&desc, &g_framework.pddsFrontBuffer, nullptr);
			if (FAILED(hr))
				return false;

			// Create clipper for windowed mode
			LPDIRECTDRAWCLIPPER clipper = nullptr;
			hr = g_framework.pDD->CreateClipper(0, &clipper, nullptr);
			if (FAILED(hr))
				return false;

			hr = clipper->SetHWnd(0, g_framework.hWnd);
			if (SUCCEEDED(hr))
				hr = g_framework.pddsFrontBuffer->SetClipper(clipper);
			clipper->Release();
			if (FAILED(hr))
				return false;

			// Create offscreen back buffer
			initSurfaceDesc(&desc, DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS, 0);
			desc.dwWidth = width;
			desc.dwHeight = height;
			desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;

			hr = g_framework.pDD->CreateSurface(&desc, &g_framework.pddsBackBuffer, nullptr);

			if (FAILED(hr))
				return false;

			// Setup rects for windowed mode
			GetClientRect(g_framework.hWnd, &g_framework.rcScreenRect);
			ClientToScreen(g_framework.hWnd, (LPPOINT)&g_framework.rcScreenRect);
			ClientToScreen(g_framework.hWnd, (LPPOINT)&g_framework.rcScreenRect.right);
		}

		SetRect(&g_framework.rcViewportRect, 0, 0, width, height);
		g_framework.pddsRenderTarget = g_framework.pddsBackBuffer;
		g_framework.pddsRenderTarget->AddRef();

		return true;
	}

	bool DeviceHooks::createZBuffer()
	{
		// Check if device needs Z-buffer
		if (g_framework.ddDeviceDesc.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_ZBUFFERLESSHSR)
			return false;

		DDSURFACEDESC2 desc;
		initSurfaceDesc(&desc, DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | DDSD_PIXELFORMAT, 0);

		desc.dwWidth = g_framework.dwRenderWidth;
		desc.dwHeight = g_framework.dwRenderHeight;
		desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | g_framework.dwDeviceMemType;
		memcpy(&desc.ddpfPixelFormat, &g_framework.ddpfZBuffer, sizeof(DDPIXELFORMAT));

		HRESULT hr = g_framework.pDD->CreateSurface(&desc, &g_framework.pddsZBuffer, nullptr);
		if (FAILED(hr))
			return false;

		// Attach Z-buffer to render target
		hr = g_framework.pddsRenderTarget->AddAttachedSurface(g_framework.pddsZBuffer);

		return SUCCEEDED(hr);
	}

	bool DeviceHooks::createDevice()
	{
		DDSURFACEDESC2 desc;
		desc.dwSize = sizeof(DDSURFACEDESC2);
		g_framework.pDD->GetDisplayMode(&desc);

		if (desc.ddpfPixelFormat.dwRGBBitCount <= 8)
			return false;

		HRESULT hr = g_framework.pD3D->CreateDevice(IID_IDirect3DHALDevice, g_framework.pddsRenderTarget, &g_framework.pd3dDevice, nullptr);

		return SUCCEEDED(hr);
	}

	bool DeviceHooks::createViewport()
	{
		HRESULT hr = g_framework.pD3D->CreateViewport(&g_framework.pvViewport, nullptr);
		if (FAILED(hr))
			return false;

		hr = g_framework.pd3dDevice->AddViewport(g_framework.pvViewport);
		if (FAILED(hr))
			return false;

		D3DVIEWPORT2 viewport = {};
		viewport.dwSize = sizeof(D3DVIEWPORT2);
		viewport.dwWidth = g_framework.dwRenderWidth;
		viewport.dwHeight = g_framework.dwRenderHeight;
		viewport.dvClipX = -1.0f;
		viewport.dvClipY = 1.0f;
		viewport.dvClipWidth = 2.0f;
		viewport.dvClipHeight = 2.0f;
		viewport.dvMaxZ = 1.0f;

		hr = g_framework.pvViewport->SetViewport2(&viewport);
		if (FAILED(hr))
			return false;

		hr = g_framework.pd3dDevice->SetCurrentViewport(g_framework.pvViewport);
		return SUCCEEDED(hr);
	}

	void DeviceHooks::cleanup()
	{
		if (g_framework.pvViewport)
		{
			g_framework.pvViewport->Release();
			g_framework.pvViewport = nullptr;
		}

		if (g_framework.pd3dDevice)
		{
			g_framework.pd3dDevice->Release();
			g_framework.pd3dDevice = nullptr;
		}

		if (g_framework.pddsZBuffer)
		{
			g_framework.pddsZBuffer->Release();
			g_framework.pddsZBuffer = nullptr;
		}

		if (g_framework.pddsRenderTarget && g_framework.pddsRenderTarget != g_framework.pddsFrontBuffer)
		{
			g_framework.pddsRenderTarget->Release();
			g_framework.pddsRenderTarget = nullptr;
		}

		if (g_framework.pddsBackBuffer)
		{
			g_framework.pddsBackBuffer->Release();
			g_framework.pddsBackBuffer = nullptr;
		}

		if (g_framework.pddsFrontBuffer)
		{
			g_framework.pddsFrontBuffer->Release();
			g_framework.pddsFrontBuffer = nullptr;
		}

		if (g_framework.pD3D)
		{
			g_framework.pD3D->Release();
			g_framework.pD3D = nullptr;
		}

		if (g_framework.pDD)
		{
			if (g_framework.bIsFullscreen)
				g_framework.pDD->RestoreDisplayMode();
			g_framework.pDD->SetCooperativeLevel(g_framework.hWnd, DDSCL_NORMAL);
			g_framework.pDD->Release();
			g_framework.pDD = nullptr;
		}

		g_framework.initialized = 0;
	}

	void DeviceHooks::initSurfaceDesc(LPDDSURFACEDESC2 desc, uint32_t flags, uint32_t caps)
	{
		memset(desc, 0, sizeof(DDSURFACEDESC2));
		desc->dwSize = sizeof(DDSURFACEDESC2);
		desc->dwFlags = flags;
		desc->ddsCaps.dwCaps = caps;
	}

	void CON_STDCALL hook_RunModeSelect()
	{
		auto* wndData = Mapper::mapAddress<WindowData*>(0x134488);

		*Mapper::mapAddress<CD3DFramework**>(0x484008) = &g_framework;

		bool success = DeviceHooks::initialize(wndData->mainHwnd, g_settings.windowStyle, g_settings.width, g_settings.height);

		std::println("[ModeSelect]: Initialize returned {}", success ? "Success" : "Fail");

		return;
	}

	bool DeviceHooks::init()
	{
		// Address definitions
		const int32_t kRunModeSelect = Mapper::mapAddress(0x12B50);
		const int32_t kPresentFrame = Mapper::mapAddress(0xABD40);

		// Init Hooks
		Hook::createHook(kRunModeSelect, &hook_RunModeSelect);
		Hook::createHook(kPresentFrame, &hook_PresentFrame, &g_originalPresentFrame);

		return ! Hook::hasFailedHooks();
	}
}
