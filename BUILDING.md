# Building

The game runtime and the launcher are built on Windows with the MSYS2 CLANG64 toolchain. Everything else
the code needs is in this repository (`gpu/third_party/`, `third_party/`).

## Toolchain

Install MSYS2 and, from the CLANG64 shell, the build packages once:

    pacman -S --needed mingw-w64-clang-x86_64-{clang,cmake,ninja,pkgconf,vulkan-headers,vulkan-loader,\
    vulkan-memory-allocator,fmt,boost,tsl-robin-map,xxhash,sdl3,zydis,ffmpeg,protobuf,nlohmann-json,\
    openssl,zlib,miniupnpc}

## Game runtime

    bash build.sh

This produces `out/DriveClubGame.exe`.

## Release

    bash packaging/package.sh 1.0

This builds the runtime and the launcher, copies the runtime libraries next to them and writes
`release/driveclub-recomp-1.0-win64.zip`. The zip needs nothing installed on the player's machine.
