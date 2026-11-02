cmake_minimum_required(VERSION 3.30)

# The version is written once, in include/spudcore.h; read it from there.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/spudcore.h" _spud_version_lines
     REGEX "^#define SPUDLIB_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*SPUDLIB_VERSION_${_part} ([0-9]+).*" "\\1"
	       SPUDLIB_VERSION_${_part} "${_spud_version_lines}")
	if(NOT SPUDLIB_VERSION_${_part} MATCHES "^[0-9]+$")
		message(FATAL_ERROR "spudlib: no SPUDLIB_VERSION_${_part} line in include/spudcore.h")
	endif()
endforeach()
project(spudlib VERSION ${SPUDLIB_VERSION_MAJOR}.${SPUDLIB_VERSION_MINOR}.${SPUDLIB_VERSION_PATCH})


# Define the allowed choices array
set(VALID_BACKENDS "Vulkan" "D3D12" "Metal")

# Initialize the cache variable with a default value
set(GRAPHICS_BACKEND "Vulkan" CACHE STRING "Choose exactly one graphics backend to compile")

# Create the drop-down menu restriction for interactive GUIs
set_property(CACHE GRAPHICS_BACKEND PROPERTY STRINGS ${VALID_BACKENDS})

# STRICT VALIDATION: Force selection to match exactly one choice from the list
if(NOT GRAPHICS_BACKEND IN_LIST VALID_BACKENDS)
    string(REPLACE ";" ", " PRINTABLE_BACKENDS "${VALID_BACKENDS}")
    message(FATAL_ERROR
        "Invalid backend selection: '${GRAPHICS_BACKEND}'.\n"
        "You must choose EXACTLY ONE option from the following list: [${PRINTABLE_BACKENDS}]"
    )
endif()

# Proceed with your clean compile-time single macro definitions...
message(STATUS "Selected Graphics Backend: ${GRAPHICS_BACKEND}")

# --- vendored-or-fetched dependency ----------------------------------------
# Probe eqdev/.deps/<dir> for a hand-vendored checkout and use it if present
# (one copy shared across the whole workspace, no per-repo duplication),
# otherwise fetch <repo> at the pinned <tag> so a standalone clone of this repo
# still configures with nothing vendored by hand. Probe <dir>/CMakeLists.txt,
# not just the directory, so an empty placeholder folder doesn't match.
include(FetchContent)
function(spud_declare_vendored_or_fetch name dir repo tag)
    # Accept either the plain `git clone` folder name (<dir>) or the
    # FetchContent-style <dir>-src, so a vendored checkout is found without
    # renaming. Probe <cand>/CMakeLists.txt, not just the directory, so an
    # empty placeholder folder doesn't match.
    foreach(_cand "${dir}" "${dir}-src")
        set(_vendored "${CMAKE_CURRENT_SOURCE_DIR}/../.deps/${_cand}")
        if(EXISTS "${_vendored}/CMakeLists.txt")
            message(STATUS "${name}: using vendored ${_vendored}")
            FetchContent_Declare(${name} SOURCE_DIR "${_vendored}")
            return()
        endif()
    endforeach()
    message(STATUS "${name}: no vendored copy under .deps/, fetching ${repo}@${tag}")
    FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${tag}")
endfunction()

# --- workspace-or-fetched dependency ----------------------------------------
# For an Endqueue repository that sits beside this one in the workspace
# (eqdev/<dir>): use that checkout when it is there, whatever is checked out
# and edits included, so a change made on both sides at once builds as one;
# otherwise fetch <repo> at <commit>, a commit hash, never a branch, so a
# standalone clone of this repo still configures and builds what was tested.
# Adds the target <name> unless something already added it (a workspace
# build that added the checkout first). The project's own tests are left to
# its top-level build: added here, PROJECT_IS_TOP_LEVEL is false there.
function(spud_add_workspace_or_fetch name dir repo commit)
    if(TARGET ${name})
        return()
    endif()
    set(_sibling "${CMAKE_CURRENT_SOURCE_DIR}/../${dir}")
    if(EXISTS "${_sibling}/CMakeLists.txt")
        message(STATUS "${name}: using the workspace's ${_sibling}")
        add_subdirectory("${_sibling}" "${CMAKE_CURRENT_BINARY_DIR}/${dir}")
    else()
        message(STATUS "${name}: no ../${dir} beside spudlib, fetching ${repo}@${commit}")
        FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${commit}")
        FetchContent_MakeAvailable(${name})
    endif()
endfunction()

