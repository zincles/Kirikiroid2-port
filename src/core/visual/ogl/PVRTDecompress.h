/******************************************************************************
 
 @File         PVRTDecompress.h
 
 @Title
 
 @Copyright    Copyright (C) 2000 - 2008 by Imagination Technologies Limited.
 
 @Platform     ANSI compatible
 
 @Description  PVRTC Texture Decompression.
 
 ******************************************************************************/

/* Vendored for this port (Kirikiroid2, Linux/SDL2, no cocos2d-x):
   source cocos2d-x 3.6 cocos/base/pvr.h, MIT licensed,
   https://raw.githubusercontent.com/cocos2d/cocos2d-x/cocos2d-x-3.6/cocos/base/pvr.h
   Kirikiroid2's Android build reached this declaration through its
   "base/pvr.h" include (-I vendor/cocos2d-x/current/cocos); with the cocos
   vendor tree gone the declaration and its implementation had to be brought
   back into the tree. Only the include guard (upstream used the reserved
   __PVR_H__, unrelated to this port's visual/ogl/pvr.h) and the include of
   the implementation header were adapted; the declaration is untouched and
   matches the call sites in src/core/visual/LoadPVRv3.cpp. */

#ifndef TVP_PVRT_DECOMPRESS_H
#define TVP_PVRT_DECOMPRESS_H


int PVRTDecompressPVRTC(const void * const pCompressedData,const int XDim,const int YDim,void *pDestData,const bool Do2bitMode);



#endif // TVP_PVRT_DECOMPRESS_H
