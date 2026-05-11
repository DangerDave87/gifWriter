cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(GifExporter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Nuke REQUIRED COMPONENTS DDImage)

add_library(gifWriter SHARED
  src/GifEncoder.cpp
  src/GifWriter.cpp
)

target_include_directories(gifWriter
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(gifWriter
  PRIVATE
    Nuke::DDImage
)

target_compile_definitions(gifWriter
  PRIVATE
    NOMINMAX
    _USE_MATH_DEFINES
    WIN32_LEAN_AND_MEAN
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
  target_compile_options(gifWriter PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
else()
  target_compile_options(gifWriter PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(gifWriter PROPERTIES
  OUTPUT_NAME "gifWriter"
  PREFIX ""
)

install(TARGETS gifWriter
  RUNTIME DESTINATION .
  LIBRARY DESTINATION .
)
