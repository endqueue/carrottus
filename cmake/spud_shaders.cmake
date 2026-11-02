# The shader step: Slang in, each graphics API's own format out, at build
# time (cards A29, A9, X1c, X18 and S27 in the workspace's DECISIONS.md).
# SpudGPU compiles nothing: a shader module takes code in the one format its
# adapter reports (SPUDGPU_ADAPTER_PROPERTIES::shader_formats). This file is
# written once, for spudlib's tests and for any caller's build, which calls
# spud_add_shader() or wraps it; nothing else in a build runs slangc or
# tools/spudgpu_msl.
#
# For each shader <name>, in the OUT_DIR given:
# - <name>.spv, SPIR-V 1.5, checked by spirv-val; on every platform.
# - <name>.reflection.json, Slang's reflection (-reflection-json): every
#   binding the source declares, read or not; on every platform.
# - <name>.dxil, DXIL: Slang writes HLSL and DXC compiles it, its validator
#   signing what it passes; on Windows, where the D3D12 backend builds.
# - <name>.slang.metal and <name>.cross.metal, Metal source by both of card
#   A29's routes (Slang's own, and Slang to SPIR-V to tools/spudgpu_msl in
#   the Metal backend's binding layout, each set's argument buffer padded to
#   every binding the reflection lists: spudgpu.h, spudgpu_shader_module_desc,
#   "Every binding of a set"). Metal 3.1 (Apple7 and later, macOS 14,
#   iOS 17). Only where a .metallib is made, or elsewhere under
#   SPUD_SHADERS_CHECK_METAL (card X1c).
# - <name>.metallib where the build is for an Apple platform, from the route
#   the shader names (tools/spudgpu_msl's by default, card S21), for this
#   build's SDK and deployment target: macOS, an iOS device or the iOS
#   simulator (from CMAKE_OSX_SYSROOT), at CMAKE_OSX_DEPLOYMENT_TARGET where
#   it is set.
# - <name>.<format>.inc, each binary as a C array (uint32_t words, zero
#   padded to a whole word, and <name>_<format>_size, the exact byte count;
#   cmake/spud_embed.cmake), for a source to include.
#
# The tools come from the Vulkan SDK (VULKAN_SDK); their versions are printed
# at configure time. tools/spudgpu_msl is always a program of its own, built
# for the build machine through ExternalProject (tools/spudgpu_msl/
# CMakeLists.txt) in a native and a cross-compiling build alike, or the one
# SPUDGPU_MSL_EXECUTABLE names; its SPIRV-Cross is never a target of the
# build that includes this file. It is built only where a step runs it.

include_guard(GLOBAL)

option(SPUD_SHADERS_CHECK_METAL
    "Off Apple platforms, translate every shader to Metal source as a check (no .metallib)" OFF)
set(SPUDGPU_MSL_EXECUTABLE "" CACHE FILEPATH
    "A spudgpu_msl already built for this machine; built here when empty")
# An older spudlib kept its own build of the tool in this variable, as an
# internal entry; that is not a program the caller named.
get_property(_spud_msl_type CACHE SPUDGPU_MSL_EXECUTABLE PROPERTY TYPE)
if(_spud_msl_type STREQUAL "INTERNAL")
    unset(SPUDGPU_MSL_EXECUTABLE CACHE)
    set(SPUDGPU_MSL_EXECUTABLE "" CACHE FILEPATH
        "A spudgpu_msl already built for this machine; built here when empty")
endif()

