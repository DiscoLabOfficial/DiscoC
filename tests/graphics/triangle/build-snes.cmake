cmake_minimum_required(VERSION 3.15)
# Retain the old regression entry point without a second implementation.
if(NOT DEFINED OUTPUT_DIR)
    get_filename_component(OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../build/plot-triangle" ABSOLUTE)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../../examples/snes/triangle/build-snes.cmake")
