/*
 * DirectGlide - DLL entry point
 */

#include <windows.h>
#include <psapi.h>
#include "log.h"

/* Force NVIDIA discrete GPU on Optimus laptops */
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
/* Force AMD discrete GPU on switchable graphics */
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

static void logModules(void) {
    HMODULE mods[256];
    DWORD needed;
    unsigned int i;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        unsigned int count = needed / sizeof(HMODULE);
        if (count > 256) count = 256;
        for (i = 0; i < count; i++) {
            char name[MAX_PATH];
            if (GetModuleFileNameA(mods[i], name, MAX_PATH)) {
                dg_log("  MOD: 0x%08X %s\n", (unsigned)(UINT_PTR)mods[i], name);
            }
        }
    }
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        dg_log("!!! ACCESS VIOLATION: addr=0x%08X\n",
               (unsigned)(UINT_PTR)ep->ExceptionRecord->ExceptionAddress);
        if (ep->ContextRecord) {
            dg_log("    EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
                   ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
                   ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
            dg_log("    ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X\n",
                   ep->ContextRecord->Esi, ep->ContextRecord->Edi,
                   ep->ContextRecord->Ebp, ep->ContextRecord->Esp);
            dg_log("    EIP=0x%08X\n", ep->ContextRecord->Eip);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    (void)hinstDLL;
    (void)lpReserved;

    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            dg_log_init("DirectGlide.log");
            dg_log("DLL_PROCESS_ATTACH\n");
            logModules();
            AddVectoredExceptionHandler(1, crashHandler);
            break;
        case DLL_PROCESS_DETACH:
            dg_log("DLL_PROCESS_DETACH\n");
            dg_log_close();
            break;
    }
    return TRUE;
}
