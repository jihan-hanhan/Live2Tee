# Live2Tee
A project for Teeworlds fans that lets you add your Tee to your videos or livestreams, turning it into your virtual avatar.

[ENG]

[[中文]](README_zh-cn.md)

## Usage (Example with OBS)
1. Run `Live2Tee.exe`.
2. Place `game.png` and `emoticons.png` in the `assets` folder located in the same directory as the executable, and put your desired skin images in the `assets/skins` folder.
3. Press `F9` to open the configuration interface. Here you can control zoom, window size, switch Tee skins, and reset the mouse position.
4. Add a `Browser` source in OBS, enter the URL provided in the configuration interface, and adjust the size and position until you are satisfied.
5. Enjoy!

### Features
- Press **any key** on the keyboard, and the Tee will display the corresponding emote (speech bubble + eye expression). Emotes and actions can play simultaneously without interrupting each other.
- Press the `Left Mouse Button` to make the Tee swing its hammer.
- Press the `Right Mouse Button` to make the Tee shoot its hook.
- The Tee's gaze follows your mouse movement (global input, no window focus required).

## Building from Source
### Dependencies
- CMake >= 3.16
- Ninja (or any other CMake generator)
- A C++17 compatible compiler (MSYS2 MinGW64 g++ / MSVC / GCC / Clang)
- Qt6 (requires Core, Gui, Widgets, OpenGL, OpenGLWidgets, and WebSockets modules)

### Windows (MSYS2, Recommended)
In the MSYS2 MinGW64 environment:

```powershell
pacman -S mingw-w64-x86_64-qt6-base mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-websockets
cmake -B build -G Ninja
cmake --build build
```

The build artifact will be `build/Live2Tee.exe`. If an `assets/` directory exists in the repository, the build script will automatically copy it to the same directory as the executable.

### Linux (Debian/Ubuntu Example)

```bash
sudo apt install cmake ninja-build g++ qt6-base-dev
cmake -B build -G Ninja
cmake --build build
```

> **Note:** Global input hooks are currently only implemented on Windows (`WH_MOUSE_LL` / `WH_KEYBOARD_LL`). The program can compile and run normally on other platforms, but input-driven actions will not work.

> Stability and usability on Linux have not been fully verified yet.

## License & Disclaimer
- This project is open-sourced under the **GPLv3** license because the rendering code under `third_party/tee_render` is derived from [Floatee](https://github.com/Tatatatataaaa/Floatee/tree/online), which is released under GPLv3. These files also retain the original zlib license notice for Teeworlds/DDNet (c) Magnus Auvinen.
- Qt is used via **dynamic linking** (open-source LGPLv3 version).
- This project is for entertainment purposes only and does not represent the views of Teeworlds, DDNet, or their respective projects.
- This repository does not contain any Teeworlds/DDNet related assets. Please obtain the required images for the `assets/` folder yourself and comply with their respective licenses.

This project was developed with the assistance of AI.