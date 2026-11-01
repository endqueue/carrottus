# SpudLib's own test shaders: Slang in, each graphics API's own format out, at
# build time (cards A29 and A9 in the workspace's DECISIONS.md). The library
# compiles nothing; this is the tests' build step, as a caller's would be.
#
# For each shader, in ${SPUD_SLANG_OUT_DIR}:
# - <name>.spv, SPIR-V, checked by spirv-val; on every platform.
# - <name>.dxil, DXIL: Slang writes HLSL and DXC compiles it, its validator
#   signing what it passes; on Windows, where the D3D12 backend builds.
# - <name>.slang.metal and <name>.cross.metal, Metal source by both of card
#   A29's routes (Slang's own, and Slang to SPIR-V to tools/spudgpu_msl,
#   card S20, in the Metal backend's binding layout), kept as build outputs
#   on every platform. Metal 3.1 (Apple7 and later, macOS 14, iOS 17).
# - <name>.metallib on Apple platforms, from the route the shader names
#   (tools/spudgpu_msl's by default, card S21), for this build's platform:
#   macOS, an iOS device or the iOS simulator (from CMAKE_OSX_SYSROOT), and
#   CMAKE_OSX_DEPLOYMENT_TARGET where it is set. Run on macOS 2026-11-01.
# - <name>.<format>.inc, each binary as a C array (uint32_t words, zero padded
#   to a whole word, and <c_name>_<format>_size, the exact byte count), for a
#   test to include.
#
# The tools come from the Vulkan SDK (VULKAN_SDK), as glslc did; their
# versions are printed at configure time.

set(SPUD_SLANG_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/spud_test_shaders)
set(_spud_sdk_bin "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
find_program(SPUD_SLANGC slangc HINTS ${_spud_sdk_bin} REQUIRED)
find_program(SPUD_SPIRV_VAL spirv-val HINTS ${_spud_sdk_bin} REQUIRED)
if(WIN32)
    find_program(SPUD_DXC dxc HINTS ${_spud_sdk_bin} REQUIRED)
endif()
execute_process(COMMAND ${SPUD_SLANGC} -version OUTPUT_VARIABLE _spud_v ERROR_VARIABLE _spud_v OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
message(STATUS "spudlib test shaders: slangc ${_spud_v} (${SPUD_SLANGC})")
if(WIN32)
    execute_process(COMMAND ${SPUD_DXC} --version OUTPUT_VARIABLE _spud_v OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(STATUS "spudlib test shaders: ${_spud_v} (${SPUD_DXC})")
endif()

# spud_add_slang_shader(<name> SOURCE <file.slang>
#     DXIL_PROFILE <profile> [SPIRV_CAPABILITIES <cap>...] [METAL_ROUTE SLANG <reason>])
#
# The source names its stage ([shader("compute")]...). <profile> is the
# shader model DXC compiles for (cs_6_5...). SPIR-V is 1.5, which Vulkan
# 1.2 and later take, plus the capabilities named. METAL_ROUTE SLANG makes
# the .metallib from Slang's own Metal source instead of the tool's, with
# the reason written beside the call (card S21). Adds <name>'s outputs to
# SPUD_SLANG_OUTPUTS in the caller's scope. The entry point is the source's
# stem; a test names it with the module.
function(spud_add_slang_shader name)
    cmake_parse_arguments(ARG "" "SOURCE;DXIL_PROFILE" "SPIRV_CAPABILITIES;METAL_ROUTE" ${ARGN})
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE})
    # The entry point is the file's stem (rq_trace.slang: rq_trace) in every
    # format, never main, which Metal reserves.
    get_filename_component(entry ${ARG_SOURCE} NAME_WE)
    set(out ${SPUD_SLANG_OUT_DIR}/${name})
    set(embed ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spud_embed.cmake)
    set(caps "")
    foreach(cap IN LISTS ARG_SPIRV_CAPABILITIES)
        list(APPEND caps -capability ${cap})
    endforeach()

    add_custom_command(
        OUTPUT ${out}.spv ${out}.spv.inc
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SPUD_SLANG_OUT_DIR}
        COMMAND ${SPUD_SLANGC} ${src} -entry ${entry} -target spirv -profile spirv_1_5 ${caps}
                -fvk-use-entrypoint-name -matrix-layout-column-major
                -warnings-as-errors all -o ${out}.spv
        COMMAND ${SPUD_SPIRV_VAL} --target-env vulkan1.2 ${out}.spv
        COMMAND ${CMAKE_COMMAND} -DIN=${out}.spv -DOUT=${out}.spv.inc -DNAME=${name}_spirv -P ${embed}
        DEPENDS ${src} ${embed}
        COMMENT "Slang: ${ARG_SOURCE} to SPIR-V (spirv-val)")
    set(outputs ${out}.spv.inc)

    if(WIN32)
        add_custom_command(
            OUTPUT ${out}.hlsl ${out}.dxil ${out}.dxil.inc
            COMMAND ${SPUD_SLANGC} ${src} -entry ${entry} -target hlsl -profile ${ARG_DXIL_PROFILE}
                    -matrix-layout-column-major
                    -warnings-as-errors all -o ${out}.hlsl
            COMMAND ${SPUD_DXC} -nologo -T ${ARG_DXIL_PROFILE} -E ${entry} -WX ${out}.hlsl -Fo ${out}.dxil
            COMMAND ${CMAKE_COMMAND} -DIN=${out}.dxil -DOUT=${out}.dxil.inc -DNAME=${name}_dxil -P ${embed}
            DEPENDS ${src} ${embed}
            COMMENT "Slang: ${ARG_SOURCE} to DXIL (DXC, validated)")
        list(APPEND outputs ${out}.dxil.inc)
    endif()

    # Both Metal routes' source, kept (card S21: which one a .metallib is
    # made from).
    add_custom_command(
        OUTPUT ${out}.slang.metal ${out}.cross.metal
        COMMAND ${SPUD_SLANGC} ${src} -entry ${entry} -target metal -matrix-layout-column-major -o ${out}.slang.metal
        COMMAND ${SPUDGPU_MSL_EXECUTABLE} --platform $<IF:$<PLATFORM_ID:iOS>,ios,macos> --msl-version 30100
                ${out}.spv ${out}.cross.metal
        DEPENDS ${src} ${out}.spv spudgpu_msl
        COMMENT "Slang: ${ARG_SOURCE} to Metal source, both routes")
    list(APPEND outputs ${out}.slang.metal ${out}.cross.metal)

    # The SDK Apple's Metal compiler builds for, and its deployment target flag,
    # so a .metallib loads wherever the code beside it does: macOS, an iOS
    # device or the iOS simulator (from CMAKE_OSX_SYSROOT), at
    # CMAKE_OSX_DEPLOYMENT_TARGET where it is set.
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
    if(APPLE)
        set(metal_src ${out}.cross.metal)
        list(GET ARG_METAL_ROUTE 0 route)
        if(route STREQUAL "SLANG")
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
    endif()

    set(SPUD_SLANG_OUTPUTS ${SPUD_SLANG_OUTPUTS} ${outputs} PARENT_SCOPE)
endfunction()
