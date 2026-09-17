#ifndef _PROFILER_H_
#define _PROFILER_H_

#define PROF_ALL    0
#define PROF_VIDEO  1
#define PROF_68K    2
#define PROF_Z80    3
#define PROF_SOUND  4

#define PROFILER_START(x) ((void)0)
#define PROFILER_STOP(x)  ((void)0)

static inline void profiler_show_stat(void) {}

#endif
