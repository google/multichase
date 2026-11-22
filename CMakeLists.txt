# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.1...3.29)

project(
  multichase
  VERSION 1.0
  LANGUAGES C)

add_custom_command(
  OUTPUT expand.h
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/gen_expand 200 > expand.h
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_expand
  VERBATIM
)

add_library(common
  br_asm.c
  permutation.c
  arena.c
  util.c
  cpu_util.h
  timer.h
  permutation.h
  util.h
  arena.h
  expand.h
)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(multichase multichase.c)
target_link_libraries(multichase PRIVATE common)
target_link_libraries(multichase PRIVATE m)

add_executable(multiload multiload.c)
target_link_libraries(multiload PRIVATE common)
target_link_libraries(multiload PRIVATE m)

add_executable(fairness multiload.c)
target_link_libraries(fairness PRIVATE common)
target_link_libraries(fairness PRIVATE m)

add_executable(pingpong multiload.c)
target_link_libraries(pingpong PRIVATE common)
target_link_libraries(pingpong PRIVATE m)
