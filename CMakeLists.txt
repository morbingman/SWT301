cmake_minimum_required(VERSION 3.20)
project(Fibinacho CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to a Release build if none is specified. Without this, CMake's
# default build type is "" (no optimization flags at all), which silently
# produces a debug-speed binary even when you think you're building release.
# This matters a lot here: the FFT multiply in field_ext leans heavily on
# the optimizer (inlining, vectorization of std::complex ops) to hit its
# real speed -- without -O2/-O3 it can end up SLOWER than the simple O(n^2)
# multiply used by matmul_fastexp, which has much less for the optimizer to do.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Fibinacho CLI binary (unchanged)
# ---------------------------------------------------------------------------

add_executable(Fibinacho
        main.cpp
        benchmark_writer.h
        benchmark_writer.cpp
        num/number.h
        num/algorithms.h
        num/mulnum_simple.h
        num/mulnum_fft.h
        impl/naive.cpp
        impl/linear.cpp
        impl/matmul_simple.cpp
        impl/matmul_fastexp.cpp
        impl/field_ext.cpp)

target_include_directories(Fibinacho PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Fibinacho PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# ImGui + ImPlot via FetchContent
# ---------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.4
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
        implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

find_package(OpenGL REQUIRED)

# GLFW
FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# ---------------------------------------------------------------------------
# ImGui + ImPlot as a static lib
# ---------------------------------------------------------------------------

add_library(imgui_lib STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${implot_SOURCE_DIR}/implot.cpp
        ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(imgui_lib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${implot_SOURCE_DIR}
)

target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# FibinachoUI desktop app
# ---------------------------------------------------------------------------

add_executable(FibinachoUI
        ui/main_ui.cpp
        ui/csv_reader.h
        ui/csv_reader.cpp
        ui/runner.h
        ui/runner.cpp
)

target_include_directories(FibinachoUI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(FibinachoUI PRIVATE imgui_lib Threads::Threads)

# inject the path to the Fibinacho binary at compile time
# $<TARGET_FILE:Fibinacho> resolves to the actual binary path in the build tree
target_compile_definitions(FibinachoUI PRIVATE
        FIBINACHO_BIN="$<TARGET_FILE:Fibinacho>"
)

# ensure Fibinacho is always built before FibinachoUI
add_dependencies(FibinachoUI Fibinacho)

if(MINGW)
    set_target_properties(FibinachoUI PROPERTIES WIN32_EXECUTABLE TRUE)
endif()