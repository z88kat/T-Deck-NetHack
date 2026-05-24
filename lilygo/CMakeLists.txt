# lilygo -- NetHack 5.0 for the LilyGo T-Deck (ESP32-S3).
#
# Lives inside the T-Deck-NetHack repository.  Consumes libnh.a built by
# `make esp32s3` at the repo root, and the nhdat data bundle built by
# the host stage (../dat/nhdat).
#
# Layout:
#   T-Deck-NetHack/
#     dat/nhdat              <- DLB bundle built by `make WANT_LIBNH=1 all`
#     targets/esp32s3/*.a    <- libnh.a etc. built by `make esp32s3`
#     lilygo/                <- this directory (the ESP-IDF project)
#
# Build:
#   cd ..
#   make WANT_LIBNH=1 all      # builds host tools + nhdat + native libnh.a
#   make esp32s3               # builds Xtensa libnh.a + companions
#   cd lilygo
#   . $IDF_PATH/export.sh
#   idf.py -p /dev/cu.usbmodem* flash monitor

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(tdeck-nethack)

# Stage data/nhdat from the repo's dat/nhdat so we don't duplicate the
# 1.5 MB DLB bundle in git.  Runs at configure-time and rebuilds if the
# source changes.
set(NH_DATA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../dat/nhdat")
set(NH_DATA_STAGE "${CMAKE_CURRENT_SOURCE_DIR}/data")
if(NOT EXISTS "${NH_DATA_SRC}")
    message(FATAL_ERROR
        "${NH_DATA_SRC} not found.  Run `make WANT_LIBNH=1 all` from the "
        "repo root first to build the nhdat bundle.")
endif()
file(MAKE_DIRECTORY "${NH_DATA_STAGE}")
configure_file("${NH_DATA_SRC}" "${NH_DATA_STAGE}/nhdat" COPYONLY)

# Build a SPIFFS image from data/ (just nhdat) and flash it to the
# `nhdat` partition.  app_main.c mounts it at /nethack which matches
# HACKDIR baked into libnh.a.
spiffs_create_partition_image(nhdat data FLASH_IN_PROJECT)
