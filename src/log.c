/*
 * DirectGlide - Debug logging
 */

#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <windows.h>

static FILE* g_logFile = NULL;
static CRITICAL_SECTION g_logLock;
static int g_lockInit = 0;

void dg_log_init(const char* filename) {
    if (!g_lockInit) {
        InitializeCriticalSection(&g_logLock);
        g_lockInit = 1;
    }
    if (g_logFile) fclose(g_logFile);
    g_logFile = fopen(filename, "w");
    if (g_logFile) {
        dg_log("DirectGlide initialized\n");
    }
}

void dg_log_close(void) {
    if (g_logFile) {
        dg_log("DirectGlide shutdown\n");
        fclose(g_logFile);
        g_logFile = NULL;
    }
    if (g_lockInit) {
        DeleteCriticalSection(&g_logLock);
        g_lockInit = 0;
    }
}

void dg_log(const char* fmt, ...) {
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
}
