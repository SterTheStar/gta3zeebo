// zeebo_brew.cpp - Qualcomm BREW 4.0.2 module & applet entry glue for GTA III (re3).
#include "zeebo_brew.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

ZeeboRuntime g_Zeebo = { 0 };

static void Zeebo_TraceFile(const char* msg)
{
    if (!g_Zeebo.pFileMgr || !g_Zeebo.pFileMgr->pvt || !msg) return;
    IFile* f = g_Zeebo.pFileMgr->pvt->OpenFile(g_Zeebo.pFileMgr, "zeebo_trace.txt", (OpenFileMode)(_OFM_READWRITE | _OFM_CREATE | _OFM_APPEND));
    if (!f) return;
    f->pvt->Seek(f, _SEEK_END, 0);
    f->pvt->Write(f, msg, (uint32_t)strlen(msg));
    f->pvt->Release(f);
}

void Zeebo_Log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Zeebo_TraceFile(buf);
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

static void FrameTimerCallback(void* pUser)
{
    (void)pUser;
    if (!g_Zeebo.bRunning) {
        return;
    }
    uint32_t startMs = Zeebo_TimeMs();
    Zeebo_OnFrameTick();
    uint32_t elapsed = Zeebo_TimeMs() - startMs;

    if (g_Zeebo.bRunning && g_Zeebo.pShell) {
        int32_t delay = (elapsed >= 25u) ? 8 : (int32_t)(33u - elapsed);
        ISHELL_SetTimer(g_Zeebo.pShell, delay, FrameTimerCallback, NULL);
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
        if (g_Zeebo.bRunning && g_Zeebo.pShell) {
            ISHELL_SetTimer(g_Zeebo.pShell, 16, FrameTimerCallback, NULL);
        }
        return TRUE;

    case EVT_KEY:
    case EVT_KEY_PRESS:
    case EVT_KEY_RELEASE:
        return Zeebo_OnKeyEvent(eCode, wParam, dwParam);

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

    ISHELL_CreateInstance(pIShell, AEECLSID_FILEMGR, (void**)&g_Zeebo.pFileMgr);
    ISHELL_CreateInstance(pIShell, AEECLSID_DISPLAY, (void**)&g_Zeebo.pDisplay);
    ISHELL_CreateInstance(pIShell, AEECLSID_HID,     (void**)&g_Zeebo.pHID);
    ISHELL_CreateInstance(pIShell, AEECLSID_EGL,     (void**)&g_Zeebo.pEGL);
    ISHELL_CreateInstance(pIShell, AEECLSID_GL,      (void**)&g_Zeebo.pGL);
    Zeebo_Log("[BREW] Module_CreateInstance ok, pFileMgr=%p pEGL=%p pGL=%p pHID=%p\n",
              g_Zeebo.pFileMgr, g_Zeebo.pEGL, g_Zeebo.pGL, g_Zeebo.pHID);

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

extern uint8_t __bss_start__[];
extern uint8_t __bss_end__[];
typedef void (*InitFn)(void);
extern InitFn __init_array_start[];
extern InitFn __init_array_end[];

extern "C" __attribute__((section(".text.entry")))
int AEEMod_Load(IShell* pIShell, void* ph, IModule** ppMod)
{
    (void)ph;
    if (!ppMod) {
        return EBADPARM;
    }

    for (uint8_t* p = __bss_start__; p < __bss_end__; ++p) {
        *p = 0;
    }

    memset(&g_Zeebo, 0, sizeof(g_Zeebo));
    g_Zeebo.pShell = pIShell;
    if (ph) {
        g_Zeebo.pHelpers = (AEEHelperFuncs*)ph;
        if (g_Zeebo.pHelpers->pfnDbgPrintf) {
            g_Zeebo.pHelpers->pfnDbgPrintf("[BREW] AEEMod_Load start\n");
        }
    }

    for (InitFn* fn = __init_array_start; fn < __init_array_end; ++fn) {
        if (*fn) {
            (*fn)();
        }
    }

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
