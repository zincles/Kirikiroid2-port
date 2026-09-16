/*

	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000-2007 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"


*/
/* TVPGL ARM/NEON route table */

/*
	This header intentionally declares and defines nothing.

	Upstream it is generated together with tvpgl_arm.cpp and holds the NEON
	route of the TVPGL function table: the definitions of the global "TVPxxx"
	function pointers together with the table that maps each pointer to its NEON
	variant after TVPGL_ASM_Init() has checked that the CPU actually has NEON.

	In this port there is no generated NEON route: the pointers are defined by
	the TVP_GL_FUNC_PTR_DECL block in tvpgl.cpp and tvpgl_arm.cpp registers the
	NEON implementations itself through the REGISTER_TVPGL_* macros (each of
	which expands to "TVPxxx = TVPxxx_NEON;"), so there is nothing left for
	this header to declare. Keep the generated table here if a separate NEON
	route is reintroduced.

	Note: this header is included from inside an "extern \"C\"" block, so it
	must stay C-compatible.
*/

#ifndef TVPGL_ARM_ROUTE_H
#define TVPGL_ARM_ROUTE_H

#endif
