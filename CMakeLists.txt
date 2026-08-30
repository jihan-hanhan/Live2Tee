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
add_custom_command(TARGET Live2Tee POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:Live2Tee>/assets
  COMMENT "Copy assets next to executable"
)
