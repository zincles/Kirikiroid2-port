Kirikiroid2 - A cross-platform port of Kirikiri2/KirikiriZ
==========================================================

Based on most code from [Kirikiri2](http://kikyou.info/tvp/) and [KirikiriZ](https://github.com/krkrz/krkrz)

Video playback module modified from [kodi](https://github.com/xbmc/xbmc)

Some string code from [glibc](https://www.gnu.org/s/libc) and [Apple Libc](https://opensource.apple.com/source/Libc).

Real-time texture codec modified from [etcpak](https://bitbucket.org/wolfpld/etcpak.git), [pvrtccompressor](https://bitbucket.org/jthlim/pvrtccompressor), [astcrt](https://github.com/daoo/astcrt)

Android storage accessing code from [AmazeFileManager](https://github.com/arpitkh96/AmazeFileManager)

Building the Linux and Nintendo Switch ports
--------------------------------------------

The Android project in `project/android` is unchanged.  The ports build from the
root `CMakeLists.txt` as a static engine core plus plugins; there is no cocos2d-x
and no vendor tree to fetch.

### Linux (x86_64)

Toolchain: GCC or Clang with C++17, CMake >= 3.16, Ninja, pkg-config and the SDL2
development package.  On Arch Linux:

    sudo pacman -S --needed base-devel cmake ninja pkgconf git sdl2 freetype2 \
        libpng libjpeg-turbo libwebp zlib bzip2 liblz4 oniguruma openal libogg \
        libvorbis opusfile ffmpeg libarchive tinyxml2 xxhash libglvnd

(`ffmpeg` is only needed for video playback; everything else is required.)

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    ninja -C build

If the distribution's `sdl2` is the **sdl2-compat** reimplementation (Arch's
`sdl2` package, `pkg-config --modversion sdl2` reporting 2.32.x, is one), CMake
says so at configure time, because that library is known to crash inside
`SDL_ShowMessageBox`.  Everything the engine does works around it -- diagnostics
go to the console -- but for real message boxes build SDL2 2.x and point the
build at it with `-DKRKR2_SDL2_PREFIX`, which appends the prefix to the search
path and sets the runtime path:

    curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz
    tar xf SDL2-2.30.11.tar.gz
    cmake -S SDL2-2.30.11 -B SDL2-2.30.11/build -G Ninja \
        -DCMAKE_INSTALL_PREFIX=$HOME/.local/sdl2 -DSDL_SHARED=ON -DSDL_STATIC=OFF
    ninja -C SDL2-2.30.11/build install
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DKRKR2_SDL2_PREFIX=$HOME/.local/sdl2
    ninja -C build

Run a game (a directory holding `startup.tjs`, an `.xp3`, or a `.7z`):

    ./build/krkr2 /path/to/game

With no game argument, `krkr2` starts one found next to the executable (also in a
`games/` sub-directory, preferring `data.xp3`); when there is none it opens a
**game browser** - the same dialog the engine's `Storages.selectFile()` reaches:
`..` is the first row (when there is a parent), directories follow and are listed
before files, the `*.xp3;*.7z` filter covers archives, `ENTER` activates the row
under the cursor (entering a directory, opening the `..` row - i.e. going up - or
picking an archive), `F2` (game pad `X`) takes the folder the cursor is in - which
is how a game that is a directory is chosen - and `ESC` cancels.  The keyboard, the
mouse and a game pad all drive it, and the roots list (`F1`, game pad `BACK`)
reaches the mounted volumes, so on a handheld with no command line the whole flow
is game pad only.  Only something the engine can actually start is accepted - an
archive containing `startup.tjs`, or a folder holding `startup.tjs` or such an
archive; anything else is refused with the reason, the cursor is left on it and the
browser comes back, so picking the next candidate is one step (an unattended run
gives up after 8 refusals instead of asking forever).  The game started last is
remembered and the next browsing run opens there.  `--browse` asks even when a game
was given (it opens at that game) or found.

An encrypted `.xp3` needs its patch files (`patch.tjs`, `patch.xp3` or
`xp3filter.tjs`) beside it, and `krkr2` exits with status 3 and says so if the
archive cannot be decrypted.  A directory without `startup.tjs` is reported the
same way instead of starting an empty window.

The log is the terminal, so it also shows what the engine prints for itself.  One
line of it looks like a crash but is not: when an engine error is raised inside
running script code - reading past the end of a string, a missing member, a bad
argument - TJS2 prints `==== An exception occured at ...` with the generated code
and a register dump *at the moment it is thrown*, before the script's own
`try`/`catch` can run.  Windows had no console for that output; here it is visible,
and it appears even when the script handles the error.  A game that is really
failing says so in its own words after that, and the exit status is non-zero.

#### Running it on another machine

A binary built here runs only on a machine whose libraries are at least as new as
the ones it was built against.  Measured on the pair this was developed on (a
container with glibc 2.43, run on a Debian 13 host with glibc 2.41):

| what the binary asks for | what the host has |
|---|---|
| `GLIBC_2.43` (symbol versions in libc/libm) | glibc 2.41 - **cannot be fixed by copying files** |
| `GLIBCXX_3.4.36` (libstdc++) | 3.4.33 |
| `libjpeg.so.8` | `libjpeg.so.62` |
| `libavcodec.so.63` | `libavcodec.so.61` |

The first two are the runtime generation of the *machine*, and no packaging scheme
changes them: the C library has to stay dynamic (this port `dlopen()`s the GL/EGL
driver, the audio backends and fontconfig, which a static libc breaks), so "link
everything statically" is not an option and would not lift this floor anyway.  The
lower two are shared-object names, and those *can* be shipped alongside the
executable.

So, in order of preference:

1. **Build on (or for) the machine that runs it** - the only thing that removes
   every axis at once, and what the instructions above describe.  For Debian 13 /
   Ubuntu 24.04 and later:

       sudo apt install build-essential cmake ninja-build pkgconf git \
           libsdl2-dev libfreetype-dev libpng-dev libjpeg-dev libturbojpeg0-dev \
           zlib1g-dev libbz2-dev liblz4-dev libonig-dev libopenal-dev libogg-dev \
           libvorbis-dev libopusfile-dev libavcodec-dev libavformat-dev \
           libavutil-dev libswscale-dev libswresample-dev libarchive-dev \
           libtinyxml2-dev libwebp-dev libxxhash-dev libglvnd-dev

   (Debian and Ubuntu carry the ffmpeg 7 series, which this port builds against.)
   To support older machines as well, do it in a container of the oldest system you
   care about: the build machine's glibc is the lowest the binary will start on.

2. **Ship the shared-object names with it** - for machines of the same or a newer
   generation, which is the usual case for a copy between distributions released
   around the same time:

       cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKRKR2_BUNDLE=ON
       ninja -C build
       scripts/make-bundle.sh --build build --out krkr2-bundle

   That links the C++ runtime in, puts `$ORIGIN/lib` in the runpath, and copies
   every library that a desktop machine does not provide itself (a few hundred
   megabytes, ffmpeg being most of it) next to the executable - copy the directory
   and run `./krkr2` in it.  The script needs `patchelf`, checks that no dependency
   was missed, and prints the glibc version the result needs; it cannot lift that
   floor.

### Nintendo Switch

Toolchain: [devkitPro](https://devkitpro.org/) with devkitA64 and the switch
portlibs the engine links against:

    sudo pacman -S --needed switch-dev switch-cmake switch-sdl2 switch-mesa \
        switch-ffmpeg switch-freetype switch-libpng switch-libjpeg-turbo \
        switch-libwebp switch-zlib switch-bzip2 switch-liblzma switch-lz4 \
        switch-libzstd switch-oniguruma switch-openal-soft switch-libogg \
        switch-libvorbis switch-opusfile switch-libarchive switch-tinyxml2 \
        switch-xxhash switch-pkg-config switch-tools

    cmake -S . -B build-switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-switch.cmake -DDEVKITPRO=/opt/devkitpro
    ninja -C build-switch

The homebrew bundle is `build-switch/krkr2.nro` (about 26 MiB).  Put it in
`sdmc:/switch/krkr2/`; run it from hbmenu either with the game in the same
directory (it is picked up automatically) or with the game path as an argument.

### Tests

    tests/run-all.sh --binary ./build/krkr2     # add --with-switch to also build the .nro

Runs everything headless in about half a minute: boot and scripting, directory /
`.xp3` / encrypted `.xp3` / `.7z` storage, the modal file selector, the API
conformance suite, archive streaming, the `layerExMovie` extension (movie pixels
in a layer, callbacks, looping), touch and game-pad input (replayed by
`KRKR2_TEST_INPUT`, see `src/core/environ/sdl/Host.h`), and the two regressions
that only show up in optimised builds.  Compare a `Debug` build with a
`RelWithDebInfo` one -- both matter, because optimised builds have caught crashes
that `Debug` hid.
