cmake_minimum_required(VERSION 3.20)
project(s2k_Tweaks LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(s2k_TweaksCore SHARED
    src/dllmain.cpp
    src/console_unlock.cpp
    src/addon_ui.cpp
    src/log.cpp
    src/imgui_overlay.cpp
    src/notification_state.cpp
    src/plugin_manager.cpp
    src/vulkan_hook.cpp
    src/xinput_proxy.cpp
    src/xinput.def
    external/minhook/src/buffer.c
    external/minhook/src/hook.c
    external/minhook/src/trampoline.c
    external/minhook/src/hde/hde64.c
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_vulkan.cpp
    external/imgui/backends/imgui_impl_win32.cpp
)

target_include_directories(s2k_TweaksCore PRIVATE
    src
    external/minhook/include
    external/minhook/src
    external/minhook/src/hde
    external/vulkan-headers/include
    external/imgui
)

target_compile_definitions(s2k_TweaksCore PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    VK_NO_PROTOTYPES
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
)

target_link_libraries(s2k_TweaksCore PRIVATE user32)
set_target_properties(s2k_TweaksCore PROPERTIES OUTPUT_NAME XINPUT1_4)

add_library(s2k_TweaksPlugin SHARED
    plugin/s2k_tweaks_plugin.cpp
)
target_include_directories(s2k_TweaksPlugin PRIVATE src)
target_compile_definitions(s2k_TweaksPlugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(s2k_TweaksPlugin PROPERTIES OUTPUT_NAME s2k_Tweaks)

if(MSVC)
    target_compile_options(s2k_TweaksCore PRIVATE /W4 /permissive- /EHsc)
    target_link_options(s2k_TweaksCore PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/xinput.def")
    target_compile_options(s2k_TweaksPlugin PRIVATE /W4 /permissive- /EHsc)
endif()

add_executable(xinput_smoke tests/xinput_smoke.cpp)
target_compile_definitions(xinput_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(signature_file_smoke tests/signature_file_smoke.cpp)

add_executable(plugin_smoke tests/plugin_smoke.cpp)
target_include_directories(plugin_smoke PRIVATE src)
target_compile_definitions(plugin_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(notification_queue_smoke
    tests/notification_queue_smoke.cpp
    src/notification_state.cpp
)
target_include_directories(notification_queue_smoke PRIVATE src)
