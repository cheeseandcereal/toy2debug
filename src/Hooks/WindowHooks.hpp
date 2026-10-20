#pragma once

#include "Hook.hpp"
#include "Settings.hpp"

#include <windows.h>

class WindowHooks : public Hook
{
public:
	WindowHooks()
		: Hook("WindowHooks") {};

	static void selectMonitor(HWND source = nullptr);
	// A fallback describes the nearest available monitor without changing the selection.
	static MONITORINFOEXA monitorInfo();
	static void enableNonClientDpiScaling(HWND window);
	// Applies game style and placement; width and height are client pixels.
	static void positionWindow(HWND window, Settings::WindowStyle style, int32_t width, int32_t height);

private:
	bool init() override;
};
