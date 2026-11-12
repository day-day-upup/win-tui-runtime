cmake_minimum_required(VERSION 3.20)
project(win-tui-runtime LANGUAGES CXX)

# 纯 C++20 + Win32，零第三方依赖（约束 A1/A2）。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode + 去掉 windows.h 里的 min/max 宏污染（见 platform.h）
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc /utf-8)
endif()

# ---- core 静态库：事件循环骨架（阶段 1）----
add_library(tui_core STATIC
    src/core/clock.cpp
    src/core/waker.cpp
    src/core/timer_heap.cpp
    src/core/event_loop.cpp
)
target_include_directories(tui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- console 静态库：控制台输入/渲染（阶段 2）----
add_library(tui_console STATIC
    src/console/render_backend.cpp
    src/console/screen_buffer.cpp
    src/console/input_decoder.cpp
    src/console/console_session.cpp
    src/console/console_input_thread.cpp
    src/console/renderer.cpp
    src/console/key_viewer.cpp
)
target_include_directories(tui_console PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tui_console PUBLIC tui_core)

# ---- 单元测试 ----
enable_testing()
add_executable(test_event_loop tests/test_event_loop.cpp)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_event_loop PRIVATE tui_core)
add_test(NAME event_loop COMMAND test_event_loop)

add_executable(test_console tests/test_console.cpp)
target_include_directories(test_console PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_console PRIVATE tui_console)
add_test(NAME console COMMAND test_console)

# ---- demo ----
add_executable(loop_demo src/app/loop_demo.cpp)
target_link_libraries(loop_demo PRIVATE tui_core)

add_executable(key_viewer src/app/key_viewer_main.cpp)
target_link_libraries(key_viewer PRIVATE tui_console)
