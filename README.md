# Toy2Debug Loader

Toy2Debug is a modern compatibility and patching tool for **Toy Story 2 PC**, built to finally make the game playable on modern Windows systems without hacks, broken menus, or external wrappers.

Expect bugs. The goal is to fix Toy Story 2 once and for all so it can be enjoyed properly on modern PCs.

<p align="center">
<img width="595" height="502" alt="image" src="https://github.com/user-attachments/assets/bff5c6ba-6ab8-4c7b-8aad-292e0f0a85f9" />
</p>

## Features

- Real exclusive fullscreen, windowed, and borderless modes  
- Proper 60 FPS framerate fix  
- Widescreen support  
- Support for resolutions above 1080p including 1440p and 4K  
- Big render distance  
- Disable all cutscenes  
- Skip ESRB and copyright movies  
- Bypass registry key checks  
- Custom resolution selection without the original broken menu  

## Modern Window and Resolution Handling

The game uses the resolution and mode selected in the launcher and starts on the monitor containing the launcher when Launch Game is clicked. The launcher initially follows the foreground window, or the cursor when no application window is available.

Windowed and borderless modes keep the requested render resolution, including resolutions larger than a monitor. The full rendered image scales to the client area when the window changes size. Moving a window between monitors does not change its render resolution.

Fullscreen selects the target monitor's DirectDraw device and attempts exclusive mode. If the device cannot provide that mode on the selected monitor, the game uses borderless mode at the requested resolution.

Windowed presentation uses DirectDraw first. Unsupported blits use Windows GDI to display the back buffer in window coordinates, including on secondary monitors and in partially off-screen windows. This fallback copies pixels through CPU memory and can cost more at high resolutions.

## Release

Currently you will need to build the project, a release is coming soon.

## How It Works

This tool is NOT a dll injector, it uses a **Run PE from memory** approach.

What this means:

- The game executable is mapped into the loaders virtual memory and essentially sandboxed.
- The game runs directly from the loader process
- Much friendlier to Windows Defender  

## Requirements

- **English version of Toy Story 2 PC**  
  Multi-language support is planned  

- Windows PC
- May work on Linux with Wine (Untested)

## Status

If you encounter issues, crashes, odd behaviour or would like a feature request, Please open a pull request or send a bug report in.
