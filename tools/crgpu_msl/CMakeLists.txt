# crgpu_msl on its own, for the build machine: what cmake/cr_shaders.cmake
# builds through ExternalProject wherever a shader step runs it, in a native
# and a cross-compiling build alike (card X18), so its SPIRV-Cross is never a
# target of the build that runs it.
#
# CRGPU_MSL_SPIRV_CROSS_DIR names a SPIRV-Cross checkout (the workspace's
# vendored one); without it, SPIRV-Cross is fetched at CRGPU_MSL_SPIRV_CROSS_TAG.
# The tool is written to bin/ of this build, the same path in every
# configuration.

cmake_minimum_required(VERSION 3.25)
project(crgpu_msl C CXX)

set(CRGPU_MSL_SPIRV_CROSS_DIR "" CACHE PATH "A SPIRV-Cross checkout; fetched when empty")
set(CRGPU_MSL_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "The SPIRV-Cross tag fetched when no checkout is named")

include(FetchContent)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
if(CRGPU_MSL_SPIRV_CROSS_DIR)
    FetchContent_Declare(spirv_cross SOURCE_DIR "${CRGPU_MSL_SPIRV_CROSS_DIR}")
else()
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG "${CRGPU_MSL_SPIRV_CROSS_TAG}")
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(spirv_cross)

add_executable(crgpu_msl crgpu_msl.c)
target_link_libraries(crgpu_msl PRIVATE spirv-cross-c)
# SPIRV-Cross is C++: link as C++ for its runtime. The generator expression
# keeps a multi-config generator from adding a per-configuration folder.
set_target_properties(crgpu_msl PROPERTIES
    LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")
