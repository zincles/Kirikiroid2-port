//---------------------------------------------------------------------------
/*
	TVP2 ( T Visual Presenter 2 )  A script authoring tool
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Executable image protection check
//---------------------------------------------------------------------------
#ifndef ProtectH
#define ProtectH

//---------------------------------------------------------------------------
// TVPProtectInit
//---------------------------------------------------------------------------
/*!
	@brief	decides whether this process is allowed to start at all.

	TVPSystemInit() calls this first and returns immediately (i.e. leaves the
	script engine, TVPGL and the base systems uninitialized) when it yields
	false, so "false" means "this executable must not run". Upstream the check
	verifies the loaded image of the executable against a checksum stored in
	the protection data of the shipped build; a mismatch (patched image, no
	protection data) makes it fail.

	This port has no protection scheme and ships no protection data: there is
	nothing to verify, so the check simply reports success and engine start-up
	proceeds. It is deliberately kept as the one place where a platform may
	veto TVPSystemInit() instead of quietly dropping the call from it.

	TVPUpdateLicense(), which upstream retries TVPProtectInit() with, belongs to
	the USING_PROTECT build and is deliberately not declared here: that macro is
	defined by no configuration of this port, and should such a build ever be
	asked for, SysInitIntf.cpp must fail to compile rather than silently loop on
	a license check that does not exist.

	@return	true - the process is allowed to run.
*/
inline bool TVPProtectInit()
{
	return true;
}
//---------------------------------------------------------------------------

#endif
