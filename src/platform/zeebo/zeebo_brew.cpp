// zeebo_brew.cpp - Qualcomm BREW 4.0.2 module & applet entry glue for GTA III (re3).
#include "zeebo_brew.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef AEE_SIMULATOR
#include <windows.h>
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
            }
        }
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

    Zeebo_OnFrameTick();
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
        ISHELL_SetTimer(g_Zeebo.pShell, rem, FrameTimerCallback, NULL);
    }
}

static boolean Applet_HandleEvent(IApplet* po, AEEEvent eCode, uint16_t wParam, uint32_t dwParam)
{
    (void)po;
    switch (eCode) {
    case EVT_APP_START:
        if (!Zeebo_OnAppStart()) {
            return FALSE;
        }
        g_Zeebo.bRunning = TRUE;
        s_nextFrameMs = 0;
        if (g_Zeebo.pShell) {
            ISHELL_SetTimer(g_Zeebo.pShell, 1, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_APP_STOP:
        g_Zeebo.bRunning = FALSE;
        if (g_Zeebo.pShell) {
            ISHELL_CancelTimer(g_Zeebo.pShell, FrameTimerCallback, NULL);
        }
        Zeebo_OnAppStop();
        return TRUE;

    case EVT_APP_SUSPEND:
        if (g_Zeebo.pShell) {
            ISHELL_CancelTimer(g_Zeebo.pShell, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_APP_RESUME:
        s_nextFrameMs = 0;
        if (g_Zeebo.bRunning && g_Zeebo.pShell) {
            ISHELL_SetTimer(g_Zeebo.pShell, 16, FrameTimerCallback, NULL);
        }
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
