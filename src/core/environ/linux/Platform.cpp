#include "Platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifdef __SWITCH__
// Nintendo Switch (devkitA64/libnx): no /proc, no swap; the kernel reports the
// DRAM budget through svcGetSystemInfo.
#include <switch.h>
#include <sys/stat.h>
#include <fcntl.h>
#else
#include <sys/sysinfo.h>
#endif

void TVPGetMemoryInfo(TVPMemoryInfo &m)
{
#ifdef __SWITCH__
	// All values in kB, matching the /proc/meminfo units used below.
	u64 total = 0, used = 0;
	if (R_SUCCEEDED(svcGetSystemInfo(&total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, 0))) {
		m.MemTotal = (unsigned long)(total / 1024);
	}
	if (R_SUCCEEDED(svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, 0))) {
		m.MemFree = (used < total) ? (unsigned long)((total - used) / 1024) : 0;
	}
	// The Switch has no swap and no vmalloc accounting.
	m.SwapTotal = m.SwapFree = 0;
	m.VirtualTotal = m.VirtualUsed = 0;
#else
    /* to read /proc/meminfo */
    FILE* meminfo;
    char buffer[100] = {0};
    char* end;
    int found = 0;

    /* Try to read /proc/meminfo, bail out if fails */
	meminfo = fopen("/proc/meminfo", "r");

    static const char
        pszMemFree[] = "MemFree:",
        pszMemTotal[] = "MemTotal:",
        pszSwapTotal[] = "SwapTotal:",
        pszSwapFree[] = "SwapFree:",
        pszVmallocTotal[] = "VmallocTotal:",
        pszVmallocUsed[] = "VmallocUsed:";

    /* Read each line untill we got all we ned */
    while( fgets( buffer, sizeof( buffer ), meminfo ) )
    {
        if( strstr( buffer, pszMemFree ) == buffer )
        {
            m.MemFree = strtol( buffer + sizeof(pszMemFree), &end, 10 );
            found++;
        }
        else if( strstr( buffer, pszMemTotal ) == buffer )
        {
            m.MemTotal = strtol( buffer + sizeof(pszMemTotal), &end, 10 );
            found++;
        }
        else if( strstr( buffer, pszSwapTotal ) == buffer )
        {
            m.SwapTotal = strtol( buffer + sizeof(pszSwapTotal), &end, 10 );
            found++;
            break;
        }
        else if( strstr( buffer, pszSwapFree ) == buffer )
        {
            m.SwapFree = strtol( buffer + sizeof(pszSwapFree), &end, 10 );
            found++;
            break;
        }
        else if( strstr( buffer, pszVmallocTotal ) == buffer )
        {
            m.VirtualTotal = strtol( buffer + sizeof(pszVmallocTotal), &end, 10 );
            found++;
            break;
        }
        else if( strstr( buffer, pszVmallocUsed ) == buffer )
        {
            m.VirtualUsed = strtol( buffer + sizeof(pszVmallocUsed), &end, 10 );
            found++;
            break;
        }
    }
    fclose(meminfo);
#endif
}

#ifdef __SWITCH__
void TVPRelinquishCPU(){
	// Give the scheduler a chance to run another thread (libnx has no
	// sched_yield; a zero-length sleep is the documented equivalent).
	svcSleepThread(0);
}

void TVP_utime(const char *name, time_t modtime) {
	struct timespec ts[2];
	ts[0].tv_sec = modtime;
	ts[0].tv_nsec = 0;
	ts[1] = ts[0];
	utimensat(AT_FDCWD, name, ts, 0);
}
#else
#include <sched.h>
void TVPRelinquishCPU(){
	sched_yield();
}

void TVP_utime(const char *name, time_t modtime) {
	timeval mt[2];
	mt[0].tv_sec = modtime;
	mt[0].tv_usec = 0;
	mt[1].tv_sec = modtime;
	mt[1].tv_usec = 0;
	utimes(name, mt);
}
#endif
