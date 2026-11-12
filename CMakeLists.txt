cmake_minimum_required(VERSION 3.20)
project(win-tui-runtime LANGUAGES CXX)

# 阶段 1：Loop 骨架
# 纯 C++20 + Win32，零第三方依赖（约束 A1/A2）。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Unicode + 去掉 windows.h 里的 min/max 宏污染（见 platform.h）
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc /utf-8)
endif()

# ---- core 静态库：事件循环骨架 ----
add_library(tui_core STATIC
    src/core/clock.cpp
    src/core/waker.cpp
    src/core/timer_heap.cpp
    src/core/event_loop.cpp
)
target_include_directories(tui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# 阶段 1 只用到 kernel32（QPC / CreateEvent / SRWLOCK 都在其中），
# 由 MSVC 默认链接，无需显式 target_link_libraries。

# ---- 单元测试 ----
enable_testing()
add_executable(test_event_loop tests/test_event_loop.cpp)
target_include_directories(test_event_loop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_event_loop PRIVATE tui_core)
add_test(NAME event_loop COMMAND test_event_loop)

# ---- headless demo ----
add_executable(loop_demo src/app/loop_demo.cpp)
target_link_libraries(loop_demo PRIVATE tui_core)