set(_spud_sdk_bin "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(SPUD_SLANGC slangc HINTS ${_spud_sdk_bin} REQUIRED)
find_program(SPUD_SPIRV_VAL spirv-val HINTS ${_spud_sdk_bin} REQUIRED)
if(WIN32)
    find_program(SPUD_DXC dxc HINTS ${_spud_sdk_bin} REQUIRED)
endif()
execute_process(COMMAND ${SPUD_SLANGC} -version OUTPUT_VARIABLE _spud_v ERROR_VARIABLE _spud_v
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
message(STATUS "Shaders: slangc ${_spud_v} (${SPUD_SLANGC})")
if(WIN32)
    execute_process(COMMAND ${SPUD_DXC} --version OUTPUT_VARIABLE _spud_v OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "Shaders: ${_spud_v} (${SPUD_DXC})")
endif()

# Metal source is written where a .metallib is made (a build for an Apple
# platform, which only a Mac can make) or, elsewhere, as a check. Tested as
# APPLE OR SPUD_SHADERS_CHECK_METAL where it is needed, both of them seen
# from every directory (this file is read once, in whichever directory
# includes it first, so a plain variable set here isn't).
#
# spudgpu_msl, an imported program the steps run and depend on. Nothing of
# this build's toolchain is passed to its own build: it is built with the
# build machine's compiler, in Release, for the build machine.
if((APPLE OR SPUD_SHADERS_CHECK_METAL) AND NOT TARGET spudgpu_msl)
    add_executable(spudgpu_msl IMPORTED GLOBAL)
    if(SPUDGPU_MSL_EXECUTABLE)
        set_target_properties(spudgpu_msl PROPERTIES IMPORTED_LOCATION ${SPUDGPU_MSL_EXECUTABLE})
        message(STATUS "Shaders: spudgpu_msl ${SPUDGPU_MSL_EXECUTABLE}")
    else()
        include(ExternalProject)
        set(_spud_msl_spirv_cross "")
        foreach(_cand SPIRV-Cross SPIRV-Cross-src)
            set(_dir ${CMAKE_CURRENT_LIST_DIR}/../../.deps/${_cand})
            if(NOT _spud_msl_spirv_cross AND EXISTS ${_dir}/CMakeLists.txt)
                get_filename_component(_spud_msl_spirv_cross ${_dir} ABSOLUTE)
            endif()
        endforeach()
        set(_spud_msl_dir ${CMAKE_BINARY_DIR}/spudgpu_msl_host)
        ExternalProject_Add(spudgpu_msl_build
            SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../tools/spudgpu_msl
            BINARY_DIR ${_spud_msl_dir}
            CMAKE_ARGS -DSPUDGPU_MSL_SPIRV_CROSS_DIR=${_spud_msl_spirv_cross} -DCMAKE_BUILD_TYPE=Release
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${_spud_msl_dir} --config Release
            INSTALL_COMMAND ""
            # Its own build is asked every time, so an edit to the tool is
            # built; nothing is remade when nothing changed.
            BUILD_ALWAYS ON
            BUILD_BYPRODUCTS ${_spud_msl_dir}/bin/spudgpu_msl${CMAKE_HOST_EXECUTABLE_SUFFIX})
        set_target_properties(spudgpu_msl PROPERTIES
            IMPORTED_LOCATION ${_spud_msl_dir}/bin/spudgpu_msl${CMAKE_HOST_EXECUTABLE_SUFFIX})
        add_dependencies(spudgpu_msl spudgpu_msl_build)
        message(STATUS "Shaders: spudgpu_msl built for the build machine in ${_spud_msl_dir}")
    endif()
endif()

# spud_add_shader(<name> SOURCE <file.slang> OUT_DIR <dir> DXIL_PROFILE <profile>
#     [INCLUDE_DIRS <dir>...] [DEPENDS <file>...] [SPIRV_CAPABILITIES <cap>...]
#     [METAL_ROUTE SLANG <reason> | NO_METAL <reason>] [OUTPUTS <var>])
#
# SOURCE and INCLUDE_DIRS are absolute or relative to the calling directory.
# The source names its stage ([shader("compute")]...), and its entry point is
# the source's stem (rq_trace.slang: rq_trace) in every format, never main,
# which Metal reserves. <name> names the outputs and their C arrays
# (<name>_spirv...). INCLUDE_DIRS are where its imports are found, and
# DEPENDS the modules it imports, so a change to one builds it again.
# <profile> is the shader model DXC compiles for (vs_6_0, cs_6_5...). SPIR-V
# is 1.5, which Vulkan 1.2 and later take, plus the capabilities named.
# METAL_ROUTE SLANG <reason> makes the .metallib from Slang's own Metal
# source instead of spudgpu_msl's (card S21); NO_METAL <reason> makes no
# Metal source at all, for a shader of a program that never builds for an
# Apple platform and that neither route takes (bindless). The reason is
# written beside the call either way.
#
# Appends the .inc files and Metal sources made to <var> in the caller's
# scope, for a target to depend on, and sets <name>_FORMATS there to the
# formats made: spirv, and dxil and metallib where made.
function(spud_add_shader name)
    cmake_parse_arguments(ARG "" "SOURCE;OUT_DIR;DXIL_PROFILE;OUTPUTS"
        "INCLUDE_DIRS;DEPENDS;SPIRV_CAPABILITIES;METAL_ROUTE;NO_METAL" ${ARGN})
    if(NOT ARG_SOURCE OR NOT ARG_OUT_DIR OR NOT ARG_DXIL_PROFILE)
        message(FATAL_ERROR "spud_add_shader(${name}): SOURCE, OUT_DIR and DXIL_PROFILE are required")
    endif()
    if(ARG_NO_METAL AND APPLE)
        message(FATAL_ERROR "spud_add_shader(${name}): NO_METAL, but this build is for an Apple platform")
    endif()
    get_filename_component(src ${ARG_SOURCE} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    get_filename_component(entry ${src} NAME_WE)
    set(out ${ARG_OUT_DIR}/${name})
    set(embed ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/spud_embed.cmake)
    set(flags "")
    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        get_filename_component(dir ${dir} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        list(APPEND flags -I ${dir})
    endforeach()
    set(caps "")
    foreach(cap IN LISTS ARG_SPIRV_CAPABILITIES)
        list(APPEND caps -capability ${cap})
    endforeach()
    set(depends ${src} ${ARG_DEPENDS})

    add_custom_command(
        OUTPUT ${out}.spv ${out}.reflection.json ${out}.spv.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUT_DIR}
        COMMAND ${SPUD_SLANGC} ${src} ${flags} -entry ${entry} -target spirv -profile spirv_1_5 ${caps}
                -fvk-use-entrypoint-name -matrix-layout-column-major
                -warnings-as-errors all -o ${out}.spv -reflection-json ${out}.reflection.json
        COMMAND ${SPUD_SPIRV_VAL} --target-env vulkan1.2 ${out}.spv
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.spv -DOUT=${out}.spv.inc -DNAME=${name}_spirv -P ${embed}
        DEPENDS ${depends} ${embed}
        COMMENT "Slang: ${entry} to SPIR-V (spirv-val)")
    set(outputs ${out}.spv.inc)
    set(formats spirv)

    if(WIN32)
        add_custom_command(
            OUTPUT ${out}.hlsl ${out}.dxil ${out}.dxil.inc
            COMMAND ${CMAKE_COMMAND} -E make_directory ${ARG_OUT_DIR}
            COMMAND ${SPUD_SLANGC} ${src} ${flags} -entry ${entry} -target hlsl -profile ${ARG_DXIL_PROFILE}
                    -matrix-layout-column-major
                    -warnings-as-errors all -o ${out}.hlsl
            COMMAND ${SPUD_DXC} -nologo -T ${ARG_DXIL_PROFILE} -E ${entry} -WX ${out}.hlsl -Fo ${out}.dxil
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.dxil -DOUT=${out}.dxil.inc -DNAME=${name}_dxil -P ${embed}
            DEPENDS ${depends} ${embed}
            COMMENT "Slang: ${entry} to DXIL (DXC, validated)")
        list(APPEND outputs ${out}.dxil.inc)
        list(APPEND formats dxil)
    endif()

    if((APPLE OR SPUD_SHADERS_CHECK_METAL) AND NOT ARG_NO_METAL)
        # Both Metal routes' source, kept (card S21: which one a .metallib
        # is made from).
        add_custom_command(
            OUTPUT ${out}.slang.metal ${out}.cross.metal
            COMMAND ${SPUD_SLANGC} ${src} ${flags} -entry ${entry} -target metal -matrix-layout-column-major
                    -o ${out}.slang.metal
            COMMAND spudgpu_msl --platform $<IF:$<PLATFORM_ID:iOS>,ios,macos> --msl-version 30100
                    --reflection ${out}.reflection.json ${out}.spv ${out}.cross.metal
            DEPENDS ${depends} ${out}.spv ${out}.reflection.json spudgpu_msl
            COMMENT "Slang: ${entry} to Metal source, both routes")
        list(APPEND outputs ${out}.slang.metal ${out}.cross.metal)
    endif()

    if(APPLE)
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
            set(metal_sdk iphonesimulator)
            set(metal_min -mios-simulator-version-min=)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(metal_sdk iphoneos)
            set(metal_min -mios-version-min=)
        else()
            set(metal_sdk macosx)
            set(metal_min -mmacosx-version-min=)
        endif()
        set(metal_flags "")
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(metal_flags ${metal_min}${CMAKE_OSX_DEPLOYMENT_TARGET})
        endif()
        set(metal_src ${out}.cross.metal)
        if("SLANG" IN_LIST ARG_METAL_ROUTE)
            set(metal_src ${out}.slang.metal)
        endif()
        add_custom_command(
            OUTPUT ${out}.metallib ${out}.metallib.inc
            COMMAND xcrun -sdk ${metal_sdk} metal -std=metal3.1 ${metal_flags} -Werror -c ${metal_src} -o ${out}.air
            COMMAND xcrun -sdk ${metal_sdk} metallib ${out}.air -o ${out}.metallib
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.metallib -DOUT=${out}.metallib.inc -DNAME=${name}_metallib -P ${embed}
            DEPENDS ${metal_src} ${embed}
            COMMENT "Metal: ${name} to a metallib")
        list(APPEND outputs ${out}.metallib.inc)
        list(APPEND formats metallib)
    endif()

    if(ARG_OUTPUTS)
        set(${ARG_OUTPUTS} ${${ARG_OUTPUTS}} ${outputs} PARENT_SCOPE)
    endif()
    set(${name}_FORMATS ${formats} PARENT_SCOPE)
endfunction()
