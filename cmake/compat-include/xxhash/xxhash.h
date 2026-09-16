/* Kirikiroid2 Linux/SDL2 port: include-path shim.
   The engine sources use the cocos2d-x vendored spelling ("xxhash/xxhash.h");
   the system xxHash (same upstream project/API) installs flat. */
#pragma once
#include <xxhash.h>
