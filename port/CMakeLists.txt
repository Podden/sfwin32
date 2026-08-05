cmake_minimum_required(VERSION 3.15)
project(fightport C)

# The game is 32-bit x86 machine code executed in-process, so the host must be
# a 32-bit process:  cmake -A Win32 ..
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "fightport must be configured for Win32 (x86): cmake -A Win32 ..")
endif()

set(SDL2_ROOT "D:/dev/libs/SDL2-2.0.12" CACHE PATH "SDL2 development package")

if(NOT EXISTS "${SDL2_ROOT}/include/SDL.h")
  message(FATAL_ERROR "SDL2 not found at ${SDL2_ROOT} -- set -DSDL2_ROOT=<path>")
endif()

add_executable(fightport
  src/main.c
  src/leimage.c
  src/emu.c
  src/services.c
  src/video.c
  src/audio.c
  src/mouse.c
  src/calltrace.c
  src/cblit.c
  src/glide.c
)

target_include_directories(fightport PRIVATE src "${SDL2_ROOT}/include")
target_link_libraries(fightport PRIVATE
  "${SDL2_ROOT}/lib/x86/SDL2.lib"
  "${SDL2_ROOT}/lib/x86/SDL2main.lib"
  opengl32
  winmm                    # timeBeginPeriod, for a usable IRQ0 tick rate
)

target_compile_definitions(fightport PRIVATE
  SDL_MAIN_HANDLED          # we provide our own main()
  _CRT_SECURE_NO_WARNINGS
  WIN32_LEAN_AND_MEAN
)

if(MSVC)
  target_compile_options(fightport PRIVATE /W3)
  # The game runs on this thread's stack, so give it plenty, and keep the
  # image out of the address range the loader maps the objects into.
  target_link_options(fightport PRIVATE /STACK:0x1000000 /DYNAMICBASE:NO
                      /MAP /MAPINFO:EXPORTS)
endif()

add_custom_command(TARGET fightport POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SDL2_ROOT}/lib/x86/SDL2.dll" "$<TARGET_FILE_DIR:fightport>")
