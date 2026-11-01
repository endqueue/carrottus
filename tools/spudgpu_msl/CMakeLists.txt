# spudgpu_msl on its own, for the build machine: what spudlib's CMakeLists.txt
# builds through ExternalProject when it is cross-compiling (an iOS build),
# where its own targets are the target platform's and can't run here. A
# native build makes the same tool as a target of spudlib's instead.
#
# SPUDGPU_MSL_SPIRV_CROSS_DIR names a SPIRV-Cross checkout (the workspace's
# vendored one); without it, SPIRV-Cross is fetched at SPUDGPU_MSL_SPIRV_CROSS_TAG.
# The tool is written to bin/ of this build, the same path in every
# configuration.

cmake_minimum_required(VERSION 3.25)
project(spudgpu_msl C CXX)

set(SPUDGPU_MSL_SPIRV_CROSS_DIR "" CACHE PATH "A SPIRV-Cross checkout; fetched when empty")
set(SPUDGPU_MSL_SPIRV_CROSS_TAG "vulkan-sdk-1.4.363.0" CACHE STRING "The SPIRV-Cross tag fetched when no checkout is named")

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
if(SPUDGPU_MSL_SPIRV_CROSS_DIR)
    FetchContent_Declare(spirv_cross SOURCE_DIR "${SPUDGPU_MSL_SPIRV_CROSS_DIR}")
else()
    FetchContent_Declare(spirv_cross GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
        GIT_TAG "${SPUDGPU_MSL_SPIRV_CROSS_TAG}")
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_MakeAvailable(spirv_cross)

add_executable(spudgpu_msl spudgpu_msl.c)
target_link_libraries(spudgpu_msl PRIVATE spirv-cross-c)
# SPIRV-Cross is C++: link as C++ for its runtime. The generator expression
# keeps a multi-config generator from adding a per-configuration folder.
set_target_properties(spudgpu_msl PROPERTIES
    LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin$<0:>")
