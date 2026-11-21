// zeebo_brew.cpp - Qualcomm BREW 4.0.2 module & applet entry glue for GTA III (re3).
#include "zeebo_brew.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef AEE_SIMULATOR
#include <windows.h>
static char s_simLogPath[MAX_PATH] = { 0 };
static PVOID s_simVehHandle = NULL;
static volatile DWORD s_wdHeartbeat = 0;

static void SimWriteLogFile(const char* text)
{
    if (!s_simLogPath[0] || !text) return;
    FILE* fp = fopen(s_simLogPath, "a");
    if (fp) {
        fputs(text, fp);
        fflush(fp);
        fclose(fp);
    }
}

extern "C" void Zeebo_SimFileLog(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SimWriteLogFile(buf);
}

static LONG WINAPI SimVectoredCrashHandler(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    HMODULE hmSelf = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&SimVectoredCrashHandler, &hmSelf);
    uintptr_t selfBase = (uintptr_t)hmSelf;

    HMODULE hmFault = NULL;
    char faultModName[MAX_PATH] = "unknown";
    uintptr_t eip = (uintptr_t)ep->ContextRecord->Eip;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)eip, &hmFault) && hmFault) {
        GetModuleFileNameA(hmFault, faultModName, MAX_PATH);
    }
    uintptr_t faultBase = (uintptr_t)hmFault;

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "\n=== SIM CRASH code=0x%08lX addr=%p EIP=%p (mod=%s+0x%X, selfBase=%p) ===\n"
             "EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX\n",
             (unsigned long)code,
             ep->ExceptionRecord->ExceptionAddress,
             (void*)eip,
             faultModName,
             (unsigned)(faultBase ? (eip - faultBase) : eip),
             (void*)selfBase,
             (unsigned long)ep->ContextRecord->Eax,
             (unsigned long)ep->ContextRecord->Ebx,
             (unsigned long)ep->ContextRecord->Ecx,
             (unsigned long)ep->ContextRecord->Edx,
             (unsigned long)ep->ContextRecord->Esi,
             (unsigned long)ep->ContextRecord->Edi,
             (unsigned long)ep->ContextRecord->Ebp,
             (unsigned long)ep->ContextRecord->Esp);
    SimWriteLogFile(msg);

    uintptr_t eax = (uintptr_t)ep->ContextRecord->Eax;
    if (eax >= 0x10000 && !IsBadReadPtr((const void*)eax, 16)) {
        const uint32_t* p = (const uint32_t*)eax;
        snprintf(msg, sizeof(msg), "EAX[0..3]: %08X %08X %08X %08X\n", p[0], p[1], p[2], p[3]);
        SimWriteLogFile(msg);
    }

    uintptr_t esp = (uintptr_t)ep->ContextRecord->Esp;
    if (esp >= 0x10000 && !IsBadReadPtr((const void*)esp, 256)) {
        const uint32_t* sp = (const uint32_t*)esp;
        SimWriteLogFile("Stack RVAs in 990002.dll:\n");
        for (int i = 0; i < 64; i++) {
            uintptr_t v = sp[i];
            if (selfBase && v >= selfBase && v < selfBase + 0x00800000) {
                snprintf(msg, sizeof(msg), "  [ESP+%02X] = 990002.dll+0x%06X\n", i * 4, (unsigned)(v - selfBase));
                SimWriteLogFile(msg);
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void SimSetModuleCwd(void)
{
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&SimSetModuleCwd, &hm) && hm) {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(hm, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            char* slash = strrchr(path, '\\');
            if (slash) {
                *slash = '\0';
                SetCurrentDirectoryA(path);
                snprintf(s_simLogPath, sizeof(s_simLogPath), "%s\\sim_debug.log", path);
                FILE* fp = fopen(s_simLogPath, "w");
                if (fp) {
                    fprintf(fp, "=== GTA3 Simulator Log Started (base=%p) ===\n", (void*)hm);
                    fclose(fp);
                }
            }
        }
    }
    if (!s_simVehHandle) {
        s_simVehHandle = AddVectoredExceptionHandler(1, SimVectoredCrashHandler);
    }
}
#endif

ZeeboRuntime g_Zeebo = { 0 };

void Zeebo_Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef AEE_SIMULATOR
    SimWriteLogFile(buf);
#endif
    if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnDbgPrintf) {
        g_Zeebo.pHelpers->pfnDbgPrintf("%s", buf);
    }
}

