cmake_minimum_required(VERSION 3.15)
project(vehtest C)

# The payload is 32-bit x86 machine code and the object bases live below 4 GB,
# so this only makes sense as a Win32 build:  cmake -A Win32 ..
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "vehtest must be configured for Win32 (x86): cmake -A Win32 ..")
endif()

add_executable(vehtest veh_test.c)

if(MSVC)
  target_compile_options(vehtest PRIVATE /W3)
  target_compile_definitions(vehtest PRIVATE _CRT_SECURE_NO_WARNINGS)
  # Fixed base, no ASLR on the image itself, so nothing drifts into the
  # 0x10000 / 0x140000 windows we need.
  target_link_options(vehtest PRIVATE /DYNAMICBASE:NO /BASE:0x400000)
endif()
