#ifdef USE_SDL3
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#else
#include <SDL.h>
#include <SDL_main.h>
#endif

#ifdef __SWITCH__
#include "platform/switch/network.h"
#include "platform/switch/random.hpp"
#include "platform/switch/romfs.hpp"
#endif
#ifdef __3DS__
#include "platform/ctr/system.h"
#endif
#ifdef __vita__
#include "platform/vita/network.h"
#include "platform/vita/random.hpp"
#endif
#ifdef NXDK
#include <nxdk/mount.h>
#endif
#ifdef GPERF_HEAP_MAIN
#include <gperftools/heap-profiler.h>
#endif

#include "diablo.h"

#if !defined(__APPLE__)
extern "C" const char *__asan_default_options() // NOLINT(bugprone-reserved-identifier, readability-identifier-naming)
{
	return "halt_on_error=0";
}
#endif

#ifdef __NDS__
extern "C" void nds_platform_init(int *argc, char ***argv);
#endif

#ifdef __NDS__
#if __has_include(<lua.hpp>)
#include <lua.hpp>
#define NDS_LUA_SELFTEST 1
#endif
#include <SDL.h>
#include <cstdio>

static void NdsLuaSelfTest()
{
#ifdef NDS_LUA_SELFTEST
	lua_State *L = luaL_newstate();
	if (L == nullptr) {
		std::printf("LUATEST: sin estado\n");
	} else {
		luaL_openlibs(L);
		const int r = luaL_dostring(L,
		    "local t = {} for i = 1, 500 do t['k' .. i] = i end "
		    "local s = 0 for k, v in pairs(t) do s = s + v end "
		    "return type(pairs) .. ' ' .. s");
		const char *res = lua_tostring(L, -1);
		std::printf("LUATEST r=%d %s\n", r, res != nullptr ? res : "(nil)");
		lua_close(L);
	}
#else
	std::printf("LUATEST: no compilado\n");
#endif
	SDL_Delay(4000);
}
#endif

#ifdef __NDS__
#define main DevilutionXMain
#endif
extern "C" int main(int argc, char **argv)
{
#ifdef __NDS__
	nds_platform_init(&argc, &argv);
	NdsLuaSelfTest();
#endif
#ifdef __SWITCH__
	switch_romfs_init();
	switch_enable_network();
#ifdef PACKET_ENCRYPTION
	randombytes_switchrandom_init();
#endif
#endif
#ifdef __3DS__
	ctr_sys_init();
#endif
#ifdef __vita__
	vita_enable_network();
#ifdef PACKET_ENCRYPTION
	randombytes_vitarandom_init();
#endif
#endif
#ifdef NXDK
	nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
#endif
#ifdef GPERF_HEAP_MAIN
	HeapProfilerStart("main");
#endif
	const int result = devilution::DiabloMain(argc, argv);
#ifdef GPERF_HEAP_MAIN
	HeapProfilerStop();
#endif
	return result;
}

#ifdef __NDS__
#undef main
#include <nds/cothread.h>
#include <threads.h>

static int nds_saved_argc;
static char **nds_saved_argv;

static int NdsEngineThread(void *)
{
	return DevilutionXMain(nds_saved_argc, nds_saved_argv);
}

extern "C" int main(int argc, char **argv)
{
	int result = 0;
	nds_saved_argc = argc;
	nds_saved_argv = argv;
	const cothread_t engine = cothread_create(NdsEngineThread, nullptr, 512 * 1024, 0);
	if (engine < 0)
		return 1;
	thrd_join(engine, &result);
	return result;
}
#endif