uint32_t Zeebo_TimeMs(void)
{
    if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnGetUpTimeMS) {
        return g_Zeebo.pHelpers->pfnGetUpTimeMS();
    }
    return 0;
}

typedef struct Gta3Applet {
    const IAppletVtbl* pvt;
    uint32_t           refCount;
} Gta3Applet;

static uint32_t Applet_AddRef(IApplet* po)
{
    Gta3Applet* self = (Gta3Applet*)po;
    return ++self->refCount;
}

static uint32_t Applet_Release(IApplet* po)
{
    Gta3Applet* self = (Gta3Applet*)po;
    if (self->refCount > 0) {
        --self->refCount;
    }
    if (self->refCount == 0 && g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnFree) {
        g_Zeebo.pHelpers->pfnFree(self);
        g_Zeebo.pApplet = NULL;
        return 0;
    }
    return self->refCount;
}

static uint32_t s_nextFrameMs = 0;

static void FrameTimerCallback(void* pUser)
{
    (void)pUser;
    if (!g_Zeebo.bRunning) {
        return;
    }
    uint32_t now = Zeebo_TimeMs();
    if (s_nextFrameMs == 0 || (int32_t)(now - s_nextFrameMs) > 66) {
        s_nextFrameMs = now;
    }
#ifdef AEE_SIMULATOR
    s_wdHeartbeat = GetTickCount();
#endif

    Zeebo_OnFrameTick();
#ifdef AEE_SIMULATOR
    s_wdHeartbeat = GetTickCount();
#endif
    s_nextFrameMs += 33u;

    if (g_Zeebo.bRunning && g_Zeebo.pShell) {
        // Fixed 33 ms (~30 fps) cadence without cumulative timer drift,
        // while always yielding at least 8 ms so BREW / Zeebx never starves.
        uint32_t after = Zeebo_TimeMs();
        int32_t rem = (int32_t)(s_nextFrameMs - after);
        if (rem < 8) {
            rem = 8;
            s_nextFrameMs = after + 8u;
        } else if (rem > 33) {
            rem = 33;
        }
        int tRc = ISHELL_SetTimer(g_Zeebo.pShell, rem, FrameTimerCallback, NULL);
#ifdef AEE_SIMULATOR
        if (tRc != AEE_SUCCESS) {
            Zeebo_SimFileLog("[TMR] ISHELL_SetTimer failed rc=%d rem=%d\n", tRc, (int)rem);
        }
#endif
    }
}

#ifdef AEE_SIMULATOR
// ---- Hang watchdog (simulator only): logs main-thread stack RVAs when frames stall ----
static HANDLE s_wdMainThread = NULL;
static volatile LONG s_wdStarted = 0;

static DWORD WINAPI SimWatchdogProc(LPVOID)
{
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&SimWatchdogProc, &self);
    DWORD base = (DWORD)(size_t)self;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    int logged = 0;
    for (;;) {
        Sleep(1000);
        DWORD hb = s_wdHeartbeat;
        if (!hb || !s_wdMainThread) continue;
        if (GetTickCount() - hb < 3000) { logged = 0; continue; }
        if (logged >= 4) continue;
        logged++;
        if (SuspendThread(s_wdMainThread) == (DWORD)-1) continue;
        CONTEXT ctx; memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        char line[4096]; int n = 0;
        if (GetThreadContext(s_wdMainThread, &ctx)) {
            n += _snprintf(line + n, sizeof(line) - n, "[WD] main thread stalled %lums EIP=%08lX (rva %08lX) ESP=%08lX\n",
                           GetTickCount() - hb, ctx.Eip, ctx.Eip - base, ctx.Esp);
            n += _snprintf(line + n, sizeof(line) - n, "[WD] game RVAs on stack:");
            const DWORD* sp = (const DWORD*)ctx.Esp;
            int found = 0;
            DWORD stackBuf[0x1000];
            SIZE_T bytesRead = 0;
            if (ReadProcessMemory(GetCurrentProcess(), sp, stackBuf, sizeof(stackBuf), &bytesRead)) {
                int count = (int)(bytesRead / sizeof(DWORD));
                for (int i = 0; i < count && found < 40; i++) {
                    DWORD v = stackBuf[i];
                    if (v >= base && v < base + size) {
                        n += _snprintf(line + n, sizeof(line) - n, " %X", v - base);
                        found++;
                    }
                }
            }
            n += _snprintf(line + n, sizeof(line) - n, "\n");
        }
        ResumeThread(s_wdMainThread);
        line[sizeof(line) - 1] = 0;
        SimWriteLogFile(line);
    }
    return 0;
}

