# Live2Tee
一个 Teeworlds 爱好者的项目,用于将你的 Tee 加入到你的 视频/直播 中,让 Tee 成为你的一个形象。

[[ENG]](README.md)

[中文]

## 用法(以 OBS 为例)
1. 运行 `Live2Tee.exe`
2. 在 exe 同级目录的 `assets` 目录下放入 `game.png` 与 `emoticons.png`,并在 `assets/skins` 下放入你想要的皮肤图片
3. 按下`F9`打开配置界面,您可以在此 控制缩放,窗口大小,切换Tee的皮肤,重置鼠标位置
4. 在OBS中添加一个`浏览器`源,填入配置界面提供的URL,调整大小和位置,直到你满意为止
5. 享受吧

### 特点
- 按下键盘**任意键**,Tee 会显示对应的表情(气泡 + 眼神),表情与动作互不打断
- 按下 `鼠标左键`,Tee 会挥锤
- 按下 `鼠标右键`,Tee 会发射钩子
- 移动鼠标时 Tee 的目光会跟随(全局输入,无需窗口焦点)

## 从源码编译
### 依赖
- CMake >= 3.16
- Ninja(或任意 CMake 生成器)
- 支持 C++17 的编译器(MSYS2 MinGW64 g++ / MSVC / gcc clang 均可)
- Qt6(需要 Core,Gui,Widgets,OpenGL,OpenGLWidgets,websockets 模块)

### Windows(MSYS2,推荐)
在 MSYS2 MinGW64 环境中:

```powershell
pacman -S mingw-w64-x86_64-qt6-base mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-websockets
cmake -B build -G Ninja
cmake --build build
```

构建产物为 `build/Live2Tee.exe`。若仓库中存在 `assets/` 目录,构建脚本会自动把它复制到 exe 同级目录。

### Linux(Debian/Ubuntu 示例)

```bash
sudo apt install cmake ninja-build g++ qt6-base-dev 
cmake -B build -G Ninja
cmake --build build
```

> 注意:全局输入钩子目前仅在 Windows 上实现(`WH_MOUSE_LL` / `WH_KEYBOARD_LL`)。其他平台程序可正常编译运行,但不会有输入驱动动作。

> Linux端稳定性与可用性暂未得到验证

## 声明
- 本项目以 **GPLv3** 协议开源,因为 `third_party/tee_render` 下的渲染代码来自 [Floatee](https://github.com/Tatatatataaaa/Floatee/tree/online),后者以 GPLv3 发布;该部分文件同时保留了原作 Teeworlds/DDNet (c) Magnus Auvinen 的 zlib 许可声明
- Qt 以**动态链接**方式使用(开源版 LGPLv3)
- 本项目不代表 Teeworlds、DDNet 及相关项目的观点,仅供娱乐
- 本项目仓库不包含任何 Teeworlds/DDNet 相关素材,`assets/` 下所需的图片请自行获取,并遵循相关素材自身许可

本项目由AI参与开发
