/*

	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2007 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"


*/
/* TVPGL route table */

/*
	This header intentionally defines and declares nothing.

	Upstream this file is generated (gengl.pl) together with tvpgl.cpp: it is
	the SIMD route of the TVPGL function table, i.e. it defines the global
	"TVPxxx" function pointers and _Initialize_Route_Ptr(), which picks an SSE2
	/ AVX2 / NEON variant of every operation at start-up according to the CPU
	features reported by TVPGetCPUFeatures().

	On this platform there is no such generated route yet. The function
	pointers are defined by the TVP_GL_FUNC_PTR_DECL block inside tvpgl.cpp
	itself, and TVPInitTVPGL() assigns each of them to its scalar "_c"
	implementation unconditionally (the hardcoded "#if 1" block there);
	_Initialize_Route_Ptr() is not built. Consequently the whole TVPGL surface
	resolves to the C implementations and there is nothing for this header to
	route - keeping it non-empty would only duplicate the definitions that
	tvpgl.cpp already provides.

	It still exists because generated tvpgl.cpp includes it; when a real SIMD
	route is added, the generated table and _Initialize_Route_Ptr() belong
	here, and TVPInitTVPGL() must call _Initialize_Route_Ptr() again.
*/

#ifndef TVPGL_ROUTE_H
#define TVPGL_ROUTE_H

#endif
