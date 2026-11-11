# iOS / iPadOS build of the recompiled game: the ps2-web-recomp runtime and the C++ generated from your own disc,
# with a small UIKit front end (ios_main.mm, touch.mm, webdav.mm). See portios/README.md for how to build it; portios/build.sh does it all.
# Nothing from the game is packed into the app: the disc image is read from the app's Documents folder at run time.
cmake_minimum_required(VERSION 3.20)
project(p3fes-ios C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)

set(PS2_ROOT "" CACHE PATH "ps2-web-recomp checkout")
set(PS2_GENERATED "" CACHE PATH "Folder with the C++ generated for the game (generated/p3fes-ios, see build.sh)")
set(P3FES_BUNDLE_ID "local.p3fes.recomp" CACHE STRING "Bundle identifier of the app")
if(NOT PS2_ROOT OR NOT PS2_GENERATED)
    message(FATAL_ERROR "set PS2_ROOT and PS2_GENERATED")
endif()

file(GLOB PS2_RUNTIME CONFIGURE_DEPENDS
    "${PS2_ROOT}/runtime/src/*.cpp" "${PS2_ROOT}/runtime/src/gs/*.cpp" "${PS2_ROOT}/runtime/src/iop/*.cpp" "${PS2_ROOT}/runtime/src/ipu/*.cpp")
# Save states (ios_main.mm). The runtime saves a state between two slices of the game, the one point where no translated call
# is on the host stack, and loads one only before the game starts. The iOS copy of system.cpp, made in the build folder like
# the gpu.cpp copy below, lets the app run a function at that point, so a state can be saved or loaded at any time.
set(PS2_SYSTEM_SRC "${PS2_ROOT}/runtime/src/system.cpp")
list(REMOVE_ITEM PS2_RUNTIME "${PS2_SYSTEM_SRC}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2_SYSTEM_SRC}")
file(READ "${PS2_SYSTEM_SRC}" PS2_SYSTEM_TEXT)
set(PS2_RUN_ANCHOR "\nint run() {\n")
set(PS2_LOOP_ANCHOR "    while (!g_halted) {\n        if (g_state_request) {\n")
string(FIND "${PS2_SYSTEM_TEXT}" "${PS2_RUN_ANCHOR}" PS2_RUN_AT)
string(FIND "${PS2_SYSTEM_TEXT}" "${PS2_LOOP_ANCHOR}" PS2_LOOP_AT)
if(PS2_RUN_AT EQUAL -1 OR PS2_LOOP_AT EQUAL -1)
    message(FATAL_ERROR "system.cpp changed: cannot find run() and its loop")
endif()
string(REPLACE "${PS2_RUN_ANCHOR}"
    "\nstd::atomic<bool> g_safe_point_due{false};   // portios: ios_main.mm saves or loads a state here\nvoid (*g_safe_point)() = nullptr;\n${PS2_RUN_ANCHOR}"
    PS2_SYSTEM_TEXT "${PS2_SYSTEM_TEXT}")
string(REPLACE "${PS2_LOOP_ANCHOR}"
    "    while (!g_halted) {\n        if (g_safe_point_due.load(std::memory_order_acquire)) {   // portios\n            g_safe_point_due.store(false);\n            if (g_safe_point) g_safe_point();\n        }\n        if (g_state_request) {\n"
    PS2_SYSTEM_TEXT "${PS2_SYSTEM_TEXT}")
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/system_ios.cpp" CONTENT "${PS2_SYSTEM_TEXT}" @ONLY)
list(APPEND PS2_RUNTIME "${CMAKE_BINARY_DIR}/system_ios.cpp")

add_library(ps2runtime STATIC ${PS2_RUNTIME})
target_include_directories(ps2runtime PUBLIC "${PS2_ROOT}/runtime/include" PRIVATE "${PS2_ROOT}/runtime/src")
target_compile_options(ps2runtime PRIVATE -Wno-unused-parameter)

