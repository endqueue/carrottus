cmake_minimum_required(VERSION 3.30)

# The version is written once, in include/crcore.h; read it from there.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/crcore.h" _cr_version_lines
     REGEX "^#define CARROTTUS_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
foreach(_part MAJOR MINOR PATCH)
	string(REGEX REPLACE ".*CARROTTUS_VERSION_${_part} ([0-9]+).*" "\\1"
	       CARROTTUS_VERSION_${_part} "${_cr_version_lines}")
	if(NOT CARROTTUS_VERSION_${_part} MATCHES "^[0-9]+$")
		message(FATAL_ERROR "carrottus: no CARROTTUS_VERSION_${_part} line in include/crcore.h")
	endif()
endforeach()
project(carrottus VERSION ${CARROTTUS_VERSION_MAJOR}.${CARROTTUS_VERSION_MINOR}.${CARROTTUS_VERSION_PATCH})


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
function(cr_declare_vendored_or_fetch name dir repo tag)
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
function(cr_add_workspace_or_fetch name dir repo commit)
    if(TARGET ${name})
        return()
    endif()
    set(_sibling "${CMAKE_CURRENT_SOURCE_DIR}/../${dir}")
    if(EXISTS "${_sibling}/CMakeLists.txt")
        message(STATUS "${name}: using the workspace's ${_sibling}")
        add_subdirectory("${_sibling}" "${CMAKE_CURRENT_BINARY_DIR}/${dir}")
    else()
        message(STATUS "${name}: no ../${dir} beside carrottus, fetching ${repo}@${commit}")
        FetchContent_Declare(${name} GIT_REPOSITORY "${repo}" GIT_TAG "${commit}")
        FetchContent_MakeAvailable(${name})
    endif()
endfunction()

