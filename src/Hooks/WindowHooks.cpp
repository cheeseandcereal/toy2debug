#include "WindowHooks.hpp"
#include "Mapper.hpp"
#include "ToyTypes.hpp"

#include <algorithm>
#include <cstring>
#include <print>

namespace
{
	MONITORINFOEXA g_monitor { sizeof(MONITORINFOEXA) };

	BOOL CALLBACK findMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
	{
		MONITORINFOEXA info { sizeof(info) };
		if (GetMonitorInfoA(monitor, &info) && std::strcmp(info.szDevice, g_monitor.szDevice) == 0)
		{
			*reinterpret_cast<MONITORINFOEXA*>(context) = info;
			return FALSE;
		}
		return TRUE;
	}

	SIZE windowSize(HWND window, int64_t width, int64_t height, UINT dpi = 0)
	{
		using GetDpi = UINT(WINAPI*)(HWND);
		using AdjustRect = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
		static auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow"));
		static auto adjustRect = reinterpret_cast<AdjustRect>(GetProcAddress(GetModuleHandleA("user32.dll"), "AdjustWindowRectExForDpi"));
		if (! dpi && getDpi)
			dpi = getDpi(window);

		RECT frame {};
		const DWORD style = GetWindowLongA(window, GWL_STYLE);
		const DWORD exStyle = GetWindowLongA(window, GWL_EXSTYLE);
		const BOOL menu = GetMenu(window) != nullptr;
		if (! adjustRect || ! dpi || ! adjustRect(&frame, style, menu, exStyle, dpi))
		{
			frame = {};
			if (! AdjustWindowRectEx(&frame, style, menu, exStyle))
				return {};
		}

		const int64_t outerWidth = width + frame.right - frame.left;
		const int64_t outerHeight = height + frame.bottom - frame.top;
		if (width <= 0 || height <= 0 || outerWidth > INT32_MAX || outerHeight > INT32_MAX)
			return {};
		return { static_cast<LONG>(outerWidth), static_cast<LONG>(outerHeight) };
	}
}

void WindowHooks::selectMonitor(HWND source)
{
	if (! source)
		source = GetForegroundWindow();

	HMONITOR monitor = nullptr;
	if (source && source != GetDesktopWindow() && source != GetShellWindow() && IsWindow(source))
		monitor = MonitorFromWindow(source, MONITOR_DEFAULTTONEAREST);
	if (! monitor)
	{
		POINT point {};
		GetCursorPos(&point);
		monitor = MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
	}
	MONITORINFOEXA info { sizeof(info) };
	if (GetMonitorInfoA(monitor, &info))
		g_monitor = info;
}

MONITORINFOEXA WindowHooks::monitorInfo()
{
	MONITORINFOEXA info { sizeof(info) };
	EnumDisplayMonitors(nullptr, nullptr, findMonitor, reinterpret_cast<LPARAM>(&info));
	if (info.szDevice[0])
		g_monitor = info;
	else if (GetMonitorInfoA(MonitorFromRect(&g_monitor.rcMonitor, MONITOR_DEFAULTTONEAREST), &info))
		return info;
	return g_monitor;
}

void WindowHooks::enableNonClientDpiScaling(HWND window)
{
	using EnableScaling = BOOL(WINAPI*)(HWND);
	static auto enableScaling = reinterpret_cast<EnableScaling>(GetProcAddress(GetModuleHandleA("user32.dll"), "EnableNonClientDpiScaling"));
	if (enableScaling)
		enableScaling(window);
}

