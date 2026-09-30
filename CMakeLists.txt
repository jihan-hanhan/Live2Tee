cmake_minimum_required(VERSION 3.16)

project(Live2Tee LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Qt:开启 AUTOMOC 处理 Q_OBJECT ------------------------------------------
set(CMAKE_AUTOMOC ON)

# ---- third-party: tee_render (零依赖 C++17 Tee 渲染管线) ----------------------
add_library(tee_render STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tee_render/src/tee_anim.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tee_render/src/tee_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tee_render/src/tee_emoticon.cpp
)
target_include_directories(tee_render PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tee_render/include
)

# ---- 主程序 ----------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets WebSockets)

add_executable(Live2Tee
  src/main.cpp
  src/config.cpp
  src/gui/preview_window.cpp
  src/gui/control_window.cpp
  src/gui/skin_picker_dialog.cpp
  src/render/scene.cpp
  src/render/offscreen_output.cpp
  src/render/gl_backend.cpp
  src/render/texture.cpp
  src/net/browser_server.cpp
  src/input/input.cpp
  src/state/state.cpp
)

target_include_directories(Live2Tee PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(Live2Tee PRIVATE
  tee_render
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  Qt6::WebSockets
)

# MinGW 控制台程序(日志打印到 stdout);如需纯 GUI 可改 WIN32_EXECUTABLE
set_target_properties(Live2Tee PROPERTIES
  WIN32_EXECUTABLE FALSE
)

if(WIN32)
  # 低级钩子需要 user32
  target_link_libraries(Live2Tee PRIVATE user32)
endif()

# ---- 把 assets 复制到可执行文件同级目录(默认资源布局) -------------------------
# 仓库不内置素材,assets/ 不存在时跳过(添加素材后重新 configure 即可生效)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
  add_custom_command(TARGET Live2Tee POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_CURRENT_SOURCE_DIR}/assets
      $<TARGET_FILE_DIR:Live2Tee>/assets
    COMMENT "Copy assets next to executable"
  )
endif()

# 浏览器源页面:src/browser/live2tee.html -> assets/browser/live2tee.html
# (程序运行时只 mkpath assets/browser,页面本身需随包部署)
add_custom_command(TARGET Live2Tee POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    $<TARGET_FILE_DIR:Live2Tee>/assets/browser
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/src/browser/live2tee.html
    $<TARGET_FILE_DIR:Live2Tee>/assets/browser/live2tee.html
  COMMENT "Deploy browser source page"
)

# ---- 一键发布包:dist/ = exe + assets + windeployqt 收集的 Qt6/MinGW 运行库 -> zip ----
if(WIN32)
  get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
  # Qt6 部署工具在新版 MSYS2 中名为 windeployqt6 / windeployqt-qt6,
  # 旧版及其他发行版可能仍叫 windeployqt,依次尝试
  find_program(WINDEPLOYQT_EXE
    NAMES windeployqt6 windeployqt-qt6 windeployqt
    HINTS "${_qt_bin_dir}")

  if(WINDEPLOYQT_EXE)
    set(_DIST_DIR "${CMAKE_BINARY_DIR}/dist")
    # assets 为可选素材目录(仓库不内置),存在时才打入发布包
    set(_assets_cmds "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets")
      list(APPEND _assets_cmds
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${_DIST_DIR}/assets")
    endif()
    # 浏览器源页面(源码内资产,不在 assets/ 下)
    list(APPEND _assets_cmds
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_DIST_DIR}/assets/browser"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/browser/live2tee.html"
        "${_DIST_DIR}/assets/browser/live2tee.html")
    # MinGW 运行库(windeployqt 在 MSYS2 环境下可能不复制,显式补齐)
    get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_rt_cmds "")
    foreach(_rt libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
      if(EXISTS "${_mingw_bin}/${_rt}")
        list(APPEND _rt_cmds
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_mingw_bin}/${_rt}" "${_DIST_DIR}/")
      endif()
    endforeach()
    add_custom_target(release
      COMMAND ${CMAKE_COMMAND} -E remove_directory "${_DIST_DIR}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_DIST_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:Live2Tee>" "${_DIST_DIR}/"
      ${_assets_cmds}
      # 自动收集 Qt6 DLL、平台插件、WebSocket 插件及 MinGW 运行库
      # (libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll)
      COMMAND "${WINDEPLOYQT_EXE}" --no-translations "${_DIST_DIR}/Live2Tee.exe"
      ${_rt_cmds}
      COMMAND powershell -NoProfile -Command
        "Compress-Archive -Path '${_DIST_DIR}/*' -DestinationPath '${CMAKE_BINARY_DIR}/Live2Tee-win64.zip' -Force"
      DEPENDS Live2Tee
      COMMENT "Assemble release zip at ${CMAKE_BINARY_DIR}/Live2Tee-win64.zip"
      VERBATIM
    )
  else()
    message(WARNING "windeployqt 未找到,无法生成 release 打包目标")
  endif()
endif()
