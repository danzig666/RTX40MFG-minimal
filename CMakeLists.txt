cmake_minimum_required(VERSION 3.20)
project(RTX40MFG LANGUAGES C CXX)

# Only the Streamline SDK's public headers are needed -- no NGX SDK, no
# ReShade, no Dear ImGui.
set(STREAMLINE_ROOT "" CACHE PATH
    "Path to the NVIDIA Streamline SDK directory")
if(NOT EXISTS "${STREAMLINE_ROOT}/include/sl_dlss_g.h")
    message(FATAL_ERROR
        "Set STREAMLINE_ROOT to a Streamline SDK directory containing include/sl_dlss_g.h")
endif()

# MinHook replaces the upstream project's bespoke entry-detour framework.
add_library(MfgMinHook STATIC
    src/third_party/minhook/src/buffer.c
    src/third_party/minhook/src/hook.c
    src/third_party/minhook/src/trampoline.c
    src/third_party/minhook/src/hde/hde64.c
)
target_include_directories(MfgMinHook
    PUBLIC  src/third_party/minhook/include
    PRIVATE src/third_party/minhook/src
            src/third_party/minhook/src/hde
)
target_compile_definitions(MfgMinHook PRIVATE WIN32_LEAN_AND_MEAN)

add_library(RTX40MFG SHARED
    src/loader.cpp
    src/config.cpp
    src/log.cpp
    src/patches.cpp
    src/gates.cpp
    src/streamline.cpp
    src/ngx.cpp
    src/ada_patch.cpp
    src/provider_policy.cpp
)
target_compile_features(RTX40MFG PRIVATE cxx_std_20)
target_compile_definitions(RTX40MFG PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(RTX40MFG PRIVATE "${STREAMLINE_ROOT}/include")
target_link_libraries(RTX40MFG PRIVATE MfgMinHook bcrypt version psapi)

# Static CRT: the .asi is dropped next to a game executable and must not
# depend on the VC++ redistributable being installed.
set_property(TARGET RTX40MFG MfgMinHook PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ultimate ASI Loader loads *.asi from the game directory.
set_target_properties(RTX40MFG PROPERTIES
    PREFIX ""
    SUFFIX ".asi"
    OUTPUT_NAME "RTX40MFG"
)