void WindowHooks::positionWindow(HWND window, Settings::WindowStyle style, int32_t width, int32_t height)
{
	const auto info = monitorInfo();
	const bool windowed = style == Settings::Windowed;
	const DWORD windowStyle = windowed ? WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX : WS_POPUP;
	const DWORD exStyle = style == Settings::Fullscreen ? WS_EX_TOPMOST : WS_EX_APPWINDOW;
	SetWindowLongA(window, GWL_STYLE, windowStyle | (GetWindowLongA(window, GWL_STYLE) & WS_VISIBLE));
	SetWindowLongA(window, GWL_EXSTYLE, exStyle);

	const RECT area = windowed ? info.rcWork : info.rcMonitor;
	// Move before querying DPI so decorations use the destination window's DPI.
	if (windowed)
		SetWindowPos(window, nullptr, area.left, area.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	const SIZE size = windowSize(window, width, height);
	if (! size.cx || ! size.cy)
	{
		std::println("[WindowHooks]: Cannot size window to {}x{}.", width, height);
		return;
	}

	const LONG x = area.left + (windowed ? (std::max)(0L, (area.right - area.left - size.cx) / 2) : 0);
	const LONG y = area.top + (windowed ? (std::max)(0L, (area.bottom - area.top - size.cy) / 2) : 0);
	if (! SetWindowPos(window, style == Settings::Fullscreen ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_FRAMECHANGED))
		std::println("[WindowHooks]: SetWindowPos failed: {}.", GetLastError());
}

LRESULT WINAPI windowWndProc(HWND hWnd, uint32_t msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_NCCREATE:
			WindowHooks::enableNonClientDpiScaling(hWnd);
			break;

		case WM_MOVE:
		case WM_SIZE: {
			auto* framework = *Mapper::mapAddress<CD3DFramework**>(0x484008);
			if (framework && framework->hWnd == hWnd && ! framework->bIsFullscreen)
			{
				GetClientRect(hWnd, &framework->rcScreenRect);
				MapWindowPoints(hWnd, nullptr, reinterpret_cast<LPPOINT>(&framework->rcScreenRect), 2);
			}
			break;
		}

		case WM_GETMINMAXINFO:
			reinterpret_cast<MINMAXINFO*>(lParam)->ptMaxTrackSize = { INT32_MAX, INT32_MAX };
			return 0;

		case WM_GETDPISCALEDSIZE: {
			auto* framework = *Mapper::mapAddress<CD3DFramework**>(0x484008);
			if (framework && framework->hWnd == hWnd && framework->bIsFullscreen)
				return FALSE;
			RECT client {}, outer {};
			if (! GetClientRect(hWnd, &client) || ! GetWindowRect(hWnd, &outer))
				return FALSE;
			auto& pending = *reinterpret_cast<SIZE*>(lParam);
			// Preserve pending client pixels, including a resize concurrent with the DPI change.
			const int64_t width = int64_t(pending.cx) - (int64_t(outer.right) - outer.left) + client.right;
			const int64_t height = int64_t(pending.cy) - (int64_t(outer.bottom) - outer.top) + client.bottom;
			const SIZE size = windowSize(hWnd, width, height, static_cast<UINT>(wParam));
			if (! size.cx || ! size.cy)
				return FALSE;
			pending = size;
			return TRUE;
		}

		case WM_DPICHANGED: {
			auto* framework = *Mapper::mapAddress<CD3DFramework**>(0x484008);
			if (framework && framework->hWnd == hWnd && framework->bIsFullscreen)
				return 0;
			const auto& rect = *reinterpret_cast<const RECT*>(lParam);
			SetWindowPos(hWnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
			return 0;
		}

		case WM_PAINT: {
			PAINTSTRUCT ps;
			BeginPaint(hWnd, &ps);
			EndPaint(hWnd, &ps);
			return 0;
		}

		case WM_DESTROY:
		case WM_NCDESTROY: {
			auto* wndData = Mapper::mapAddress<WindowData*>(0x134488);
			wndData->wndIsExiting = 1;
			break;
		}
	}

	return DefWindowProcA(hWnd, msg, wParam, lParam);
}

int32_t CON_STDCALL hook_BuildWindow()
{
	if (g_settings.width <= 0 || g_settings.height <= 0)
		return FALSE;

	auto* wndData = Mapper::mapAddress<WindowData*>(0x134488);

	memset(&wndData->wndClass, 0, sizeof(wndData->wndClass));
	wndData->wndClass.cbSize = 48;

	CoInitialize(0);

	constexpr char kClassName[] = "Toy2Debug - Toy Story 2: Buzz Lightyear to the Rescue";

	wndData->wndClass.style = CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
	wndData->wndClass.lpfnWndProc = windowWndProc;
	wndData->wndClass.cbClsExtra = 0;
	wndData->wndClass.cbWndExtra = 0;
	wndData->wndClass.hInstance = wndData->hInstance;
	wndData->wndClass.hIcon = LoadIconA(wndData->hInstance, "AppIcon");
	wndData->wndClass.lpszMenuName = "AppMenu";
	wndData->wndClass.lpszClassName = kClassName;
	wndData->wndClass.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

	if (! RegisterClassExA(&wndData->wndClass))
	{
		constexpr DWORD formatFlags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM;
		char* buffer = nullptr;

		FormatMessageA(formatFlags, 0, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, 0, 0);
		MessageBoxA(0, buffer, "GetLastError", MB_ICONINFORMATION | MB_OK);
		LocalFree(buffer);

		return FALSE;
	}

	const RECT monitor = WindowHooks::monitorInfo().rcMonitor;
	HWND window = CreateWindowExA(WS_EX_APPWINDOW, kClassName, kClassName, WS_POPUP, monitor.left, monitor.top, g_settings.width, g_settings.height, 0, 0, wndData->hInstance, 0);

	wndData->mainHwnd = window;

	if (! window)
	{
		std::println("Failed to create main window!");
		return FALSE;
	}

	WindowHooks::positionWindow(window, g_settings.windowStyle, g_settings.width, g_settings.height);
	ShowWindow(window, SW_SHOW);
	UpdateWindow(window);

	SetForegroundWindow(window);
	SetFocus(window);
	SetActiveWindow(window);

	wndData->hAccTable = LoadAcceleratorsA(wndData->hInstance, "AppAccel");

	static uint32_t g_sysParamsInfo = 0;
	if (SystemParametersInfoA(SPI_SCREENSAVERRUNNING, TRUE, &g_sysParamsInfo, 0))
	{
		atexit([] {
			// being faithful
			SystemParametersInfoA(SPI_SCREENSAVERRUNNING, g_sysParamsInfo, 0, 0);
		});
	}

	ShowCursor(TRUE);

	return TRUE;
}

bool WindowHooks::init()
{
	// Address definitions
	const int32_t kBuildWindow = Mapper::mapAddress(0xA6B30);

	// Init Hooks
	Hook::createHook(kBuildWindow, &hook_BuildWindow);

	return ! Hook::hasFailedHooks();
}