# Don't do anything if another library/application is already compiling Carrottus
if(NOT TARGET carrottus)
    # Set initial Carrottus sources
    set(CARROTTUS_SOURCES
        include/craudio.h
        include/crcore.h
        include/crfiles.h
        include/crgpu.h
        include/crinput.h
        include/crmemory.h
        include/crnet.h
        include/crperf.h
        include/crrandom.h
        src/net/crnetshared.h
        src/input/crinputshared.h
        src/crcore.c
        src/crmessage.h
        src/crmemory.c
        src/memory/crmemoryshared.h
        src/crfiles.c
        src/files/crfilesshared.h
        src/crperf.c
        src/crrandom.c
        src/crnet.c
        src/net/crnethttp.c
        src/net/crnetwaitset.c
        src/craudio.c
        src/crinput.c
        src/gpu/crgpuformat.c
        src/gpu/crgpushared.c
        src/gpu/crgpushared.h
    )
    # Determine which backend sources to append.
    if(WIN32)
        list(APPEND CARROTTUS_SOURCES
            src/files/backends/windows/crfileswindows.c
            src/net/backends/windows/crnetwindows.c
            src/net/backends/windows/crnetwinhttp.c
            src/net/backends/windows/crnetwinhttphead.h
            src/memory/backends/windows/crmemorywindows.c
        )
    endif()
    if(UNIX AND NOT APPLE)
        list(APPEND CARROTTUS_SOURCES
            src/files/backends/posix/crfilesposix.c
            src/net/backends/posix/crnetsockets.c
            src/net/backends/linux/crnetcurl.c
            src/memory/backends/posix/crmemoryposix.c
        )
    endif()
    if(APPLE)
        # CrNet's HTTP/WebSocket backend is Objective-C (NSURLSession), on
        # every graphics backend. Must enable OBJC before add_library() below.
        enable_language(OBJC)
        list(APPEND CARROTTUS_SOURCES
            src/files/backends/posix/crfilesposix.c
            src/memory/backends/posix/crmemoryposix.c
            src/net/backends/posix/crnetsockets.c
            src/net/backends/apple/crneturlsession.m
        )
    endif()

    # CrInput: the native input APIs built for the target platform, published
    # as PUBLIC CRINPUT_COMPILE_<api> definitions (after add_library). See
    # the table in include/crinput.h. The front end (src/crinput.c) is in
    # every build; with no API it has nothing to bind an instance to.
    set(CRINPUT_APIS "")
    if(APPLE)
        # GameController is on macOS, iOS and iPadOS; watchOS has none.
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
            set(CRINPUT_APIS GAMECONTROLLER)
        endif()
    elseif(UNIX)
        # The kernel's event devices, read with nothing but its own headers.
        set(CRINPUT_APIS EVDEV)
    elseif(WIN32)
        # GameInput, from the Windows SDK (GameInput.h, gameinput.lib).
        set(CRINPUT_APIS GAMEINPUT)
    endif()
    message(STATUS "CrInput: native APIs: ${CRINPUT_APIS}")
    # The backends are not compiled on their own: src/crinput.c includes
    # each one built in, so that the front end's switch on the instance's
    # API inlines them (see src/input/crinputshared.h).
    if("GAMECONTROLLER" IN_LIST CRINPUT_APIS)
        # The GameController backend is Objective-C, so the front end that
        # includes it is compiled as Objective-C (OBJC is already enabled
        # for Apple above).
        set_source_files_properties(src/crinput.c PROPERTIES LANGUAGE OBJC)
    endif()
    if(MSVC)
        # A CRINPUT_BACKEND_FUNCTION MSVC didn't inline (C4714, level 4)
        # fails the build instead of becoming a call.
        set_source_files_properties(src/crinput.c PROPERTIES COMPILE_OPTIONS "/w14714;/we4714")
    endif()

    # CrAudio: the target platform and the native audio APIs built for it,
    # decided once here. They pick the sources and link libraries, and become
    # the PUBLIC CRAUDIO_PLATFORM_<platform> / CRAUDIO_COMPILE_<api>
    # definitions (after add_library), so CrAudio's code and its hosts see
    # the same answer. See the table in include/craudio.h.
    set(CRAUDIO_PLATFORM "")
    set(CRAUDIO_APIS "")
    if(WIN32)
        set(CRAUDIO_PLATFORM WINDOWS)
        set(CRAUDIO_APIS WASAPI)
    elseif(APPLE)
        if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set(CRAUDIO_PLATFORM MACOS)
            set(CRAUDIO_APIS COREAUDIO)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS") # iPhone and iPad, one backend
            set(CRAUDIO_PLATFORM IOS)
            set(CRAUDIO_APIS REMOTEIO)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
            set(CRAUDIO_PLATFORM WATCHOS)      # no backend yet
        else()
            message(FATAL_ERROR "CrAudio: unsupported Apple platform '${CMAKE_SYSTEM_NAME}'")
        endif()
    elseif(UNIX)
        set(CRAUDIO_PLATFORM LINUX)
        set(CRAUDIO_APIS ALSA)
    endif()
    message(STATUS "CrAudio: platform ${CRAUDIO_PLATFORM}, native APIs: ${CRAUDIO_APIS}")

    if("WASAPI" IN_LIST CRAUDIO_APIS)
        list(APPEND CARROTTUS_SOURCES src/audio/backends/windows/craudiowasapi.c)
    endif()
    if("ALSA" IN_LIST CRAUDIO_APIS)
        list(APPEND CARROTTUS_SOURCES src/audio/backends/linux/craudioalsa.c)
    endif()
    if("COREAUDIO" IN_LIST CRAUDIO_APIS)
        list(APPEND CARROTTUS_SOURCES src/audio/backends/apple/craudiocoreaudio.c)
    endif()
    if("REMOTEIO" IN_LIST CRAUDIO_APIS)
        # The audio unit in C; the AVAudioSession half, which reads the
        # session and hears its notifications, in Objective-C.
        list(APPEND CARROTTUS_SOURCES
            src/audio/backends/apple/craudiosession.h
            src/audio/backends/apple/craudiosession.m
            src/audio/backends/apple/craudioremoteio.c
        )
    endif()
    if(CRAUDIO_PLATFORM MATCHES "^(MACOS|IOS|WATCHOS)$")
        # Shared by every Apple audio backend; builds on all Apple OSes.
        list(APPEND CARROTTUS_SOURCES
            src/audio/backends/apple/craudioapple.h
            src/audio/backends/apple/craudioapple.c
        )
    endif()
    if(NOT CRAUDIO_APIS)
        # No native API for this platform yet: argument checks only.
        list(APPEND CARROTTUS_SOURCES src/audio/backends/stub/craudiostub.c)
    endif()
    if(WIN32 OR(UNIX AND NOT APPLE))
        # Append Vulkan sources if Vulkan API is selected for compile.
        if(GRAPHICS_BACKEND STREQUAL "Vulkan")
            list(APPEND CARROTTUS_SOURCES
                src/gpu/backends/vulkan/crgpuvulkan.h
                src/gpu/backends/vulkan/crgpuvulkancontext.c
                src/gpu/backends/vulkan/crgpuvulkanbuffer.c
                src/gpu/backends/vulkan/crgpuvulkanheap.c
                src/gpu/backends/vulkan/crgpuvulkanshader.c
                src/gpu/backends/vulkan/crgpuvulkanswapchain.c
                src/gpu/backends/vulkan/crgpuvulkanimage.c
                src/gpu/backends/vulkan/crgpuvulkandescriptors.c
                src/gpu/backends/vulkan/crgpuvulkancommand.c
                src/gpu/backends/vulkan/crgpuvulkanrenderpass.c
                src/gpu/backends/vulkan/crgpuvulkannative.c
                src/gpu/backends/vulkan/crgpuvulkansync.c
                src/gpu/backends/vulkan/crgpuvulkanquery.c
                src/gpu/backends/vulkan/crgpuvulkanraytracing.c
                include/crgpu_vulkan_natives.h
            )

        # Append D3D12 sources if D3D12 API is selected for compile.
        elseif(GRAPHICS_BACKEND STREQUAL "D3D12")
            list(APPEND CARROTTUS_SOURCES
                src/gpu/backends/d3d12/crgpud3d12.hpp
                src/gpu/backends/d3d12/crgpud3d12context.cpp
                src/gpu/backends/d3d12/crgpud3d12buffer.cpp
                src/gpu/backends/d3d12/crgpud3d12heap.cpp
                src/gpu/backends/d3d12/crgpud3d12shader.cpp
                src/gpu/backends/d3d12/crgpud3d12swapchain.cpp
                src/gpu/backends/d3d12/crgpud3d12image.cpp
                src/gpu/backends/d3d12/crgpud3d12descriptors.cpp
                src/gpu/backends/d3d12/crgpud3d12command.cpp
                src/gpu/backends/d3d12/crgpud3d12renderpass.cpp
                src/gpu/backends/d3d12/crgpud3d12native.cpp
                src/gpu/backends/d3d12/crgpud3d12sync.cpp
                src/gpu/backends/d3d12/crgpud3d12query.cpp
                src/gpu/backends/d3d12/crgpud3d12raytracing.cpp
                include/crgpu_d3d12_natives.h
            )
        endif()

        # Append SDL3 support for Windows and Linux systems, but not Apple.
        list(APPEND CARROTTUS_SOURCES
            include/crgpu_sdl3.h
        )
    elseif(APPLE)
        # Append Metal sources if Metal API is selected for compile.
        if (GRAPHICS_BACKEND STREQUAL "Metal")
            # Metal backend is plain Objective-C (.m), not Objective-C++ -
            # chosen for lowest compile overhead (matches the Vulkan
            # backend's plain-C compile speed rather than D3D12's C++
            # backend). Must enable OBJC before add_library() below.
            enable_language(OBJC)

            list(APPEND CARROTTUS_SOURCES
                include/crgpu_metal_natives.h
                src/gpu/backends/metal/crgpumetalcontext.m
                src/gpu/backends/metal/crgpumetalbuffer.m
                src/gpu/backends/metal/crgpumetalheap.m
                src/gpu/backends/metal/crgpumetalshader.m
                src/gpu/backends/metal/crgpumetalswapchain.m
                src/gpu/backends/metal/crgpumetalimage.m
                src/gpu/backends/metal/crgpumetalformat.m
                src/gpu/backends/metal/crgpumetaldescriptors.m
                src/gpu/backends/metal/crgpumetalcommand.m
                src/gpu/backends/metal/crgpumetalrenderpass.m
                src/gpu/backends/metal/crgpumetalnative.m
                src/gpu/backends/metal/crgpumetalsync.m
                src/gpu/backends/metal/crgpumetalquery.m
                src/gpu/backends/metal/crgpumetalraytracing.m
            )
        endif()
    endif()

    # After determing source backends, add_library to Carrottus target.
    add_library(carrottus STATIC ${CARROTTUS_SOURCES})

    # eqstd: the containers Carrottus's sources grow their arrays with
    # (EQ_ARRAY, through the instance's CR_ALLOCATOR; src/crallocator.h).
    # Header-only, and PRIVATE: no public header includes it, so no caller
    # sees its types and Carrottus's ABI never depends on their layout.
    cr_add_workspace_or_fetch(eqstd eqstd
        https://github.com/endqueue/eqstd.git
        b302f96b6aa644ab59a8b9cd23d88aaa4aa5b2dd) # eqstd 0.1.0
    target_link_libraries(carrottus PRIVATE eqstd)

    # _DEBUG in a Debug build, for every compiler. MSVC's debug CRT defines it
    # already (CMAKE_MSVC_RUNTIME_LIBRARY pairs the two per config); GCC and
    # Clang define nothing, so without this a Debug build there leaves out
    # everything under `#if _DEBUG`. PUBLIC because crcore.h and crgpu.h
    # test it: structs in them change with it, so carrottus and whatever links
    # it must agree.
    if(NOT MSVC)
        target_compile_definitions(carrottus PUBLIC $<$<CONFIG:Debug>:_DEBUG>)
    endif()

    # Now set the compile-time macros and packages and whatnot.
    if(WIN32)
        target_compile_definitions(carrottus PUBLIC CARROTTUS_PLATFORM_WIN32=1)
        target_compile_definitions(carrottus PUBLIC CARROTTUS_PLATFORM_WINDOWS=1)
        # winhttp: CrNet's HTTP and WebSocket clients. crypt32: their
        # certificate checks against a caller's own roots and pins.
        target_link_libraries(carrottus PRIVATE shlwapi ws2_32 winhttp crypt32 bcrypt)
    elseif (UNIX AND NOT APPLE)
        target_compile_definitions(carrottus PUBLIC CARROTTUS_PLATFORM_LINUX=1)
        # CrNet's HTTP and WebSocket clients (and its instance): libcurl4-openssl-dev /
        # libcurl-devel, and pthreads for the WebSocket's lock. All of CrNet
        # needs libcurl 7.84 or later: a crnet_instance may be made while
        # other threads run, and libcurl's global init is only safe then from
        # that version on. It also has to have been built thread-safe, which
        # only the library itself can say; crnet_instance_create asks it at
        # run time. WebSocket needs 8.11 or later and otherwise compiles to
        # entry points that return CR_RESULT_NET_UNSUPPORTED.
        find_package(CURL 7.84 REQUIRED)
        find_package(Threads REQUIRED)
        target_link_libraries(carrottus PRIVATE CURL::libcurl Threads::Threads)
        # libm: carrottus calls libm (sqrtf, etc.) internally. On Linux that's a
        # separate DSO; Windows folds it into the CRT and Apple into libSystem,
        # so it's only linked here. carrottus builds STATIC, so this propagates to
        # consumers' final link line too - which is also why a sample or host
        # that calls libm itself doesn't have to repeat it.
        target_link_libraries(carrottus PRIVATE m)
        # CrFiles' library calls (dlopen, dlsym, dlclose, dlerror): in libc
        # from glibc 2.34, in libdl before it; CMAKE_DL_LIBS names whichever.
        target_link_libraries(carrottus PRIVATE ${CMAKE_DL_LIBS})
        if(CURL_VERSION_STRING VERSION_LESS "8.11")
            message(WARNING "CrNet: libcurl ${CURL_VERSION_STRING} is older than 8.11 - crnet_websocket_* will return CR_RESULT_NET_UNSUPPORTED.")
        endif()
    elseif (APPLE)
        target_compile_definitions(carrottus PUBLIC CARROTTUS_PLATFORM_APPLE=1)
        # CARROTTUS_PLATFORM_APPLE is every Apple system. CrNet has to tell
        # Apple Watch apart: the system refuses TCP sockets and WebSockets to
        # an ordinary application there, and crnet.h leaves both out
        # (CRNET_EXT_TCP, CRNET_EXT_WEBSOCKET) - and stops the build if
        # this is missing from a watchOS one. The TCP and wait-set sources
        # stay in the list above; with both parts out they compile to nothing.
        if(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
            target_compile_definitions(carrottus PUBLIC CARROTTUS_PLATFORM_WATCHOS=1)
        endif()
        # Foundation: NSURLSession, for CrNet's HTTP and WebSocket clients.
        # Security: SecTrust, for their certificate checks.
        target_link_libraries(carrottus PRIVATE "-framework Foundation" "-framework Security")
    endif()

    # CrInput definitions and libraries, from the choice made above.
    foreach(api IN LISTS CRINPUT_APIS)
        target_compile_definitions(carrottus PUBLIC CRINPUT_COMPILE_${api}=1)
    endforeach()
    if("GAMECONTROLLER" IN_LIST CRINPUT_APIS)
        # CoreHaptics is what GameController plays a gamepad's motors through.
        target_link_libraries(carrottus PRIVATE "-framework GameController" "-framework CoreHaptics" "-framework Foundation")
    endif()
    if("GAMEINPUT" IN_LIST CRINPUT_APIS)
        target_link_libraries(carrottus PRIVATE gameinput)
    endif()

    # CrAudio definitions and libraries, from the choice made above.
    if(CRAUDIO_PLATFORM)
        target_compile_definitions(carrottus PUBLIC CRAUDIO_PLATFORM_${CRAUDIO_PLATFORM}=1)
    endif()
    foreach(api IN LISTS CRAUDIO_APIS)
        target_compile_definitions(carrottus PUBLIC CRAUDIO_COMPILE_${api}=1)
    endforeach()
    if("WASAPI" IN_LIST CRAUDIO_APIS)
        # ole32: COM (CoCreateInstance / CoIncrementMTAUsage).
        # avrt: MMCSS (AvSetMmThreadCharacteristicsW) for the audio thread.
        target_link_libraries(carrottus PRIVATE ole32 avrt)
    endif()
    if("ALSA" IN_LIST CRAUDIO_APIS)
        # libasound2-dev / alsa-lib-devel, and pthreads for the audio thread.
        find_package(ALSA REQUIRED)
        find_package(Threads REQUIRED)
        # libudev-dev / systemd-devel: udev's "sound" events, which are how
        # device-change notification hears of a card (ALSA reports none).
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(CRAUDIO_LIBUDEV REQUIRED IMPORTED_TARGET libudev)
        target_link_libraries(carrottus PRIVATE ALSA::ALSA Threads::Threads PkgConfig::CRAUDIO_LIBUDEV)
    endif()
    if("COREAUDIO" IN_LIST CRAUDIO_APIS)
        # The HAL, and CoreFoundation for its CFString names.
        target_link_libraries(carrottus PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
    endif()
    if("REMOTEIO" IN_LIST CRAUDIO_APIS)
        # AVAudioSession and AVAudioApplication (AVFAudio), Foundation for
        # the notification center; the audio unit is AudioToolbox's (below).
        target_link_libraries(carrottus PRIVATE "-framework AVFAudio" "-framework Foundation")
    endif()
    if(CRAUDIO_PLATFORM MATCHES "^(MACOS|IOS)$")
        # AudioToolbox expands channel-layout tags; the watchOS SDK has none.
        target_link_libraries(carrottus PRIVATE "-framework AudioToolbox")
    endif()

    if(WIN32 OR(UNIX AND NOT APPLE))
        # Set the Vulkan compile-time packages and macros.
        if(GRAPHICS_BACKEND STREQUAL "Vulkan")
            set_property(TARGET carrottus PROPERTY C_STANDARD 23)
            # The Vulkan backend counts with <stdatomic.h>, which MSVC's C
            # compiler only provides behind this switch.
            if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
                target_compile_options(carrottus PRIVATE $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
            endif()
            target_compile_definitions(carrottus PUBLIC CRGPU_COMPILE_VULKAN_API=1)

            # On Windows find_package(Vulkan) finds the SDK through the VULKAN_SDK
            # variable its installer sets, whatever version is installed.
            if(UNIX AND NOT APPLE)
                # The Vulkan WSI surface type is compiled in (crgpuvulkanswapchain.c
                # switches on CRGPU_PLATFORM_XLIB / CRGPU_PLATFORM_WAYLAND), so it
                # must match the window handles the caller will pass: an X11
                # Display*/Window, or a wl_display*/wl_surface*. This is the caller's
                # choice, NOT something Carrottus sniffs from whichever dev packages
                # happen to be installed on the build machine - auto-detecting it
                # silently built a Wayland surface creator that then crashed in Mesa's
                # WSI when handed X11 handles. Pick it explicitly via CRGPU_LINUX_WSI;
                # Xlib is the default because it also works under XWayland.
                set(CRGPU_LINUX_WSI "Xlib" CACHE STRING "Linux Vulkan WSI surface type: Xlib or Wayland")
                set_property(CACHE CRGPU_LINUX_WSI PROPERTY STRINGS "Xlib" "Wayland")
                if(CRGPU_LINUX_WSI STREQUAL "Wayland")
                    find_package(PkgConfig REQUIRED)
                    pkg_check_modules(WAYLAND REQUIRED wayland-client)
                    target_compile_definitions(carrottus PUBLIC CRGPU_PLATFORM_WAYLAND VK_USE_PLATFORM_WAYLAND_KHR)
                    target_link_libraries(carrottus PRIVATE ${WAYLAND_LIBRARIES})
                    target_include_directories(carrottus PRIVATE ${WAYLAND_INCLUDE_DIRS})
                elseif(CRGPU_LINUX_WSI STREQUAL "Xlib")
                    target_compile_definitions(carrottus PUBLIC CRGPU_PLATFORM_XLIB VK_USE_PLATFORM_XLIB_KHR)
                    target_link_libraries(carrottus PRIVATE X11) # X11 is usually available without find_package on Linux
                else()
                    message(FATAL_ERROR "CRGPU_LINUX_WSI must be 'Xlib' or 'Wayland', got '${CRGPU_LINUX_WSI}'")
                endif()
            endif()

            # Add Vulkan packages after setting up everything else for Vulkan.
            find_package(Vulkan REQUIRED)
            target_include_directories(carrottus PRIVATE ${Vulkan_INCLUDE_DIRS})
            target_link_libraries(carrottus PRIVATE Vulkan::Vulkan)
        endif()

        # Set the D3D12 compile-time packages and macros.
        if(GRAPHICS_BACKEND STREQUAL "D3D12")
            # CMake knows no C++26 flag for MSVC; its C++23 maps to /std:c++latest,
            # the newest dialect MSVC offers.
            if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
                set_property(TARGET carrottus PROPERTY CXX_STANDARD 23)
            else()
                set_property(TARGET carrottus PROPERTY CXX_STANDARD 26)
            endif()
            set_property(TARGET carrottus PROPERTY CXX_SCAN_FOR_MODULES OFF)
            target_compile_definitions(carrottus PUBLIC CRGPU_COMPILE_D3D12_API=1)

            # CrGPU takes DXIL and compiles nothing (card A9, revised): no
            # SPIRV-Cross and no DXC here since 2026-10-31.
            if(WIN32)
                target_link_libraries(carrottus PRIVATE
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
        target_link_libraries(carrottus PRIVATE SDL3::SDL3)

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
            target_compile_definitions(carrottus PUBLIC CRGPU_COMPILE_METAL_API=1)

            # Required to link against id<MTLDevice>/id<MTLCommandQueue>/etc.
            # QuartzCore is needed for CAMetalLayer (surface/swapchain
            # creation); Foundation for NSObject-family types Metal.h pulls in.
            target_link_libraries(carrottus PRIVATE
                "-framework Metal"
                "-framework QuartzCore"
                "-framework Foundation"
            )

            # CrGPU takes a .metallib and compiles nothing (card A9,
            # revised): no SPIRV-Cross here since 2026-10-31; the caller's
            # build makes Metal source (tools/crgpu_msl for SPIR-V, card
            # S20) and Apple's compiler the library.

            # Command-list encoding, image/buffer resources, and the swap
            # chain are implemented and verified against real Apple hardware
            # - see src/gpu/backends/metal/*.m. include/crgpu_metal_natives.h
            # is drafted and mostly implemented alongside them.
        endif()
    endif()

    target_precompile_headers(carrottus PRIVATE include/crgpu.h)

    target_include_directories(carrottus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

    # The shader step (cmake/cr_shaders.cmake, cr_add_shader()) is
    # written once for this repository's tests and for any caller's build.
    # Its Metal route's cross-compiler, tools/crgpu_msl (card S20), is a
    # program of its own built for the build machine (card X18), never linked
    # into carrottus and never a target here. A caller includes the file named
    # by CR_SHADERS_CMAKE.
    set(CR_SHADERS_CMAKE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cr_shaders.cmake CACHE INTERNAL
        "carrottus's shader step, for a caller's build to include")

    # Hardware tests: each needs a real device (and craudio_sine a
    # listener), so they're built but not registered with ctest.
    # CrNet's fuzz targets (tests/fuzz), replayed as ctests below and
    # built for libFuzzer under CARROTTUS_BUILD_FUZZERS.
    set(CARROTTUS_FUZZ_TARGETS
        crnet_fuzz_http_head
        crnet_fuzz_certificate
        crnet_fuzz_text
        crnet_fuzz_proxy)

    option(CARROTTUS_BUILD_TESTS "Build Carrottus's hardware tests" ON)
    if(CARROTTUS_BUILD_TESTS)
        # CrPerf's clocks and its wait. No hardware and nobody watching, so
        # this one is a ctest.
        enable_testing()
        # CrCore: the version, as the header, CMake and the library each
        # have it, and every result's name. Needs nothing, so a ctest.
        add_executable(crcore_test tests/crcore_test.c)
        target_link_libraries(crcore_test PRIVATE carrottus)
        target_compile_definitions(crcore_test PRIVATE
            CRCORE_TEST_CMAKE_VERSION="${PROJECT_VERSION}")
        add_test(NAME crcore_test COMMAND crcore_test)

        # Every public function called with every argument zeroed: written
        # by tests/misuse/make_misuse_tests.py from abi.txt, never by hand.
        add_executable(cr_misuse_tests tests/cr_misuse_tests.c)
        target_link_libraries(cr_misuse_tests PRIVATE carrottus)
        add_test(NAME cr_misuse_tests COMMAND cr_misuse_tests)

        # No allocation of Carrottus's per frame (ROADMAP.md in the workspace,
        # section 3.1): each module's hot path counted after a warm-up,
        # through tests/alloctrace.h. Needs a GPU, an output device and a
        # loopback connection for the parts that use them, each skipped with
        # its reason when missing, and nothing heard (it writes silence), so
        # a ctest. --adapter N runs CrGPU's part on another adapter.
        add_executable(cr_noalloc_tests tests/cr_noalloc_tests.c)
        target_link_libraries(cr_noalloc_tests PRIVATE carrottus)
        # tests/alloctrace.h names a frame with the C++ runtime's demangler
        # (__cxa_demangle, to pass over operator new and std::); carrottus has
        # no C++ on Apple, so the test is linked as C++ to bring the runtime in.
        set_target_properties(cr_noalloc_tests PROPERTIES LINKER_LANGUAGE CXX)
        if(APPLE AND GRAPHICS_BACKEND STREQUAL "Metal")
            target_sources(cr_noalloc_tests PRIVATE tests/crgpu_test_metal_layer.m)
            target_link_libraries(cr_noalloc_tests PRIVATE "-framework QuartzCore" "-framework Foundation")
        endif()
        if(UNIX AND NOT APPLE)
            target_link_libraries(cr_noalloc_tests PRIVATE ${CMAKE_DL_LIBS})
            # glibc declares dladdr and Dl_info (tests/alloctrace.h) only under
            # _GNU_SOURCE, which must come before the first system header; and
            # dladdr names only an executable's exported symbols, so it
            # exports them all.
            target_compile_definitions(cr_noalloc_tests PRIVATE _GNU_SOURCE)
            set_target_properties(cr_noalloc_tests PROPERTIES ENABLE_EXPORTS ON)
        endif()
        add_test(NAME cr_noalloc_tests COMMAND cr_noalloc_tests)

        # CrGPU's adapters and devices: needs a GPU, no window and nobody
        # watching, so a ctest. With no adapter it exits 77, reported as
        # skipped.
        add_executable(crgpu_device_test tests/crgpu_device_test.c)
        target_link_libraries(crgpu_device_test PRIVATE carrottus)
        add_test(NAME crgpu_device_test COMMAND crgpu_device_test)
        set_tests_properties(crgpu_device_test PROPERTIES SKIP_RETURN_CODE 77)

        # CrGPU calls nothing else in the workspace makes (formats by name,
        # a CPU-signalled fence, semaphores, buffer, view and heap descs, a
        # pool reset). Needs a GPU, no window; 77, skipped, with no adapter.
        add_executable(crgpu_objects_test tests/crgpu_objects_test.c)
        # eqstd for src/crallocator.h, whose cr_object_head the test
        # makes a surface of for the checks before a surface is read.
        target_link_libraries(crgpu_objects_test PRIVATE carrottus eqstd)

        # The tests' shaders, Slang compiled to each API's own format at
        # build time (cmake/cr_shaders.cmake), for crgpu_objects_test,
        # cr_noalloc_tests and cr_misuse_tests. The ray query pair is
        # card S12's step 0: compiled and validated on every build, run on a
        # device by crgpu_objects_test once CRGPU_EXT_RAY_QUERY exists.
        include(${CR_SHADERS_CMAKE})
        set(CR_TEST_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/cr_test_shaders)
        set(CR_TEST_SHADERS
            # name                               source                          DXIL profile  SPIR-V capability
            "crgpu_test_rq_trace|rq_trace|cs_6_5|spvRayQueryKHR"
            "crgpu_test_rq_shadow|rq_shadow|ps_6_5|spvRayQueryKHR"
            # The vertex shader under rq_shadow, and the caller's GPU
            # writers of instances, one per layout kind (card S14).
            "crgpu_test_rq_shadow_vert|rq_shadow_vert|vs_6_0|"
            "crgpu_test_rq_instances_row64|rq_instances_row64|cs_6_0|"
            "crgpu_test_rq_instances_col72|rq_instances_col72|cs_6_0|"
            "crgpu_test_objects_draw_vert|objects_draw_vert|vs_6_0|"
            "crgpu_test_objects_draw_frag|objects_draw_frag|ps_6_0|"
            "crgpu_test_objects_draw_mesh|objects_draw_mesh|ms_6_5|spvMeshShadingEXT"
            "crgpu_test_objects_double_comp|objects_double_comp|cs_6_0|"
            # An attachment image cleared first, then written by compute (card S38).
            "crgpu_test_objects_first_use_comp|objects_first_use_comp|cs_6_0|"
            # Every binding of a set (card S27): a set of a uniform buffer at
            # 0, a sampled image at 5 and a sampler at 8, declared whole and
            # read at 5 and 8 (the build pads 0); and on Metal the modules
            # refused, declaring the set in part, with a binding it lacks,
            # with one of another type, and with one of another count.
            "crgpu_test_bindings_vert|bindings_vert|vs_6_0|"
            "crgpu_test_bindings_whole_frag|bindings_whole_frag|ps_6_0|"
            "crgpu_test_bindings_part_frag|bindings_part_frag|ps_6_0|"
            "crgpu_test_bindings_extra_frag|bindings_extra_frag|ps_6_0|"
            "crgpu_test_bindings_type_frag|bindings_type_frag|ps_6_0|"
            "crgpu_test_bindings_count_frag|bindings_count_frag|ps_6_0|"
            "crgpu_test_bindings_whole_comp|bindings_whole_comp|cs_6_0|"
            "crgpu_test_bindings_part_comp|bindings_part_comp|cs_6_0|"
            "crgpu_test_bindings_extra_comp|bindings_extra_comp|cs_6_0|"
            "crgpu_test_bindings_type_comp|bindings_type_comp|cs_6_0|"
            "crgpu_test_bindings_none_comp|bindings_none_comp|cs_6_0|")
        set(CR_TEST_SHADER_OUTPUTS "")
        foreach(_shader IN LISTS CR_TEST_SHADERS)
            string(REPLACE "|" ";" _shader "${_shader}")
            list(GET _shader 0 _name)
            list(GET _shader 1 _source)
            list(GET _shader 2 _profile)
            list(GET _shader 3 _cap)
            set(_caps "")
            if(_cap)
                set(_caps SPIRV_CAPABILITIES ${_cap})
            endif()
            cr_add_shader(${_name} SOURCE tests/shaders/${_source}.slang OUT_DIR ${CR_TEST_SHADER_DIR}
                DXIL_PROFILE ${_profile} ${_caps} OUTPUTS CR_TEST_SHADER_OUTPUTS)
        endforeach()
        add_custom_target(carrottus_test_shaders DEPENDS ${CR_TEST_SHADER_OUTPUTS})
        # What CRGPU_EXT_RAY_QUERY costs on one adapter (tests/crgpu_rq_measure.c):
        # a bottom level's build and compaction, top levels of 1 to 100,000
        # instances, a ray per pixel at 1920 x 1080. It measures and checks
        # nothing else, and needs a GPU with ray queries: not a ctest.
        add_executable(crgpu_rq_measure tests/crgpu_rq_measure.c)
        target_link_libraries(crgpu_rq_measure PRIVATE carrottus)
        foreach(_shader_test crgpu_objects_test cr_noalloc_tests cr_misuse_tests crgpu_rq_measure)
            add_dependencies(${_shader_test} carrottus_test_shaders)
            target_include_directories(${_shader_test} PRIVATE ${CR_TEST_SHADER_DIR})
            if(WIN32)
                target_compile_definitions(${_shader_test} PRIVATE CARROTTUS_TEST_SHADERS_DXIL=1)
            endif()
            if(APPLE)
                target_compile_definitions(${_shader_test} PRIVATE CARROTTUS_TEST_SHADERS_METALLIB=1)
            endif()
        endforeach()
        # On Metal a surface is a CAMetalLayer, which needs no window, so the
        # test makes swap chains on one.
        if(APPLE AND GRAPHICS_BACKEND STREQUAL "Metal")
            target_sources(crgpu_objects_test PRIVATE tests/crgpu_test_metal_layer.m)
            target_link_libraries(crgpu_objects_test PRIVATE "-framework QuartzCore" "-framework Foundation")
        endif()
        add_test(NAME crgpu_objects_test COMMAND crgpu_objects_test)
        set_tests_properties(crgpu_objects_test PROPERTIES SKIP_RETURN_CODE 77)

        # CrFiles' file calls, in a temporary directory. Needs nothing
        # else, so a ctest; exits 77, skipped, with no temporary directory.
        add_executable(crfiles_test tests/crfiles_test.c)
        target_link_libraries(crfiles_test PRIVATE carrottus)
        # The lock test's flags are <stdatomic.h>'s (craudio_test, below,
        # says why MSVC needs these).
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            set_property(TARGET crfiles_test PROPERTY C_STANDARD 11)
            target_compile_options(crfiles_test PRIVATE /experimental:c11atomics)
        endif()
        add_test(NAME crfiles_test COMMAND crfiles_test)
        set_tests_properties(crfiles_test PROPERTIES SKIP_RETURN_CODE 77)

        # The dynamic libraries crfiles_test opens (card S44), one source
        # built several ways (tests/crfiles_test_library.c says which).
        # Their paths reach the test as definitions, so it needs no search.
        foreach(_lib a b)
            add_library(crfiles_test_library_${_lib} SHARED tests/crfiles_test_library.c)
            set_target_properties(crfiles_test_library_${_lib} PROPERTIES C_VISIBILITY_PRESET hidden)
        endforeach()
        target_compile_definitions(crfiles_test_library_a PRIVATE CRFILES_TEST_LIBRARY_VALUE=1)
        target_compile_definitions(crfiles_test_library_b PRIVATE CRFILES_TEST_LIBRARY_VALUE=2)
        # A dependency put where no search reaches it, and a library that
        # imports it, which the loader then can't load.
        add_library(crfiles_test_dependency SHARED tests/crfiles_test_library.c)
        target_compile_definitions(crfiles_test_dependency PRIVATE CRFILES_TEST_DEPENDENCY)
        set_target_properties(crfiles_test_dependency PROPERTIES
            C_VISIBILITY_PRESET hidden
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/crfiles_test_hidden/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/crfiles_test_hidden/$<CONFIG>")
        add_library(crfiles_test_library_dependent SHARED tests/crfiles_test_library.c)
        target_compile_definitions(crfiles_test_library_dependent PRIVATE
            CRFILES_TEST_LIBRARY_VALUE=3 CRFILES_TEST_LIBRARY_NEEDS_DEPENDENCY)
        target_link_libraries(crfiles_test_library_dependent PRIVATE crfiles_test_dependency)
        # No rpath to the dependency's directory: it stays out of reach.
        set_target_properties(crfiles_test_library_dependent PROPERTIES
            C_VISIBILITY_PRESET hidden SKIP_BUILD_RPATH ON)
        set(_crfiles_test_libraries
            CRFILES_TEST_LIBRARY_A="$<TARGET_FILE:crfiles_test_library_a>"
            CRFILES_TEST_LIBRARY_B="$<TARGET_FILE:crfiles_test_library_b>"
            CRFILES_TEST_LIBRARY_DEPENDENT="$<TARGET_FILE:crfiles_test_library_dependent>")
        # An initializer that fails: DllMain returning FALSE. A POSIX
        # constructor can't fail, so Windows only.
        if(WIN32)
            add_library(crfiles_test_library_failing SHARED tests/crfiles_test_library.c)
            target_compile_definitions(crfiles_test_library_failing PRIVATE
                CRFILES_TEST_LIBRARY_VALUE=4 CRFILES_TEST_LIBRARY_FAILS_INIT)
            list(APPEND _crfiles_test_libraries
                CRFILES_TEST_LIBRARY_FAILING="$<TARGET_FILE:crfiles_test_library_failing>")
            add_dependencies(crfiles_test crfiles_test_library_failing)
        endif()
        # On iOS, a framework in a Frameworks folder beside the test, where an
        # app's bundle keeps one (card S44, Q5a): the test is a bare
        # executable, not a bundle, so its folder stands in for one.
        if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
            add_library(crfiles_test_framework SHARED tests/crfiles_test_library.c)
            target_compile_definitions(crfiles_test_framework PRIVATE CRFILES_TEST_LIBRARY_VALUE=5)
            set_target_properties(crfiles_test_framework PROPERTIES
                C_VISIBILITY_PRESET hidden
                FRAMEWORK TRUE
                MACOSX_FRAMEWORK_IDENTIFIER com.endqueue.crfiles-test-framework
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/Frameworks")
            list(APPEND _crfiles_test_libraries
                CRFILES_TEST_FRAMEWORK="$<TARGET_FILE:crfiles_test_framework>")
            add_dependencies(crfiles_test crfiles_test_framework)
        endif()
        target_compile_definitions(crfiles_test PRIVATE ${_crfiles_test_libraries})
        add_dependencies(crfiles_test crfiles_test_library_a crfiles_test_library_b crfiles_test_library_dependent)

        # CrMemory's virtual memory calls. Needs nothing, so a ctest.
        add_executable(crmemory_test tests/crmemory_test.c)
        target_link_libraries(crmemory_test PRIVATE carrottus)
        add_test(NAME crmemory_test COMMAND crmemory_test)

        add_executable(crperf_test tests/crperf_test.c)
        target_link_libraries(crperf_test PRIVATE carrottus)
        add_test(NAME crperf_test COMMAND crperf_test)

        add_executable(crrandom_test tests/crrandom_test.c)
        target_link_libraries(crrandom_test PRIVATE carrottus)
        add_test(NAME crrandom_test COMMAND crrandom_test)

        # CrInput's front end, and whatever gamepads happen to be plugged
        # in. It passes with none, so it is a ctest too.
        add_executable(crinput_test tests/crinput_test.c)
        # eqstd for src/input/crinputshared.h, whose instance starts with
        # a cr_object_head (src/crallocator.h, which includes eqstd).
        target_link_libraries(crinput_test PRIVATE carrottus eqstd)
        if(APPLE)
            # The test runs the main run loop itself, which is how
            # GameController's devices arrive.
            target_link_libraries(crinput_test PRIVATE "-framework CoreFoundation")
        endif()
        add_test(NAME crinput_test COMMAND crinput_test)

        # CrAudio's calls, with nothing started and nothing heard: passes
        # with no device, so a ctest. craudio_sine below is the one that
        # plays.
        add_executable(craudio_test tests/craudio_test.c)
        target_link_libraries(craudio_test PRIVATE carrottus)
        # The callback's counts are <stdatomic.h>'s, which MSVC's C compiler
        # provides only in C11 or later and behind this switch, as for the
        # Vulkan backend above.
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            set_property(TARGET craudio_test PROPERTY C_STANDARD 11)
            target_compile_options(craudio_test PRIVATE /experimental:c11atomics)
        endif()
        add_test(NAME craudio_test COMMAND craudio_test)
        if(UNIX AND NOT APPLE)
            # Under the -asan and -tsan presets: what PipeWire and ALSA's
            # pipewire plugin report from inside snd_pcm_open and _close,
            # third-party code, each entry with its reason in the file. Set
            # on the test so it holds in every build that adds carrottus.
            set_tests_properties(craudio_test PROPERTIES ENVIRONMENT_MODIFICATION
                "LSAN_OPTIONS=string_append::suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/linux-lsan.supp;TSAN_OPTIONS=string_append::suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/sanitizers/linux-tsan.supp")
        endif()

        add_executable(craudio_sine tests/craudio_sine.c)
        target_link_libraries(craudio_sine PRIVATE carrottus)
        if(UNIX AND NOT APPLE)
            target_link_libraries(craudio_sine PRIVATE m)
        endif()

        # CrNet against a real network stack. It needs the server in
        # tests/crnet_test_server.py running beside it (Python 3 and the
        # openssl command), which tests/run_crnet_test.sh sees to, so it
        # isn't registered with ctest either.
        add_executable(crnet_test tests/crnet_test.c)
        target_link_libraries(crnet_test PRIVATE carrottus)
        if(UNIX)
            find_package(Threads REQUIRED)
            target_link_libraries(crnet_test PRIVATE Threads::Threads)
        endif()

        # CrNet's parsers of the network's bytes, each fuzz target in
        # tests/fuzz run over its seed corpus and every input the fuzzer
        # once crashed on (tests/fuzz/regressions/<target>), with an
        # ordinary main instead of libFuzzer's: no network, so ctests, and
        # on every compiler. The targets reach CrNet's internal functions
        # (src/net/crnetshared.h, which includes eqstd) in the static
        # library.
        foreach(_fuzz IN LISTS CARROTTUS_FUZZ_TARGETS)
            add_executable(${_fuzz}_replay tests/fuzz/${_fuzz}.c tests/fuzz/crfuzz_replay.c)
            target_include_directories(${_fuzz}_replay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_link_libraries(${_fuzz}_replay PRIVATE carrottus eqstd)
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
    option(CARROTTUS_BUILD_FUZZERS "Build CrNet's libFuzzer targets (clang)" OFF)
    if(CARROTTUS_BUILD_FUZZERS)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "CARROTTUS_BUILD_FUZZERS needs clang with libFuzzer (-fsanitize=fuzzer)")
        endif()
        foreach(_fuzz IN LISTS CARROTTUS_FUZZ_TARGETS)
            add_executable(${_fuzz} tests/fuzz/${_fuzz}.c)
            target_include_directories(${_fuzz} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
            target_compile_options(${_fuzz} PRIVATE -fsanitize=fuzzer)
            target_link_options(${_fuzz} PRIVATE -fsanitize=fuzzer)
            target_link_libraries(${_fuzz} PRIVATE carrottus eqstd)
        endforeach()
    endif()
endif()
