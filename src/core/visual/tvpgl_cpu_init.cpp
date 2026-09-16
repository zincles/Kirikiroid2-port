/*
	Kirikiroid2 Linux/Switch port: non-ARM definition of the TVPGL CPU-routine
	initialization hook.

	`TVPBeforeSystemInit()` (src/core/base/win32/SysInitImpl.cpp) calls
	TVPGL_ASM_Init() unconditionally, after TVPInitTVPGL() (called from
	TVPSystemInit()) has bound every TVPGL entry point to its scalar C
	implementation. Upstream, the hook is where the platform's SIMD variants get
	routed in:

		- ARM builds: src/core/visual/ARM/tvpgl_arm.cpp registers the NEON
		  implementations (TVPxxx = TVPxxx_NEON) and is compiled for them.
		- x86 builds: krkrz's IA32/nasm tree generated the SSE2/AVX2 variants.
		  That tree is not part of this repository (it lived in the missing
		  vendor/ directory), so on x86 there is nothing to route.

	Hence on non-ARM this is a hook with no work to do: the scalar C path bound
	by TVPInitTVPGL() is the complete, correct implementation, only slower.
	Keeping the function (instead of dropping the call site) preserves the
	upstream extension point for when an x86 SIMD route is added.
*/

#if !defined(__arm__) && !defined(__aarch64__) && !defined(_M_ARM) && !defined(_M_ARM64)

extern "C" void TVPGL_ASM_Init()
{
}

#endif
