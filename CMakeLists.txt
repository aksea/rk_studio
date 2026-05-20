cmake_minimum_required(VERSION 3.18)

project(rk_studio LANGUAGES CXX)

# Project-wide build defaults.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Core dependencies.
find_package(Threads REQUIRED)
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-allocators-1.0
  gstreamer-app-1.0
  gstreamer-video-1.0
  gstreamer-audio-1.0
)

set(RK_STUDIO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Infrastructure sources (self-contained, no external project dependency).
set(RK_STUDIO_INFRA_SOURCES
  src/infra/runtime.cpp
  src/infra/gst_util.cpp
  src/infra/gst_audio_recorder.cpp
  src/infra/telemetry.cpp
  src/infra/session_files.cpp
)
# Application sources.
set(RK_STUDIO_SOURCES
  src/main.cpp
  src/domain/types.cpp
  src/domain/config.cpp
  src/domain/session.cpp
  src/runtime/runtime_manager.cpp
  src/media_core/v4l2_pipeline.cpp
  src/media_core/media_engine.cpp
  src/media_core/session_writer.cpp
  src/ui/preview_tile_widget.cpp
  src/ui/main_window.cpp
)

set(RK_STUDIO_HEADERS
  include/rk_studio/domain/types.h
  include/rk_studio/domain/config.h
  include/rk_studio/domain/session.h
  include/rk_studio/runtime/runtime_manager.h
  include/rk_studio/media_core/v4l2_pipeline.h
  include/rk_studio/media_core/media_engine.h
  include/rk_studio/media_core/session_writer.h
  include/rk_studio/ui/preview_tile_widget.h
  include/rk_studio/ui/main_window.h
  include/rk_studio/infra/runtime.h
  include/rk_studio/infra/gst_util.h
  include/rk_studio/infra/config_types.h
  include/rk_studio/infra/telemetry.h
  include/rk_studio/infra/gst_audio_recorder.h
  include/rk_studio/infra/session_files.h
)

add_executable(rk_studio)
target_sources(rk_studio PRIVATE
  ${RK_STUDIO_SOURCES}
  ${RK_STUDIO_HEADERS}
  ${RK_STUDIO_INFRA_SOURCES}
)

target_include_directories(rk_studio
  PRIVATE
    ${RK_STUDIO_ROOT}/include
    ${RK_STUDIO_ROOT}/third_party/tomlplusplus
)

target_link_libraries(rk_studio
  PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Threads::Threads
    PkgConfig::GST
)

target_compile_options(rk_studio PRIVATE -Wall -Wextra -Wpedantic)