static void SimWatchdogStart(void)
{
    if (InterlockedExchange(&s_wdStarted, 1)) return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_wdMainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    s_wdHeartbeat = GetTickCount();
    CreateThread(NULL, 0, SimWatchdogProc, NULL, 0, NULL);
}
#endif
static boolean Applet_HandleEvent(IApplet* po, AEEEvent eCode, uint16_t wParam, uint32_t dwParam)
{
    (void)po;
    switch (eCode) {
    case EVT_APP_START:
#ifdef AEE_SIMULATOR
        SimSetModuleCwd();
#endif
        if (!Zeebo_OnAppStart()) {
            return FALSE;
        }
        g_Zeebo.bRunning = TRUE;
#ifdef AEE_SIMULATOR
    SimWatchdogStart();
#endif
        s_nextFrameMs = 0;
        if (g_Zeebo.pShell) {
            ISHELL_SetTimer(g_Zeebo.pShell, 1, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_APP_STOP:
#ifdef AEE_SIMULATOR
        Zeebo_SimFileLog("[BREW] EVT_APP_STOP\n");
#endif
        g_Zeebo.bRunning = FALSE;
        if (g_Zeebo.pShell) {
            ISHELL_CancelTimer(g_Zeebo.pShell, FrameTimerCallback, NULL);
        }
        Zeebo_OnAppStop();
        return TRUE;

    case EVT_APP_SUSPEND:
#ifdef AEE_SIMULATOR
        Zeebo_SimFileLog("[BREW] EVT_APP_SUSPEND\n");
#endif
        if (g_Zeebo.pfnAudioSuspend) {
            g_Zeebo.pfnAudioSuspend();
        }
        if (g_Zeebo.pShell) {
            ISHELL_CancelTimer(g_Zeebo.pShell, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_APP_RESUME:
#ifdef AEE_SIMULATOR
        Zeebo_SimFileLog("[BREW] EVT_APP_RESUME\n");
#endif
        s_nextFrameMs = 0;
        if (g_Zeebo.bRunning && g_Zeebo.pShell) {
            ISHELL_SetTimer(g_Zeebo.pShell, 16, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_APP_NO_SLEEP:
        return TRUE;

    default:
        return FALSE;
    }
}

static const IAppletVtbl s_appletVtbl = {
    Applet_AddRef,
    Applet_Release,
    Applet_HandleEvent,
};

typedef struct Gta3Module {
    const IModuleVtbl* pvt;
    uint32_t           refCount;
} Gta3Module;

static uint32_t Module_AddRef(IModule* po)
{
    Gta3Module* self = (Gta3Module*)po;
    return ++self->refCount;
}

static uint32_t Module_Release(IModule* po)
{
    Gta3Module* self = (Gta3Module*)po;
    if (self->refCount > 0) {
        --self->refCount;
    }
    if (self->refCount == 0 && g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnFree) {
        g_Zeebo.pHelpers->pfnFree(self);
        return 0;
    }
    return self->refCount;
}

static int Module_CreateInstance(IModule* po, IShell* pIShell, AEECLSID clsId, void** ppObj)
{
    (void)po;
    if (!ppObj) {
        return EBADPARM;
    }
    *ppObj = NULL;

    if (clsId != AEECLSID_GTA3_ZEEBO && clsId != 0) {
        return ECLASSNOTSUPPORT;
    }

    if (!g_Zeebo.pHelpers || !g_Zeebo.pHelpers->pfnMalloc) {
        return ENOMEMORY;
    }

    Gta3Applet* app = (Gta3Applet*)g_Zeebo.pHelpers->pfnMalloc(sizeof(Gta3Applet));
    if (!app) {
        return ENOMEMORY;
    }
    app->pvt      = &s_appletVtbl;
    app->refCount = 1;

    g_Zeebo.pShell  = pIShell;
    g_Zeebo.pApplet = (IApplet*)app;

    ISHELL_CreateInstance(pIShell, AEECLSID_FILEMGR,   (void**)&g_Zeebo.pFileMgr);
    ISHELL_CreateInstance(pIShell, AEECLSID_DISPLAY,   (void**)&g_Zeebo.pDisplay);
    ISHELL_CreateInstance(pIShell, AEECLSID_HID,       (void**)&g_Zeebo.pHID);
    ISHELL_CreateInstance(pIShell, AEECLSID_EGL,       (void**)&g_Zeebo.pEGL);
    ISHELL_CreateInstance(pIShell, AEECLSID_GL,        (void**)&g_Zeebo.pGL);
    ISHELL_CreateInstance(pIShell, AEECLSID_MEDIAUTIL, (void**)&g_Zeebo.pMediaUtil);
    Zeebo_Log("[BREW] Module_CreateInstance ok, pFileMgr=%p pEGL=%p pGL=%p pHID=%p pMediaUtil=%p\n",
              g_Zeebo.pFileMgr, g_Zeebo.pEGL, g_Zeebo.pGL, g_Zeebo.pHID, g_Zeebo.pMediaUtil);

    *ppObj = app;
    return AEE_SUCCESS;
}

static void Module_FreeResources(IModule* po, IFileMgr* pIFileMgr, void* pIRes)
{
    (void)po;
    (void)pIFileMgr;
    (void)pIRes;
}

static const IModuleVtbl s_moduleVtbl = {
    Module_AddRef,
    Module_Release,
    Module_CreateInstance,
    Module_FreeResources,
};

#ifndef AEE_SIMULATOR
extern uint8_t __bss_start__[];
extern uint8_t __bss_end__[];
typedef void (*InitFn)(void);
extern InitFn __init_array_start[];
extern InitFn __init_array_end[];
#endif

static AEEHelperFuncs* ResolveHelpers(void* ph)
{
    if (ph) {
        return (AEEHelperFuncs*)ph;
    }
#ifdef AEE_SIMULATOR
    return NULL;
#else
    uintptr_t addr = ((uintptr_t)&AEEMod_Load) & ~(uintptr_t)3u;
    for (int i = 0; i < 256; ++i) {
        const uint32_t* p = (const uint32_t*)(addr - (uintptr_t)(i * 4));
        if (p[2] == 0x57455242u /* "BREW" */) {
            return *(AEEHelperFuncs* const*)(p - 1);
        }
    }
    return *(((AEEHelperFuncs* const*)(uintptr_t)&AEEMod_Load) - 1);
#endif
}

#ifdef AEE_SIMULATOR
extern "C" __declspec(dllexport)
#else
extern "C" __attribute__((section(".text.entry")))
#endif
int AEEMod_Load(IShell* pIShell, void* ph, IModule** ppMod)
{
#ifdef AEE_SIMULATOR
    SimSetModuleCwd();
#endif
    if (!ppMod) {
        return EBADPARM;
    }

#ifndef AEE_SIMULATOR
    for (uint8_t* p = __bss_start__; p < __bss_end__; ++p) {
        *p = 0;
    }
#endif

    memset(&g_Zeebo, 0, sizeof(g_Zeebo));
    g_Zeebo.pShell = pIShell;
    g_Zeebo.pHelpers = ResolveHelpers(ph);
    if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnDbgPrintf) {
        g_Zeebo.pHelpers->pfnDbgPrintf("[BREW] AEEMod_Load start\n");
    }

#ifndef AEE_SIMULATOR
    for (InitFn* fn = __init_array_start; fn < __init_array_end; ++fn) {
        if (*fn) {
            (*fn)();
        }
    }
#endif

    if (!g_Zeebo.pHelpers || !g_Zeebo.pHelpers->pfnMalloc) {
        return EFAILED;
    }

    Gta3Module* mod = (Gta3Module*)g_Zeebo.pHelpers->pfnMalloc(sizeof(Gta3Module));
    if (!mod) {
        return ENOMEMORY;
    }
    mod->pvt      = &s_moduleVtbl;
    mod->refCount = 1;

    *ppMod = (IModule*)mod;
    return AEE_SUCCESS;
}

#ifdef AEE_SIMULATOR
extern "C" void Zeebo_PoolLog(const char* m, int a, int b, int c){ Zeebo_Log("[POOL] %s size=%d a=%d b=%d\n", m, a, b, c); }
#endif
