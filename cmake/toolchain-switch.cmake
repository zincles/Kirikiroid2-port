# CMake toolchain file for the Nintendo Switch (devkitPro / devkitA64 + libnx).
#
#   DEVKITPRO=/opt/devkitpro cmake -S . -B build-switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-switch.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   DEVKITPRO=/opt/devkitpro ninja -C build-switch
#
# DEVKITPRO must be exported for BOTH steps: devkitPro's link rules expand
# $DEVKITPRO inside the compiler's -specs argument at build time, so a configure
# that sees the variable is not enough.  The build produces build-switch/krkr2.nro
# via elf2nro (switch-tools).
#
# Requires the devkitPro pacman packages:
#   switch-dev switch-cmake switch-sdl2 switch-freetype switch-libpng
#   switch-libjpeg-turbo switch-libwebp switch-zlib switch-bzip2
#   switch-libarchive switch-lz4 switch-oniguruma switch-libvorbis
#   switch-libopus switch-opusfile switch-openal-soft switch-tinyxml2
#   switch-xxhash
#
# devkitA64's GCC is the same generation as the host compiler used for the Linux
# build, so the same language-level assumptions hold.

if(DEFINED ENV{DEVKITPRO})
	set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro installation prefix")
else()
	set(DEVKITPRO "/opt/devkitpro" CACHE PATH "devkitPro installation prefix")
endif()

include("${DEVKITPRO}/cmake/Switch.cmake" OPTIONAL RESULT_VARIABLE _devkit_switch_cmake)
if(NOT _devkit_switch_cmake)
	message(FATAL_ERROR "devkitPro's ${DEVKITPRO}/cmake/Switch.cmake was not found; "
		"install switch-dev/switch-cmake")
endif()

set(KRKR2_TARGET_SWITCH ON CACHE BOOL "Building for the Nintendo Switch" FORCE)

# devkitPro ships pkg-config files for every dependency this port uses (bzip2
# has none; CMakeLists.txt falls back to the plain library for it).
set(ENV{PKG_CONFIG_PATH} "${DEVKITPRO}/portlibs/switch/lib/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR} "${DEVKITPRO}/portlibs/switch/lib/pkgconfig")

# An .nro is a single self-contained binary: link everything statically.
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(BUILD_SHARED_LIBS OFF)
