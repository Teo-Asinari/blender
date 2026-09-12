# Blender-owned, host-neutral painting and sculpting backend. This directory
# also builds independently for fast backend tests without a Blender executable.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.22)
  project(blender_golemics_backend LANGUAGES CXX)
  include(CTest)
endif()

add_library(bf_golemics_backend STATIC
  src/gpu_paint_session.cpp
  src/sculpt_session.cpp
  src/voxel_sculpt.cpp
  src/voxel_sculpt_c_api.cpp
  src/voxel_sculpt_host.cpp
)
add_library(bf::golemics_backend ALIAS bf_golemics_backend)
target_include_directories(bf_golemics_backend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bf_golemics_backend PUBLIC cxx_std_20)
set_target_properties(bf_golemics_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The imported backend uses exceptions internally. They are contained at its
# C host boundary; Blender's other modules keep their own compiler policy.
if(MSVC)
  target_compile_options(bf_golemics_backend PRIVATE /EHsc)
else()
  target_compile_options(bf_golemics_backend PRIVATE -fexceptions)
endif()

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(INC
    host
    ../gpu
    ../gpu/intern
    ../blenlib
  )
  set(SRC host/gpu_paint_blender.cc host/gpu_paint_blender.hh)
  set(LIB
    PRIVATE bf::golemics_backend
    PRIVATE bf::gpu
    PRIVATE bf::blenlib
    PRIVATE bf::intern::guardedalloc
  )
  blender_add_lib(bf_golemics_host "${SRC}" "${INC}" "" "${LIB}")
  add_library(bf::golemics_host ALIAS bf_golemics_host)
  target_include_directories(bf_golemics_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/host)
  if(MSVC)
    target_compile_options(bf_golemics_host PRIVATE /EHsc)
  else()
    target_compile_options(bf_golemics_host PRIVATE -fexceptions)
  endif()
endif()

option(WITH_GOLEMICS_BACKEND_TESTS "Build standalone Golemics backend tests" OFF)
if(BUILD_TESTING OR WITH_GOLEMICS_BACKEND_TESTS)
  enable_testing()
  foreach(test_name voxel_sculpt voxel_sculpt_host gpu_paint_session)
    add_executable(golemics_${test_name}_test tests/test_${test_name}.cpp)
    target_link_libraries(golemics_${test_name}_test PRIVATE bf::golemics_backend)
    if(MSVC)
      target_compile_options(golemics_${test_name}_test PRIVATE /UNDEBUG /EHsc)
    else()
      target_compile_options(golemics_${test_name}_test PRIVATE -UNDEBUG -fexceptions)
    endif()
    add_test(NAME golemics.${test_name} COMMAND golemics_${test_name}_test)
  endforeach()
endif()
