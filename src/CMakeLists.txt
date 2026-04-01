cmake_minimum_required(VERSION 3.20.0 FATAL_ERROR)

project(kompas_mcp_bridge C CXX)

message(STATUS "Output dir: ${OUTPUT_DIRECTORY}")

# Build type
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release MinSizeRel RelWithDebInfo)

# Platform
set(PLATFORM "x64" CACHE STRING "Build platform")

# Include compile options from the KOMPAS SDK pattern
include(${CMAKE_SOURCE_DIR}/CMake/CompileOptions.cmake)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  ${OUTPUT_DIRECTORY})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY  ${OUTPUT_DIRECTORY})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY  ${OUTPUT_DIRECTORY})

# KOMPAS SDK paths (Release mode - using SDK)
set(KOMPAS_INCLUDE_DIRECTORIES
  $ENV{KOMPAS_SDK}/KsAPI/Include
)

set(KOMPAS_LINK_DIRECTORIES
  $ENV{KOMPAS_SDK}/KsAPI/Include
  ${KOMPAS_OUTPUT_DIR}
)

message(STATUS "Kompas API include dir: ${KOMPAS_INCLUDE_DIRECTORIES}")
message(STATUS "Kompas output dir: ${KOMPAS_OUTPUT_DIR}")

set(COMMON_INCLUDE_DIRECTORIES
  "."
  ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

set(COMMON_LINK_DIRECTORIES
  "."
  ${OUTPUT_DIRECTORY}
  ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)

# Target
set(this_target "kompas_mcp_bridge")
set(this_target_name "kompas_mcp_bridge")

set(${this_target}_SOURCE_FILES
  bridge.cpp
)

add_library(${this_target} SHARED
  ${${this_target}_SOURCE_FILES}
)

set_target_properties(${this_target} PROPERTIES
  OUTPUT_NAME ${this_target_name}
  SUFFIX ".rtw"
)

target_include_directories(${this_target} PRIVATE
  ${KOMPAS_INCLUDE_DIRECTORIES}
  ${COMMON_INCLUDE_DIRECTORIES}
)

target_link_directories(${this_target} PRIVATE
  ${KOMPAS_LINK_DIRECTORIES}
  ${COMMON_LINK_DIRECTORIES}
)

target_compile_definitions(${this_target} PRIVATE
  ${UNICODE_MACROS}
)

# Qt5 for QTimer (main-thread scheduling)
find_package(Qt5 COMPONENTS Core QUIET)
if(Qt5_FOUND)
  target_link_libraries(${this_target} PRIVATE Qt5::Core)
  message(STATUS "Qt5 found, enabling QTimer support")
else()
  message(STATUS "Qt5 not found, QTimer disabled")
endif()

# dl for dlsym
target_link_libraries(${this_target} PRIVATE dl)

target_compile_features(${this_target} PRIVATE
  ${CXX_STD_VERSION}
)

target_compile_options(${this_target} PRIVATE
  ${COMMON_COMPILE_OPTIONS}
)

target_link_options(${this_target} PRIVATE
  ${ADDITIONAL_CXX_LINKER_FLAGS}
)