# GPU drawing (much faster than the CPU renderer): unpack wgpu-native's iOS release for the target you build
# (wgpu-ios-aarch64-release.zip for a device, wgpu-ios-aarch64-simulator-release.zip for the Simulator, the same
# version as third_party/wgpu-native) and point P3FES_WGPU_DIR at it. Without it the CPU renderer draws.
set(P3FES_WGPU_DIR "" CACHE PATH "wgpu-native iOS release folder (include/ and lib/libwgpu_native.a)")
if(P3FES_WGPU_DIR AND EXISTS "${P3FES_WGPU_DIR}/lib/libwgpu_native.a")
    set(PS2_GS_WGSL "${CMAKE_BINARY_DIR}/gs_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_GS_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${PS2_ROOT}/runtime/src/gs/gpu/gs.wgsl -DOUT=${PS2_GS_WGSL} -DNAME=kGsShader -P ${PS2_ROOT}/cmake/embed.cmake
        DEPENDS ${PS2_ROOT}/runtime/src/gs/gpu/gs.wgsl ${PS2_ROOT}/cmake/embed.cmake)
    set(PS2_BLIT_WGSL "${CMAKE_BINARY_DIR}/blit_wgsl.cpp")
    add_custom_command(OUTPUT "${PS2_BLIT_WGSL}"
        COMMAND ${CMAKE_COMMAND} -DSRC=${PS2_ROOT}/runtime/src/gs/gpu/blit.wgsl -DOUT=${PS2_BLIT_WGSL} -DNAME=kBlitShader -P ${PS2_ROOT}/cmake/embed.cmake
        DEPENDS ${PS2_ROOT}/runtime/src/gs/gpu/blit.wgsl ${PS2_ROOT}/cmake/embed.cmake)
    # wgpu hands some iOS GPUs (the Simulator's at least) reduced default limits, e.g. 4 storage buffers per shader stage where
    # the GS shaders use 7, so the iOS build asks for everything the adapter supports. That is done on a copy of gpu.cpp in the
    # build folder, leaving the ps2-web-recomp tree as it is.
    set(PS2_GPU_SRC "${PS2_ROOT}/runtime/src/gs/gpu/gpu.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS2_GPU_SRC}")
    file(READ "${PS2_GPU_SRC}" PS2_GPU_TEXT)
    set(PS2_LIMITS_ANCHOR "    WGPULimits limits = WGPU_LIMITS_INIT;\n")
    string(FIND "${PS2_GPU_TEXT}" "${PS2_LIMITS_ANCHOR}" PS2_LIMITS_AT)
    if(PS2_LIMITS_AT EQUAL -1)
        message(FATAL_ERROR "gpu.cpp changed: cannot find where it sets the device limits")
    endif()
    string(REPLACE "${PS2_LIMITS_ANCHOR}" "${PS2_LIMITS_ANCHOR}    limits = supported;   // portios: all the adapter supports\n    limits.nextInChain = nullptr;\n"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    # The app hands the runtime a CAMetalLayer so frames go straight to the screen, with no copy back to the CPU. There is no
    # Metal kind in SurfaceTarget, so the copy takes kind Canvas (unused outside the browser) with the layer in `surface`.
    set(PS2_WAYLAND_ANCHOR "    WGPUSurfaceSourceWaylandSurface wayland = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;\n")
    set(PS2_SURFACE_ELSE "    } else {\n        return false;\n    }\n#endif\n    g_surface = wgpuInstanceCreateSurface")
    string(FIND "${PS2_GPU_TEXT}" "${PS2_WAYLAND_ANCHOR}" PS2_WAYLAND_AT)
    string(FIND "${PS2_GPU_TEXT}" "${PS2_SURFACE_ELSE}" PS2_SURFACE_ELSE_AT)
    if(PS2_WAYLAND_AT EQUAL -1 OR PS2_SURFACE_ELSE_AT EQUAL -1)
        message(FATAL_ERROR "gpu.cpp changed: cannot find where it makes the window surface")
    endif()
    string(REPLACE "${PS2_WAYLAND_ANCHOR}" "${PS2_WAYLAND_ANCHOR}    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;   // portios\n"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    string(REPLACE "${PS2_SURFACE_ELSE}" "    } else if (target.kind == SurfaceTarget::Canvas && target.surface) {   // portios: a CAMetalLayer\n        metal.layer = target.surface;\n        sd.nextInChain = &metal.chain;\n${PS2_SURFACE_ELSE}"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    # Widescreen (wide.cpp): draw() narrows flat drawing (menus, text, movies) toward the middle, and present() shows the picture
    # at the shape the app picked.
    set(PS2_GPU_NS_ANCHOR "\nnamespace ps2::gs::gpu {\n")
    set(PS2_DRAW_ANCHOR "\nvoid draw(const State &st, const Vertex *v, int count) {\n")
    set(PS2_PRESENT_ANCHOR "\nbool present(const DisplaySource &src) {\n    if (!g_surface) return false;\n")
    # Resolution: the app picks the scale (ios_main.mm, PS2_SCALE); 4x lets an iPad's picture be as tall as its screen.
    set(PS2_SCALE_ANCHOR "g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 3u);")
    set(PS2_SCISSOR_ANCHOR "    const int scax0 = static_cast<int>(setup[10] & 0xFFFFu), scax1 = static_cast<int>(setup[10] >> 16);\n")
    foreach(anchor PS2_GPU_NS_ANCHOR PS2_DRAW_ANCHOR PS2_PRESENT_ANCHOR PS2_SCISSOR_ANCHOR PS2_SCALE_ANCHOR)
        string(FIND "${PS2_GPU_TEXT}" "${${anchor}}" PS2_ANCHOR_AT)
        string(FIND "${PS2_GPU_TEXT}" "${${anchor}}" PS2_ANCHOR_LAST REVERSE)
        if(PS2_ANCHOR_AT EQUAL -1 OR NOT PS2_ANCHOR_AT EQUAL PS2_ANCHOR_LAST)
            message(FATAL_ERROR "gpu.cpp changed: cannot find the one place for ${anchor} (widescreen)")
        endif()
    endforeach()
    string(REPLACE "${PS2_GPU_NS_ANCHOR}"
        "\nfloat portios_aspect();   // portios: wide.cpp\nvoid portios_presented(std::uint32_t fbp);\nconst ps2::gs::Vertex *portios_widen(const ps2::gs::State &st, const ps2::gs::Vertex *v, int count, ps2::gs::Vertex *out);\nvoid portios_clip(int &x0, int &x1);\n${PS2_GPU_NS_ANCHOR}"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    string(REPLACE "${PS2_DRAW_ANCHOR}" "${PS2_DRAW_ANCHOR}    Vertex portios_v[3];   // portios: wide.cpp\n    v = ::portios_widen(st, v, count, portios_v);\n"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    string(REPLACE "${PS2_SCALE_ANCHOR}" "g_scale = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::atoi(scale)), 1u, 4u);   // portios: up to 4x"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    string(REPLACE "${PS2_SCISSOR_ANCHOR}"
        "    int scax0 = static_cast<int>(setup[10] & 0xFFFFu), scax1 = static_cast<int>(setup[10] >> 16);\n    ::portios_clip(scax0, scax1);   // portios: wide.cpp\n"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    string(REPLACE "${PS2_PRESENT_ANCHOR}" "${PS2_PRESENT_ANCHOR}    g_aspect = ::portios_aspect();   // portios: wide.cpp\n    ::portios_presented(src.fbp);\n"
        PS2_GPU_TEXT "${PS2_GPU_TEXT}")
    file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/gpu_ios.cpp" CONTENT "${PS2_GPU_TEXT}" @ONLY)
    target_sources(ps2runtime PRIVATE "${CMAKE_BINARY_DIR}/gpu_ios.cpp" "${PS2_GS_WGSL}" "${PS2_BLIT_WGSL}" "${CMAKE_CURRENT_SOURCE_DIR}/wide.cpp")
    target_include_directories(ps2runtime PRIVATE "${PS2_ROOT}/runtime/src/gs/gpu" "${PS2_ROOT}/runtime/src/gs")
    target_compile_definitions(ps2runtime PUBLIC PS2_GPU)
    target_include_directories(ps2runtime PRIVATE "${P3FES_WGPU_DIR}/include")
    target_link_libraries(ps2runtime PUBLIC "${P3FES_WGPU_DIR}/lib/libwgpu_native.a"
        "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreFoundation" "-framework IOSurface" objc)
    message(STATUS "drawing with the GPU (wgpu-native from ${P3FES_WGPU_DIR})")
else()
    message(STATUS "drawing with the CPU renderer (no P3FES_WGPU_DIR)")
endif()

file(GLOB PS2_UNITS CONFIGURE_DEPENDS "${PS2_GENERATED}/*.cpp")
add_library(ps2game STATIC ${PS2_UNITS})
target_include_directories(ps2game PRIVATE "${PS2_GENERATED}")
target_link_libraries(ps2game PUBLIC ps2runtime)
target_compile_options(ps2game PRIVATE -Wno-unused-label -Wno-unused-variable)
# Nothing 3DS-specific: generated/p3fes-ios has no hot copies, and PS2_NO_HOT keeps them out if another folder is used.
# -fno-math-errno lets sqrt and friends compile to single instructions (the game never reads errno).
target_compile_definitions(ps2game PRIVATE PS2_NO_HOT)
target_compile_options(ps2game PRIVATE -fno-math-errno)
target_compile_options(ps2runtime PRIVATE -fno-math-errno)

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(P3FES_PLATFORM "iPhoneSimulator")
else()
    set(P3FES_PLATFORM "iPhoneOS")
endif()

add_executable(p3fes MACOSX_BUNDLE ios_main.mm touch.mm webdav.mm)
target_link_libraries(p3fes PRIVATE ps2game ps2runtime
    "-framework UIKit" "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics"
    "-framework AVFAudio" "-framework GameController" "-framework Metal" "-framework Security" -Wl,-dead_strip)
target_compile_options(p3fes PRIVATE -fobjc-arc)
set_target_properties(p3fes PROPERTIES
    OUTPUT_NAME "P3FES"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${P3FES_BUNDLE_ID}"
    MACOSX_BUNDLE_BUNDLE_NAME "P3FES"
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${P3FES_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
