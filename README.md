# bin2iso

A C++ tool that converts BIN/CUE CD images into ISO files. It ships with both a
command-line program and a small Win32 GUI (modelled on WinBin2Iso: a Source
field, a Target field, and a Convert button with a progress bar).

## What it does

A `.bin` file is a raw dump of a CD's sectors. The companion `.cue` sheet
describes the track layout. An ISO image, by contrast, stores only the 2048-byte
user-data payload of each data sector. Converting therefore means reading each
raw sector of the first data track and writing out just its user data:

| Track mode   | Raw sector | User-data offset | User-data length |
|--------------|-----------:|-----------------:|-----------------:|
| MODE1/2048   |       2048 |                0 |             2048 |
| MODE1/2352   |       2352 |               16 |             2048 |
| MODE2/2048   |       2048 |                0 |             2048 |
| MODE2/2336   |       2336 |                8 |             2048 |
| MODE2/2352   |       2352 |               24 |             2048 |

Audio tracks and MODE2 Form 2 sectors cannot be represented inside an ISO, so
they are reported as warnings and skipped.

## Project layout

```
src/cue_parser.{h,cpp}   CUE sheet parser (tracks, modes, MSF timestamps)
src/converter.{h,cpp}    Sector-stripping conversion engine
src/main_cli.cpp         Command-line front-end
src/main_gui.cpp         Win32 GUI front-end (Windows only)
```

## Building

Requires CMake 3.15+ and a C++17 compiler (MSVC, MinGW, GCC, or Clang).

```
cmake -S . -B build
cmake --build build --config Release
```

Outputs:
- `bin2iso` (CLI) on every platform
- `bin2iso_gui` (GUI) on Windows

## Command-line usage

```
bin2iso <input.cue> [output.iso] [--bin <path>] [-q]
```

Examples:

```
bin2iso game.cue
bin2iso game.cue out.iso
bin2iso game.cue --bin D:\dumps\game.bin
```

If `output.iso` is omitted it defaults to the cue file name with an `.iso`
extension. `--bin` overrides the BIN path named inside the cue sheet.
