/*
 * DirectGlide - Debug logging.
 * Logging is globally gated by DG_LOG_ENABLED. Define it as 1 to re-enable
 * file logging for debugging; leave as 0 for release builds.
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <windows.h>

#ifndef DG_LOG_ENABLED
#define DG_LOG_ENABLED 0
#endif

#if DG_LOG_ENABLED
static FILE* g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static int g_lockInit = 0;
#endif

void dg_log_init(const char* filename) {
#if DG_LOG_ENABLED
    if (!g_lockInit) {
        InitializeCriticalSection(&g_logLock);
        g_lockInit = 1;
    }
    if (g_logFile) fclose(g_logFile);
    g_logFile = fopen(filename, "w");
    if (g_logFile) {
        dg_log("DirectGlide initialized\n");
    }
#else
    (void)filename;
#endif
}

void dg_log_close(void) {
#if DG_LOG_ENABLED
    if (g_logFile) {
        dg_log("DirectGlide shutdown\n");
        fclose(g_logFile);
        g_logFile = NULL;
    }
    if (g_lockInit) {
        DeleteCriticalSection(&g_logLock);
        g_lockInit = 0;
    }
#endif
}

void dg_log(const char* fmt, ...) {
#if DG_LOG_ENABLED
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    {
        va_list args;
        va_start(args, fmt);
        vfprintf(g_logFile, fmt, args);
        va_end(args);
        fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock);
#else
    (void)fmt;
#endif
}
