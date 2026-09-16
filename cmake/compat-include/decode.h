/* Kirikiroid2 Linux/SDL2 port: include-path shim.
   The engine sources use the flat libwebp spelling ("decode.h"),
   which is how the Android vendor tree lays the headers out. */
#pragma once
#include <webp/decode.h>
