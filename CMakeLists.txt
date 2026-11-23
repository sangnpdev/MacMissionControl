cmake_minimum_required(VERSION 3.24)
project(MissionControl VERSION 0.6.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no redist dependency

if(NOT MSVC)
  message(FATAL_ERROR "MissionControl targets MSVC only")
endif()
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX MC_VERSION="${PROJECT_VERSION}")

# Pure, platform-independent logic. Unit-tested.
add_library(mc_core STATIC
  src/input/wintab_detector.cpp
  src/desktop/desktop_state.cpp
  src/animation/animation.cpp
  src/animation/gesture.cpp
  src/config/json.cpp
  src/ui/layout.cpp
  src/window/window_filter.cpp
  src/config/animation_config.cpp)
target_include_directories(mc_core PUBLIC src)

# Native application.
add_executable(MissionControl WIN32
  src/app/main.cpp
  src/app/app.cpp
  src/app/overlay_window.cpp
  src/window/window_manager.cpp
  src/desktop/virtual_desktop_provider.cpp
  src/preview/thumbnail.cpp
  src/rendering/scene_renderer.cpp
  src/rendering/compositor_thread.cpp
  src/diagnostics/log.cpp
  src/input/keyboard_hook.cpp
  src/app/autostart.cpp
  src/app/tray.cpp
  resources/version.rc
  resources/app.manifest)
target_link_libraries(MissionControl PRIVATE mc_core dwmapi dcomp d3d11 d2d1 dwrite windowscodecs dxgi dxguid dbghelp ole32 shell32 shcore user32)
target_compile_definitions(MissionControl PRIVATE MC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} MC_VERSION_MINOR=${PROJECT_VERSION_MINOR} MC_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Benchmark helper: creates N plain top-level windows of class MCBenchWindow.
add_executable(mc_testwin WIN32 benchmarks/testwin.cpp resources/app.manifest)
target_link_libraries(mc_testwin PRIVATE user32 gdi32)

# Animation engine demo (Phase 2).
add_executable(mc_animdemo WIN32 demo/animdemo.cpp resources/app.manifest)
target_link_libraries(mc_animdemo PRIVATE mc_core d2d1 dwrite dwmapi user32 shell32)

enable_testing()
add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE mc_core)
target_compile_definitions(unit_tests PRIVATE MC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit_tests COMMAND unit_tests)

# Needs an interactive desktop (real DWM). Run with: ctest -L integration
add_executable(integration_tests tests/integration_tests.cpp src/window/window_manager.cpp src/preview/thumbnail.cpp src/app/autostart.cpp src/diagnostics/log.cpp)
target_link_libraries(integration_tests PRIVATE mc_core dwmapi ole32 shcore user32 advapi32)
add_test(NAME integration_tests COMMAND integration_tests $<TARGET_FILE:mc_testwin>)
set_tests_properties(integration_tests PROPERTIES LABELS integration)

# Changes the user's session (creates/switches/removes one desktop). Not registered with ctest: run by hand.
add_executable(desktop_tests tests/desktop_tests.cpp src/desktop/virtual_desktop_provider.cpp src/diagnostics/log.cpp)
target_link_libraries(desktop_tests PRIVATE mc_core dwmapi ole32 user32 advapi32)
