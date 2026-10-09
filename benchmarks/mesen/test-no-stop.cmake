cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/profile.cmake")
mesen_measure("${OUTPUT_DIR}" 3 2)
message(FATAL_ERROR "A program without STOP unexpectedly produced a measurement")
