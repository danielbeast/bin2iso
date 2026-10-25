cmake_minimum_required(VERSION 3.15)
project(bin2iso LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Core conversion library (platform independent)
add_library(bin2iso_core STATIC
    src/cue_parser.cpp
    src/converter.cpp
)
target_include_directories(bin2iso_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Command line front-end
add_executable(bin2iso src/main_cli.cpp)
target_link_libraries(bin2iso PRIVATE bin2iso_core)

# Optional Win32 GUI front-end (Windows only)
if(WIN32)
    # The VS generator compiles src/app.rc with rc.exe automatically (icon +
    # Common-Controls v6 manifest + version info).
    add_executable(bin2iso_gui WIN32 src/main_gui.cpp src/app.rc)
    target_link_libraries(bin2iso_gui PRIVATE bin2iso_core comctl32 comdlg32)
    # Our .rc embeds the process manifest (id 1). Turn off the linker's own
    # auto-generated manifest so the two do not collide (CVT1100 duplicate).
    target_link_options(bin2iso_gui PRIVATE /MANIFEST:NO)
endif()
