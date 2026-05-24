# components/nethack -- consumes the libnh.a + hacklib.a + lua548.a archives
# produced by `make esp32s3` at the T-Deck-NetHack repository root.
#
# Project layout:
#   T-Deck-NetHack/
#     targets/esp32s3/*.a       <- artifacts (this component imports them)
#     lilygo/                   <- the ESP-IDF project (this file's parent x3)
#       components/nethack/
#
# The archives are imported as IMPORTED STATIC libraries.  We expose this
# component to the rest of the project so main/ can link against the full
# triple via `REQUIRES nethack` in main/CMakeLists.txt.

idf_component_register(
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf"
)

set(NH_ARTIFACTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../targets/esp32s3"
    CACHE PATH "Path to libnh.a + hacklib.a + lua548.a")

if(NOT EXISTS "${NH_ARTIFACTS_DIR}/libnh.a")
    message(FATAL_ERROR
        "libnh.a not found at ${NH_ARTIFACTS_DIR}.\n"
        "Build it first from the repo root:  make esp32s3\n"
        "Or override the location:  -DNH_ARTIFACTS_DIR=/path/to/targets/esp32s3")
endif()

# Each archive must be a separate IMPORTED target so CMake re-runs the link
# step when any of them changes.
add_library(libnh STATIC IMPORTED GLOBAL)
set_target_properties(libnh PROPERTIES IMPORTED_LOCATION "${NH_ARTIFACTS_DIR}/libnh.a")

add_library(libnh_hacklib STATIC IMPORTED GLOBAL)
set_target_properties(libnh_hacklib PROPERTIES IMPORTED_LOCATION "${NH_ARTIFACTS_DIR}/hacklib.a")

add_library(libnh_lua STATIC IMPORTED GLOBAL)
set_target_properties(libnh_lua PROPERTIES IMPORTED_LOCATION "${NH_ARTIFACTS_DIR}/lua548.a")

# Bind the IMPORTED targets to this IDF component so anyone REQUIRESing
# nethack picks them up transitively.  --start-group / --end-group is needed
# because libnh.a and hacklib.a have mutual references (the engine calls
# helpers in hacklib.a; some hacklib helpers call back into the engine via
# function pointers stored in static state).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    -Wl,--start-group
    libnh
    libnh_hacklib
    libnh_lua
    -Wl,--end-group
    # esp_rom is searched before libnh.a in ESP-IDF's link order, so the
    # linker doesn't yet know it needs __wrap_longjmp when scanning
    # libesp_rom.a.  Force the wrapper symbol to be undefined up front so
    # esp_rom_longjmp.o gets pulled in on the first pass.
    "-u __wrap_longjmp"
)