# Don't do anything if another library/application is already compiling SpudLib
if(NOT TARGET spudlib)
    # Set initial SpudLib sources
    set(SPUDLIB_SOURCES
        include/spudaudio.h
        include/spudcore.h
        include/spudfiles.h
        include/spudgpu.h
        include/spudinput.h
        include/spudmemory.h
        include/spudnet.h
        include/spudperf.h
        include/spudrandom.h
        src/net/spudnetshared.h
        src/input/spudinputshared.h
        src/spudcore.c
        src/spudmemory.c
        src/memory/spudmemoryshared.h
        src/spudfiles.c
        src/files/spudfilesshared.h
        src/spudperf.c
        src/spudrandom.c
        src/spudnet.c
        src/net/spudnethttp.c
        src/net/spudnetwaitset.c
        src/spudaudio.c
        src/spudinput.c
        src/gpu/spudgpuformat.c
        src/gpu/spudgpushared.c
        src/gpu/spudgpushared.h
    )
    # Determine which backend sources to append.
    if(WIN32)
        list(APPEND SPUDLIB_SOURCES
            src/files/backends/windows/spudfileswindows.c
            src/net/backends/windows/spudnetwindows.c
            src/net/backends/windows/spudnetwinhttp.c
            src/net/backends/windows/spudnetwinhttphead.h
            src/memory/backends/windows/spudmemorywindows.c
        )
    endif()
    if(UNIX AND NOT APPLE)
        list(APPEND SPUDLIB_SOURCES
            src/files/backends/posix/spudfilesposix.c
            src/net/backends/posix/spudnetsockets.c
            src/net/backends/linux/spudnetcurl.c
            src/memory/backends/posix/spudmemoryposix.c
        )
    endif()
    if(APPLE)
        # SpudNet's HTTP/WebSocket backend is Objective-C (NSURLSession), on
        # every graphics backend. Must enable OBJC before add_library() below.
        enable_language(OBJC)
        list(APPEND SPUDLIB_SOURCES
            src/files/backends/posix/spudfilesposix.c
            src/memory/backends/posix/spudmemoryposix.c
            src/net/backends/posix/spudnetsockets.c
            src/net/backends/apple/spudneturlsession.m
        )
    endif()

    # SpudInput: the native input APIs built for the target platform, published
    # as PUBLIC SPUDINPUT_COMPILE_<api> definitions (after add_library). See
    # the table in include/spudinput.h. The front end (src/spudinput.c) is in
    # every build; with no API it has nothing to bind an instance to.
    set(SPUDINPUT_APIS "")
    if(APPLE)
        # GameController is on macOS, iOS and iPadOS; watchOS has none.
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(SPUDINPUT_APIS GAMECONTROLLER)
        endif()
    elseif(UNIX)
        # The kernel's event devices, read with nothing but its own headers.
        set(SPUDINPUT_APIS EVDEV)
    elseif(WIN32)
        # GameInput, from the Windows SDK (GameInput.h, gameinput.lib).
        set(SPUDINPUT_APIS GAMEINPUT)
    endif()
    message(STATUS "SpudInput: native APIs: ${SPUDINPUT_APIS}")
    # The backends are not compiled on their own: src/spudinput.c includes
    # each one built in, so that the front end's switch on the instance's
    # API inlines them (see src/input/spudinputshared.h).
    if("GAMECONTROLLER" IN_LIST SPUDINPUT_APIS)
        # The GameController backend is Objective-C, so the front end that
        # includes it is compiled as Objective-C (OBJC is already enabled
        # for Apple above).
        set_source_files_properties(src/spudinput.c PROPERTIES LANGUAGE OBJC)
    endif()
    if(MSVC)
        # A SPUDINPUT_BACKEND_FUNCTION MSVC didn't inline (C4714, level 4)
        # fails the build instead of becoming a call.
        set_source_files_properties(src/spudinput.c PROPERTIES COMPILE_OPTIONS "/w14714;/we4714")
    endif()

    # SpudAudio: the target platform and the native audio APIs built for it,
    # decided once here. They pick the sources and link libraries, and become
    # the PUBLIC SPUDAUDIO_PLATFORM_<platform> / SPUDAUDIO_COMPILE_<api>
    # definitions (after add_library), so SpudAudio's code and its hosts see
    # the same answer. See the table in include/spudaudio.h.
    set(SPUDAUDIO_PLATFORM "")
    set(SPUDAUDIO_APIS "")
    if(WIN32)
        set(SPUDAUDIO_PLATFORM WINDOWS)
        set(SPUDAUDIO_APIS WASAPI)
    elseif(APPLE)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(SPUDAUDIO_PLATFORM MACOS)
            set(SPUDAUDIO_APIS COREAUDIO)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS") # iPhone and iPad
            set(SPUDAUDIO_PLATFORM IOS)          # no backend yet (AVAudioSession + RemoteIO)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
            set(SPUDAUDIO_PLATFORM WATCHOS)      # no backend yet
        else()
            message(FATAL_ERROR "SpudAudio: unsupported Apple platform '${CMAKE_SYSTEM_NAME}'")
        endif()
    elseif(UNIX)
        set(SPUDAUDIO_PLATFORM LINUX)
        set(SPUDAUDIO_APIS ALSA)
    endif()
    message(STATUS "SpudAudio: platform ${SPUDAUDIO_PLATFORM}, native APIs: ${SPUDAUDIO_APIS}")

    if("WASAPI" IN_LIST SPUDAUDIO_APIS)
        list(APPEND SPUDLIB_SOURCES src/audio/backends/windows/spudaudiowasapi.c)
    endif()
    if("ALSA" IN_LIST SPUDAUDIO_APIS)
        list(APPEND SPUDLIB_SOURCES src/audio/backends/linux/spudaudioalsa.c)
    endif()
    if("COREAUDIO" IN_LIST SPUDAUDIO_APIS)
        list(APPEND SPUDLIB_SOURCES src/audio/backends/apple/spudaudiocoreaudio.c)
    endif()
    if(SPUDAUDIO_PLATFORM MATCHES "^(MACOS|IOS|WATCHOS)$")
        # Shared by every Apple audio backend; builds on all Apple OSes.
        list(APPEND SPUDLIB_SOURCES
            src/audio/backends/apple/spudaudioapple.h
            src/audio/backends/apple/spudaudioapple.c
        )
    endif()
    if(NOT SPUDAUDIO_APIS)
        # No native API for this platform yet: argument checks only.
        list(APPEND SPUDLIB_SOURCES src/audio/backends/stub/spudaudiostub.c)
    endif()
    if(WIN32 OR(UNIX AND NOT APPLE))
        # Append Vulkan sources if Vulkan API is selected for compile.
        if(GRAPHICS_BACKEND STREQUAL "Vulkan")
            list(APPEND SPUDLIB_SOURCES
                src/gpu/backends/vulkan/spudgpuvulkan.h
                src/gpu/backends/vulkan/spudgpuvulkancontext.c
                src/gpu/backends/vulkan/spudgpuvulkanbuffer.c
                src/gpu/backends/vulkan/spudgpuvulkanheap.c
                src/gpu/backends/vulkan/spudgpuvulkanshader.c
                src/gpu/backends/vulkan/spudgpuvulkanswapchain.c
                src/gpu/backends/vulkan/spudgpuvulkanimage.c
                src/gpu/backends/vulkan/spudgpuvulkandescriptors.c
                src/gpu/backends/vulkan/spudgpuvulkancommand.c
                src/gpu/backends/vulkan/spudgpuvulkanrenderpass.c
                src/gpu/backends/vulkan/spudgpuvulkannative.c
                src/gpu/backends/vulkan/spudgpuvulkansync.c
                src/gpu/backends/vulkan/spudgpuvulkanquery.c
                src/gpu/backends/vulkan/spudgpuvulkanraytracing.c
                include/spudgpu_vulkan_natives.h
            )

        # Append D3D12 sources if D3D12 API is selected for compile.
        elseif(GRAPHICS_BACKEND STREQUAL "D3D12")
            list(APPEND SPUDLIB_SOURCES
                src/gpu/backends/d3d12/spudgpud3d12.hpp
                src/gpu/backends/d3d12/spudgpud3d12context.cpp
                src/gpu/backends/d3d12/spudgpud3d12buffer.cpp
                src/gpu/backends/d3d12/spudgpud3d12heap.cpp
                src/gpu/backends/d3d12/spudgpud3d12shader.cpp
                src/gpu/backends/d3d12/spudgpud3d12swapchain.cpp
                src/gpu/backends/d3d12/spudgpud3d12image.cpp
                src/gpu/backends/d3d12/spudgpud3d12descriptors.cpp
                src/gpu/backends/d3d12/spudgpud3d12command.cpp
                src/gpu/backends/d3d12/spudgpud3d12renderpass.cpp
                src/gpu/backends/d3d12/spudgpud3d12native.cpp
                src/gpu/backends/d3d12/spudgpud3d12sync.cpp
                src/gpu/backends/d3d12/spudgpud3d12query.cpp
                src/gpu/backends/d3d12/spudgpud3d12raytracing.cpp
                include/spudgpu_d3d12_natives.h
            )
        endif()

        # Append SDL3 support for Windows and Linux systems, but not Apple.
        list(APPEND SPUDLIB_SOURCES
            include/spudgpu_sdl3.h
        )
    elseif(APPLE)
        # Append Metal sources if Metal API is selected for compile.
        if (GRAPHICS_BACKEND STREQUAL "Metal")
            # Metal backend is plain Objective-C (.m), not Objective-C++ -
            # chosen for lowest compile overhead (matches the Vulkan
            # backend's plain-C compile speed rather than D3D12's C++
            # backend). Must enable OBJC before add_library() below.
            enable_language(OBJC)

            list(APPEND SPUDLIB_SOURCES
                include/spudgpu_metal_natives.h
                src/gpu/backends/metal/spudgpumetalcontext.m
                src/gpu/backends/metal/spudgpumetalbuffer.m
                src/gpu/backends/metal/spudgpumetalheap.m
                src/gpu/backends/metal/spudgpumetalshader.m
                src/gpu/backends/metal/spudgpumetalswapchain.m
                src/gpu/backends/metal/spudgpumetalimage.m
                src/gpu/backends/metal/spudgpumetalformat.m
                src/gpu/backends/metal/spudgpumetaldescriptors.m
                src/gpu/backends/metal/spudgpumetalcommand.m
                src/gpu/backends/metal/spudgpumetalrenderpass.m
                src/gpu/backends/metal/spudgpumetalnative.m
                src/gpu/backends/metal/spudgpumetalsync.m
                src/gpu/backends/metal/spudgpumetalquery.m
                src/gpu/backends/metal/spudgpumetalraytracing.m
            )
        endif()
    endif()

    # After determing source backends, add_library to SpudLib target.
    add_library(spudlib STATIC ${SPUDLIB_SOURCES})

    # eqstd: the containers SpudLib's sources grow their arrays with
    # (EQ_ARRAY, through the instance's SPUD_ALLOCATOR; src/spudallocator.h).
    # Header-only, and PRIVATE: no public header includes it, so no caller
    # sees its types and SpudLib's ABI never depends on their layout.
    spud_add_workspace_or_fetch(eqstd eqstd
        https://github.com/endqueue/eqstd.git
        b302f96b6aa644ab59a8b9cd23d88aaa4aa5b2dd) # eqstd 0.1.0
    target_link_libraries(spudlib PRIVATE eqstd)

    # _DEBUG in a Debug build, for every compiler. MSVC's debug CRT defines it
    # already (CMAKE_MSVC_RUNTIME_LIBRARY pairs the two per config); GCC and
    # Clang define nothing, so without this a Debug build there leaves out
    # everything under `#if _DEBUG`. PUBLIC because spudcore.h and spudgpu.h
    # test it: structs in them change with it, so spudlib and whatever links
    # it must agree.
    if(NOT MSVC)
        target_compile_definitions(spudlib PUBLIC $<$<CONFIG:Debug>:_DEBUG>)
    endif()

    # Now set the compile-time macros and packages and whatnot.
    if(WIN32)
        target_compile_definitions(spudlib PUBLIC SPUDLIB_PLATFORM_WIN32=1)
        target_compile_definitions(spudlib PUBLIC SPUDLIB_PLATFORM_WINDOWS=1)
        # winhttp: SpudNet's HTTP and WebSocket clients. crypt32: their
        # certificate checks against a caller's own roots and pins.
        target_link_libraries(spudlib PRIVATE shlwapi ws2_32 winhttp crypt32 bcrypt)
    elseif (UNIX AND NOT APPLE)
        target_compile_definitions(spudlib PUBLIC SPUDLIB_PLATFORM_LINUX=1)
        # SpudNet's HTTP and WebSocket clients (and its instance): libcurl4-openssl-dev /
        # libcurl-devel, and pthreads for the WebSocket's lock. All of SpudNet
        # needs libcurl 7.84 or later: a spudnet_instance may be made while
        # other threads run, and libcurl's global init is only safe then from
        # that version on. It also has to have been built thread-safe, which
        # only the library itself can say; spudnet_instance_create asks it at
        # run time. WebSocket needs 8.11 or later and otherwise compiles to
        # entry points that return SPUDRESULT_SPUDNET_UNSUPPORTED.
        find_package(CURL 7.84 REQUIRED)
        find_package(Threads REQUIRED)
        target_link_libraries(spudlib PRIVATE CURL::libcurl Threads::Threads)
        # libm: spudlib calls libm (sqrtf, etc.) internally. On Linux that's a
        # separate DSO; Windows folds it into the CRT and Apple into libSystem,
        # so it's only linked here. spudlib builds STATIC, so this propagates to
        # consumers' final link line too - which is also why a sample or host
        # that calls libm itself doesn't have to repeat it.
        target_link_libraries(spudlib PRIVATE m)
        if(CURL_VERSION_STRING VERSION_LESS "8.11")
            message(WARNING "SpudNet: libcurl ${CURL_VERSION_STRING} is older than 8.11 - spudnet_websocket_* will return SPUDRESULT_SPUDNET_UNSUPPORTED.")
        endif()
    elseif (APPLE)
        target_compile_definitions(spudlib PUBLIC SPUDLIB_PLATFORM_APPLE=1)
        # SPUDLIB_PLATFORM_APPLE is every Apple system. SpudNet has to tell
        # Apple Watch apart: the system refuses TCP sockets and WebSockets to
        # an ordinary application there, and spudnet.h leaves both out
        # (SPUDNET_EXT_TCP, SPUDNET_EXT_WEBSOCKET) - and stops the build if
        # this is missing from a watchOS one. The TCP and wait-set sources
        # stay in the list above; with both parts out they compile to nothing.
        if(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
            target_compile_definitions(spudlib PUBLIC SPUDLIB_PLATFORM_WATCHOS=1)
        endif()
        # Foundation: NSURLSession, for SpudNet's HTTP and WebSocket clients.
        # Security: SecTrust, for their certificate checks.
        target_link_libraries(spudlib PRIVATE "-framework Foundation" "-framework Security")
    endif()

    # SpudInput definitions and libraries, from the choice made above.
    foreach(api IN LISTS SPUDINPUT_APIS)
        target_compile_definitions(spudlib PUBLIC SPUDINPUT_COMPILE_${api}=1)
    endforeach()
    if("GAMECONTROLLER" IN_LIST SPUDINPUT_APIS)
        # CoreHaptics is what GameController plays a gamepad's motors through.
        target_link_libraries(spudlib PRIVATE "-framework GameController" "-framework CoreHaptics" "-framework Foundation")
    endif()
    if("GAMEINPUT" IN_LIST SPUDINPUT_APIS)
        target_link_libraries(spudlib PRIVATE gameinput)
    endif()

    # SpudAudio definitions and libraries, from the choice made above.
    if(SPUDAUDIO_PLATFORM)
        target_compile_definitions(spudlib PUBLIC SPUDAUDIO_PLATFORM_${SPUDAUDIO_PLATFORM}=1)
    endif()
    foreach(api IN LISTS SPUDAUDIO_APIS)
        target_compile_definitions(spudlib PUBLIC SPUDAUDIO_COMPILE_${api}=1)
    endforeach()
    if("WASAPI" IN_LIST SPUDAUDIO_APIS)
        # ole32: COM (CoCreateInstance / CoIncrementMTAUsage).
        # avrt: MMCSS (AvSetMmThreadCharacteristicsW) for the audio thread.
        target_link_libraries(spudlib PRIVATE ole32 avrt)
    endif()
    if("ALSA" IN_LIST SPUDAUDIO_APIS)
        # libasound2-dev / alsa-lib-devel, and pthreads for the audio thread.
        find_package(ALSA REQUIRED)
        find_package(Threads REQUIRED)
        # libudev-dev / systemd-devel: udev's "sound" events, which are how
        # device-change notification hears of a card (ALSA reports none).
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SPUDAUDIO_LIBUDEV REQUIRED IMPORTED_TARGET libudev)
        target_link_libraries(spudlib PRIVATE ALSA::ALSA Threads::Threads PkgConfig::SPUDAUDIO_LIBUDEV)
    endif()
    if("COREAUDIO" IN_LIST SPUDAUDIO_APIS)
        # The HAL, and CoreFoundation for its CFString names.
        target_link_libraries(spudlib PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
    endif()
    if(SPUDAUDIO_PLATFORM MATCHES "^(MACOS|IOS)$")
        # AudioToolbox expands channel-layout tags; the watchOS SDK has none.
        target_link_libraries(spudlib PRIVATE "-framework AudioToolbox")
    endif()

    if(WIN32 OR(UNIX AND NOT APPLE))
        # Set the Vulkan compile-time packages and macros.
        if(GRAPHICS_BACKEND STREQUAL "Vulkan")
            set_property(TARGET spudlib PROPERTY C_STANDARD 23)
            # The Vulkan backend counts with <stdatomic.h>, which MSVC's C
            # compiler only provides behind this switch.
            if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
                target_compile_options(spudlib PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
            endif()
            target_compile_definitions(spudlib PUBLIC SPUDGPU_COMPILE_VULKAN_API=1)

            # On Windows find_package(Vulkan) finds the SDK through the VULKAN_SDK
            # variable its installer sets, whatever version is installed.
            if(UNIX AND NOT APPLE)
                # The Vulkan WSI surface type is compiled in (spudgpuvulkanswapchain.c
                # switches on SPUDGPU_PLATFORM_XLIB / SPUDGPU_PLATFORM_WAYLAND), so it
                # must match the window handles the caller will pass: an X11
                # Display*/Window, or a wl_display*/wl_surface*. This is the caller's
                # choice, NOT something SpudLib sniffs from whichever dev packages
                # happen to be installed on the build machine - auto-detecting it
                # silently built a Wayland surface creator that then crashed in Mesa's
                # WSI when handed X11 handles. Pick it explicitly via SPUDGPU_LINUX_WSI;
                # Xlib is the default because it also works under XWayland.
                set(SPUDGPU_LINUX_WSI "Xlib" CACHE STRING "Linux Vulkan WSI surface type: Xlib or Wayland")
                set_property(CACHE SPUDGPU_LINUX_WSI PROPERTY STRINGS "Xlib" "Wayland")
                if(SPUDGPU_LINUX_WSI STREQUAL "Wayland")
                    find_package(PkgConfig REQUIRED)
                    pkg_check_modules(WAYLAND REQUIRED wayland-client)
                    target_compile_definitions(spudlib PUBLIC SPUDGPU_PLATFORM_WAYLAND VK_USE_PLATFORM_WAYLAND_KHR)
                    target_link_libraries(spudlib PRIVATE ${WAYLAND_LIBRARIES})
                    target_include_directories(spudlib PRIVATE ${WAYLAND_INCLUDE_DIRS})
                elseif(SPUDGPU_LINUX_WSI STREQUAL "Xlib")
                    target_compile_definitions(spudlib PUBLIC SPUDGPU_PLATFORM_XLIB VK_USE_PLATFORM_XLIB_KHR)
                    target_link_libraries(spudlib PRIVATE X11) # X11 is usually available without find_package on Linux
                else()
                    message(FATAL_ERROR "SPUDGPU_LINUX_WSI must be 'Xlib' or 'Wayland', got '${SPUDGPU_LINUX_WSI}'")
                endif()
            endif()

            # Add Vulkan packages after setting up everything else for Vulkan.
            find_package(Vulkan REQUIRED)
            target_include_directories(spudlib PRIVATE ${Vulkan_INCLUDE_DIRS})
            target_link_libraries(spudlib PRIVATE Vulkan::Vulkan)
        endif()

        # Set the D3D12 compile-time packages and macros.
        if(GRAPHICS_BACKEND STREQUAL "D3D12")
            # CMake knows no C++26 flag for MSVC; its C++23 maps to /std:c++latest,
            # the newest dialect MSVC offers.
            if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
                set_property(TARGET spudlib PROPERTY CXX_STANDARD 23)
            else()
                set_property(TARGET spudlib PROPERTY CXX_STANDARD 26)
            endif()
            set_property(TARGET spudlib PROPERTY CXX_SCAN_FOR_MODULES OFF)
            target_compile_definitions(spudlib PUBLIC SPUDGPU_COMPILE_D3D12_API=1)

            # SpudGPU takes DXIL and compiles nothing (card A9, revised): no
            # SPIRV-Cross and no DXC here since 2026-10-31.
            if(WIN32)
                target_link_libraries(spudlib PRIVATE
                    d3d12
                    dxgi
                    dxguid
                )
            else()
                return()
            endif()
        endif()

        # Set up SDL3 stuff
        # Path to the folder containing SDL3Config.cmake
        # Usually: [SDL3-folder]/lib/cmake/SDL3
        if(WIN32)
            set(SDL3_DIR "../SDL3-3.4.8/cmake")
        endif()

        find_package(SDL3 REQUIRED)
        target_link_libraries(spudlib PRIVATE SDL3::SDL3)

        # Copy DLL to output folder (So the app actually runs)
        if(WIN32)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            )
        endif()
    elseif(APPLE)
        # Set the Metal compile-time packages and macros.
        if(GRAPHICS_BACKEND STREQUAL "Metal")
            target_compile_definitions(spudlib PUBLIC SPUDGPU_COMPILE_METAL_API=1)

            # Required to link against id<MTLDevice>/id<MTLCommandQueue>/etc.
            # QuartzCore is needed for CAMetalLayer (surface/swapchain
            # creation); Foundation for NSObject-family types Metal.h pulls in.
            target_link_libraries(spudlib PRIVATE
                "-framework Metal"
                "-framework QuartzCore"
                "-framework Foundation"
            )

            # SpudGPU takes a .metallib and compiles nothing (card A9,
            # revised): no SPIRV-Cross here since 2026-10-31; the caller's
            # build makes Metal source (tools/spudgpu_msl for SPIR-V, card
            # S20) and Apple's compiler the library.

            # Command-list encoding, image/buffer resources, and the swap
            # chain are implemented and verified against real Apple hardware
            # - see src/gpu/backends/metal/*.m. include/spudgpu_metal_natives.h
            # is drafted and mostly implemented alongside them.
        endif()
    endif()

    target_precompile_headers(spudlib PRIVATE include/spudgpu.h)

    target_include_directories(spudlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # The shader step (cmake/spud_shaders.cmake, spud_add_shader()) is
    # written once for this repository's tests and for any caller's build.
    # Its Metal route's cross-compiler, tools/spudgpu_msl (card S20), is a
    # program of its own built for the build machine (card X18), never linked
    # into spudlib and never a target here. A caller includes the file named
    # by SPUD_SHADERS_CMAKE.
    set(SPUD_SHADERS_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spud_shaders.cmake CACHE INTERNAL
        "spudlib's shader step, for a caller's build to include")

    # Hardware tests: each needs a real device (and spudaudio_sine a
    # listener), so they're built but not registered with ctest.
    # SpudNet's fuzz targets (tests/fuzz), replayed as ctests below and
    # built for libFuzzer under SPUDLIB_BUILD_FUZZERS.
    set(SPUDLIB_FUZZ_TARGETS
        spudnet_fuzz_http_head
        spudnet_fuzz_certificate
        spudnet_fuzz_text
        spudnet_fuzz_proxy)

    option(SPUDLIB_BUILD_TESTS "Build SpudLib's hardware tests" ON)
    if(SPUDLIB_BUILD_TESTS)
        # SpudPerf's clocks and its wait. No hardware and nobody watching, so
        # this one is a ctest.
        enable_testing()
        # SpudCore: the version, as the header, CMake and the library each
        # have it, and every result's name. Needs nothing, so a ctest.
        add_executable(spudcore_test tests/spudcore_test.c)
        target_link_libraries(spudcore_test PRIVATE spudlib)
        target_compile_definitions(spudcore_test PRIVATE
            SPUDCORE_TEST_CMAKE_VERSION="${PROJECT_VERSION}")
        add_test(NAME spudcore_test COMMAND spudcore_test)

        # Every public function called with every argument zeroed: written
        # by tests/misuse/make_misuse_tests.py from abi.txt, never by hand.
        add_executable(spud_misuse_tests tests/spud_misuse_tests.c)
        target_link_libraries(spud_misuse_tests PRIVATE spudlib)
        add_test(NAME spud_misuse_tests COMMAND spud_misuse_tests)

        # No allocation of SpudLib's per frame (ROADMAP.md in the workspace,
        # section 3.1): each module's hot path counted after a warm-up,
        # through tests/alloctrace.h. Needs a GPU, an output device and a
        # loopback connection for the parts that use them, each skipped with
        # its reason when missing, and nothing heard (it writes silence), so
        # a ctest. --adapter N runs SpudGPU's part on another adapter.
        add_executable(spud_noalloc_tests tests/spud_noalloc_tests.c)
        target_link_libraries(spud_noalloc_tests PRIVATE spudlib)
        # tests/alloctrace.h names a frame with the C++ runtime's demangler
        # (__cxa_demangle, to pass over operator new and std::); spudlib has
        # no C++ on Apple, so the test is linked as C++ to bring the runtime in.
        set_target_properties(spud_noalloc_tests PROPERTIES LINKER_LANGUAGE CXX)
        if(APPLE AND GRAPHICS_BACKEND STREQUAL "Metal")
            target_sources(spud_noalloc_tests PRIVATE tests/spudgpu_test_metal_layer.m)
            target_link_libraries(spud_noalloc_tests PRIVATE "-framework QuartzCore" "-framework Foundation")
        endif()
        if(UNIX AND NOT APPLE)
            target_link_libraries(spud_noalloc_tests PRIVATE ${CMAKE_DL_LIBS})
        endif()
        add_test(NAME spud_noalloc_tests COMMAND spud_noalloc_tests)

        # SpudGPU's adapters and devices: needs a GPU, no window and nobody
        # watching, so a ctest. With no adapter it exits 77, reported as
        # skipped.
        add_executable(spudgpu_device_test tests/spudgpu_device_test.c)
        target_link_libraries(spudgpu_device_test PRIVATE spudlib)
        add_test(NAME spudgpu_device_test COMMAND spudgpu_device_test)
        set_tests_properties(spudgpu_device_test PROPERTIES SKIP_RETURN_CODE 77)

        # SpudGPU calls nothing else in the workspace makes (formats by name,
        # a CPU-signalled fence, semaphores, buffer, view and heap descs, a
        # pool reset). Needs a GPU, no window; 77, skipped, with no adapter.
        add_executable(spudgpu_objects_test tests/spudgpu_objects_test.c)
        # eqstd for src/spudallocator.h, whose spud_object_head the test
        # makes a surface of for the checks before a surface is read.
        target_link_libraries(spudgpu_objects_test PRIVATE spudlib eqstd)

        # The tests' shaders, Slang compiled to each API's own format at
        # build time (cmake/spud_shaders.cmake), for spudgpu_objects_test,
        # spud_noalloc_tests and spud_misuse_tests. The ray query pair is
        # card S12's step 0: compiled and validated on every build, run on a
        # device by spudgpu_objects_test once SPUDGPU_EXT_RAY_QUERY exists.
        include(${SPUD_SHADERS_CMAKE})
        set(SPUD_TEST_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/spud_test_shaders)
        set(SPUD_TEST_SHADERS
            # name                               source                          DXIL profile  SPIR-V capability
            "spudgpu_test_rq_trace|rq_trace|cs_6_5|spvRayQueryKHR"
            "spudgpu_test_rq_shadow|rq_shadow|ps_6_5|spvRayQueryKHR"
            # The vertex shader under rq_shadow, and the caller's GPU
            # writers of instances, one per layout kind (card S14).
            "spudgpu_test_rq_shadow_vert|rq_shadow_vert|vs_6_0|"
            "spudgpu_test_rq_instances_row64|rq_instances_row64|cs_6_0|"
            "spudgpu_test_rq_instances_col72|rq_instances_col72|cs_6_0|"
            "spudgpu_test_objects_draw_vert|objects_draw_vert|vs_6_0|"
            "spudgpu_test_objects_draw_frag|objects_draw_frag|ps_6_0|"
            "spudgpu_test_objects_draw_mesh|objects_draw_mesh|ms_6_5|spvMeshShadingEXT"
            "spudgpu_test_objects_double_comp|objects_double_comp|cs_6_0|"
            # Every binding of a set (card S27): a set of a uniform buffer at
            # 0, a sampled image at 5 and a sampler at 8, declared whole and
            # read at 5 and 8 (the build pads 0); and on Metal the modules
            # refused, declaring the set in part, with a binding it lacks,
            # with one of another type, and with one of another count.
            "spudgpu_test_bindings_vert|bindings_vert|vs_6_0|"
            "spudgpu_test_bindings_whole_frag|bindings_whole_frag|ps_6_0|"
            "spudgpu_test_bindings_part_frag|bindings_part_frag|ps_6_0|"
            "spudgpu_test_bindings_extra_frag|bindings_extra_frag|ps_6_0|"
            "spudgpu_test_bindings_type_frag|bindings_type_frag|ps_6_0|"
            "spudgpu_test_bindings_count_frag|bindings_count_frag|ps_6_0|"
            "spudgpu_test_bindings_whole_comp|bindings_whole_comp|cs_6_0|"
            "spudgpu_test_bindings_part_comp|bindings_part_comp|cs_6_0|"
            "spudgpu_test_bindings_extra_comp|bindings_extra_comp|cs_6_0|"
            "spudgpu_test_bindings_type_comp|bindings_type_comp|cs_6_0|"
            "spudgpu_test_bindings_none_comp|bindings_none_comp|cs_6_0|")
        set(SPUD_TEST_SHADER_OUTPUTS "")
        foreach(_shader IN LISTS SPUD_TEST_SHADERS)
            string(REPLACE "|" ";" _shader "${_shader}")
            list(GET _shader 0 _name)
            list(GET _shader 1 _source)
            list(GET _shader 2 _profile)
            list(GET _shader 3 _cap)
            set(_caps "")
            if(_cap)
                set(_caps SPIRV_CAPABILITIES ${_cap})
            endif()
            spud_add_shader(${_name} SOURCE tests/shaders/${_source}.slang OUT_DIR ${SPUD_TEST_SHADER_DIR}
                DXIL_PROFILE ${_profile} ${_caps} OUTPUTS SPUD_TEST_SHADER_OUTPUTS)
        endforeach()
        add_custom_target(spudlib_test_shaders DEPENDS ${SPUD_TEST_SHADER_OUTPUTS})
        # What SPUDGPU_EXT_RAY_QUERY costs on one adapter (tests/spudgpu_rq_measure.c):
        # a bottom level's build and compaction, top levels of 1 to 100,000
        # instances, a ray per pixel at 1920 x 1080. It measures and checks
        # nothing else, and needs a GPU with ray queries: not a ctest.
        add_executable(spudgpu_rq_measure tests/spudgpu_rq_measure.c)
        target_link_libraries(spudgpu_rq_measure PRIVATE spudlib)
        foreach(_shader_test spudgpu_objects_test spud_noalloc_tests spud_misuse_tests spudgpu_rq_measure)
            add_dependencies(${_shader_test} spudlib_test_shaders)
            target_include_directories(${_shader_test} PRIVATE ${SPUD_TEST_SHADER_DIR})
            if(WIN32)
                target_compile_definitions(${_shader_test} PRIVATE SPUDLIB_TEST_SHADERS_DXIL=1)
            endif()
            if(APPLE)
                target_compile_definitions(${_shader_test} PRIVATE SPUDLIB_TEST_SHADERS_METALLIB=1)
            endif()
        endforeach()
        # On Metal a surface is a CAMetalLayer, which needs no window, so the
        # test makes swap chains on one.
        if(APPLE AND GRAPHICS_BACKEND STREQUAL "Metal")
            target_sources(spudgpu_objects_test PRIVATE tests/spudgpu_test_metal_layer.m)
            target_link_libraries(spudgpu_objects_test PRIVATE "-framework QuartzCore" "-framework Foundation")
        endif()
        add_test(NAME spudgpu_objects_test COMMAND spudgpu_objects_test)
        set_tests_properties(spudgpu_objects_test PROPERTIES SKIP_RETURN_CODE 77)

        # SpudFiles' file calls, in a temporary directory. Needs nothing
        # else, so a ctest; exits 77, skipped, with no temporary directory.
        add_executable(spudfiles_test tests/spudfiles_test.c)
        target_link_libraries(spudfiles_test PRIVATE spudlib)
        add_test(NAME spudfiles_test COMMAND spudfiles_test)
        set_tests_properties(spudfiles_test PROPERTIES SKIP_RETURN_CODE 77)

        # SpudMemory's virtual memory calls. Needs nothing, so a ctest.
        add_executable(spudmemory_test tests/spudmemory_test.c)
        target_link_libraries(spudmemory_test PRIVATE spudlib)
        add_test(NAME spudmemory_test COMMAND spudmemory_test)

        add_executable(spudperf_test tests/spudperf_test.c)
        target_link_libraries(spudperf_test PRIVATE spudlib)
        add_test(NAME spudperf_test COMMAND spudperf_test)

        add_executable(spudrandom_test tests/spudrandom_test.c)
        target_link_libraries(spudrandom_test PRIVATE spudlib)
        add_test(NAME spudrandom_test COMMAND spudrandom_test)

        # SpudInput's front end, and whatever gamepads happen to be plugged
        # in. It passes with none, so it is a ctest too.
        add_executable(spudinput_test tests/spudinput_test.c)
        # eqstd for src/input/spudinputshared.h, whose instance starts with
        # a spud_object_head (src/spudallocator.h, which includes eqstd).
        target_link_libraries(spudinput_test PRIVATE spudlib eqstd)
        if(APPLE)
            # The test runs the main run loop itself, which is how
            # GameController's devices arrive.
            target_link_libraries(spudinput_test PRIVATE "-framework CoreFoundation")
        endif()
        add_test(NAME spudinput_test COMMAND spudinput_test)

        # SpudAudio's calls, with nothing started and nothing heard: passes
        # with no device, so a ctest. spudaudio_sine below is the one that
        # plays.
        add_executable(spudaudio_test tests/spudaudio_test.c)
        target_link_libraries(spudaudio_test PRIVATE spudlib)
        add_test(NAME spudaudio_test COMMAND spudaudio_test)
        if(UNIX AND NOT APPLE)
            # Under the -asan and -tsan presets: what PipeWire and ALSA's
            # pipewire plugin report from inside snd_pcm_open and _close,
            # third-party code, each entry with its reason in the file. Set
            # on the test so it holds in every build that adds spudlib.
            set_tests_properties(spudaudio_test PROPERTIES ENVIRONMENT_MODIFICATION
                "LSAN_OPTIONS=string_append::suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/linux-lsan.supp;TSAN_OPTIONS=string_append::suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/linux-tsan.supp")
        endif()

        add_executable(spudaudio_sine tests/spudaudio_sine.c)
        target_link_libraries(spudaudio_sine PRIVATE spudlib)
        if(UNIX AND NOT APPLE)
            target_link_libraries(spudaudio_sine PRIVATE m)
        endif()

        # SpudNet against a real network stack. It needs the server in
        # tests/spudnet_test_server.py running beside it (Python 3 and the
        # openssl command), which tests/run_spudnet_test.sh sees to, so it
        # isn't registered with ctest either.
        add_executable(spudnet_test tests/spudnet_test.c)
        target_link_libraries(spudnet_test PRIVATE spudlib)
        if(UNIX)
            find_package(Threads REQUIRED)
            target_link_libraries(spudnet_test PRIVATE Threads::Threads)
        endif()

        # SpudNet's parsers of the network's bytes, each fuzz target in
        # tests/fuzz run over its seed corpus and every input the fuzzer
        # once crashed on (tests/fuzz/regressions/<target>), with an
        # ordinary main instead of libFuzzer's: no network, so ctests, and
        # on every compiler. The targets reach SpudNet's internal functions
        # (src/net/spudnetshared.h, which includes eqstd) in the static
        # library.
        foreach(_fuzz IN LISTS SPUDLIB_FUZZ_TARGETS)
            add_executable(${_fuzz}_replay tests/fuzz/${_fuzz}.c tests/fuzz/spudfuzz_replay.c)
            target_include_directories(${_fuzz}_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${_fuzz}_replay PRIVATE spudlib eqstd)
            add_test(NAME ${_fuzz}_replay COMMAND ${_fuzz}_replay
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus/${_fuzz}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/regressions/${_fuzz})
        endforeach()
    endif()

    # The same targets under libFuzzer, for the macos-fuzz and linux-fuzz
    # presets (clang only; Apple's has no libFuzzer, Homebrew's LLVM does).
    # The presets build the whole library with -fsanitize=fuzzer-no-link and
    # AddressSanitizer + UndefinedBehaviorSanitizer; here each target adds
    # libFuzzer's main. How to run them: tests/fuzz/README.md.
    option(SPUDLIB_BUILD_FUZZERS "Build SpudNet's libFuzzer targets (clang)" OFF)
    if(SPUDLIB_BUILD_FUZZERS)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "SPUDLIB_BUILD_FUZZERS needs clang with libFuzzer (-fsanitize=fuzzer)")
        endif()
        foreach(_fuzz IN LISTS SPUDLIB_FUZZ_TARGETS)
            add_executable(${_fuzz} tests/fuzz/${_fuzz}.c)
            target_include_directories(${_fuzz} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_options(${_fuzz} PRIVATE -fsanitize=fuzzer)
            target_link_options(${_fuzz} PRIVATE -fsanitize=fuzzer)
            target_link_libraries(${_fuzz} PRIVATE spudlib eqstd)
        endforeach()
    endif()
endif()
