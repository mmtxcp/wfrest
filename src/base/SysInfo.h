#ifndef WFREST_SYSINFO_H_
#define WFREST_SYSINFO_H_

#include <ctime>
#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include "workflow/PlatformSocket.h"
#include <windows.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#endif

#include <cstdio>

namespace wfrest
{

namespace CurrentThread
{
// internal
extern thread_local int t_cached_tid;
extern thread_local char t_tid_str[32];
extern thread_local int t_tid_str_len;

static long getCurrentThreadID(void)
{
#if defined(_WIN32) || defined(_WIN64)
	return GetCurrentThreadId();
#elif __linux__
	return syscall(SYS_gettid);
#elif defined(__APPLE__) && defined(__MACH__)
	return syscall(SYS_thread_selfid);
#else
	return (long)pthread_self();
#endif /* defined(_WIN32) || defined(_WIN64) */
}

#ifndef WIN32
inline pid_t gettid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

#else
typedef  int pid_t;

inline pid_t gettid() { return static_cast<pid_t>(getCurrentThreadID()); }
#endif // !1



inline void cacheTid()
{
    if (t_cached_tid == 0)
    {
        t_cached_tid = gettid();
        t_tid_str_len = snprintf(t_tid_str, sizeof t_tid_str, "%5d ", t_cached_tid);
    }
}
#ifndef WIN32
inline int tid()
{
    if (__builtin_expect(t_cached_tid == 0, 0))
    {
        cacheTid();
    }
    return t_cached_tid;
}
#else
inline int tid()
{
	if (t_cached_tid == 0)
	{
		cacheTid();
	}
	return t_cached_tid;
}
#endif // !1
// for logging
inline const char *tid_str() { return t_tid_str; }
// for logging
inline int tid_str_len() { return t_tid_str_len; }

}  // namespace CurrentThread

}  // wfrest

#endif // WFREST_SYSINFO_H_
