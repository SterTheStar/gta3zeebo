// zeebo.cpp - re3 skeleton / platform layer for Qualcomm BREW 4.0.2 / Zeebo.
#ifdef __ZEEBO__

#include "common.h"
#include "crossplatform.h"
#include "rwcore.h"
#include "skeleton.h"
#include "platform.h"
#include "main.h"
#include "Game.h"
#include "Draw.h"
#include "Frontend.h"
#include "Pad.h"
#include "PCSave.h"
#include "FileMgr.h"
#include "Streaming.h"
#include "Timer.h"
#include "DMAudio.h"
#ifdef _WIN32
#include <direct.h>
#endif

#include "../../platform/zeebo/zeebo_brew.h"
#include "../../platform/zeebo/zeebo_movie.h"
#include "../../platform/zeebo/zeebo_gles1.h"
#include "../../platform/zeebo/zeebo_mempanel.h"
#include "../../platform/zeebo/pandora_logo.h"
#include "../../platform/zeebo/splash_image.h"

namespace rw {
namespace gles1 {
void notifyTextureBound(unsigned int texid);
void setClientStates(bool vert, bool norm, bool color, bool tex);
void setLightingState(bool lighting, bool rescaleNormal, bool colorMaterial);
bool isTextureEnabled(void);
}
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
DWORD _dwOperatingSystemVersion = 0;
#ifdef _MSC_VER
int strcasecmp(const char *str1, const char *str2)
{
    return _strcmpi(str1, str2);
}
#endif
#else
long _dwOperatingSystemVersion = 0;
#endif
size_t _dwMemAvailPhys = 64 * 1024 * 1024;
RwUInt32 gGameState = 0;

void _psCreateFolder(const char *path);

const char *_psGetUserFilesFolder(void)
{
	static bool s_dirCreated = false;
	if(!s_dirCreated){
		_psCreateFolder("userfiles");
		s_dirCreated = true;
	}
	return "userfiles";
}

FILE *_fcaseopen(char const *filename, char const *mode)
{
	if(!filename || !mode) return NULL;
	char buf[260];
	size_t i = 0;
	for(; filename[i] && i + 1 < sizeof(buf); i++){
		buf[i] = (filename[i] == '\\') ? '/' : filename[i];
	}
	buf[i] = '\0';
	return fopen(buf, mode);
}

int _caserename(char const *oldname, char const *newname)
{
	if(!oldname || !newname) return -1;
	return rename(oldname, newname);
}

static RwBool      s_rwInitialized = FALSE;
static IHIDDevice *s_hidDevice     = NULL;
static uint32_t    s_nextRescanMs  = 0;
static uint32_t    s_hidBtnState   = 0;
static uint32_t    s_prevHidBtn    = 0;

void _psCreateFolder(const char *path)
{
#ifdef _WIN32
	if(path) _mkdir(path);
#endif
	if(path && g_Zeebo.pFileMgr && g_Zeebo.pFileMgr->pvt){
		g_Zeebo.pFileMgr->pvt->MkDir(g_Zeebo.pFileMgr, path);
	}
}

double psTimer(void)
{
	return (double)Zeebo_TimeMs();
}

RwBool psCameraBeginUpdate(RwCamera *camera)
{
	if(!RwCameraBeginUpdate(camera)){
		RsEventHandler(rsCAMERASIZE, nil);
		return FALSE;
	}
	return TRUE;
}

void psCameraShowRaster(RwCamera *camera)
{
	RwCameraShowRaster(camera, nil, 0);
}

RwImage *psGrabScreen(RwCamera *pCamera)
{
	(void)pCamera;
	return nil;
}

void psMouseSetPos(RwV2d *pos)
{
	(void)pos;
}

#ifdef AEE_SIMULATOR
extern "C" int Zeebo_PurgeMemoryForAlloc(size_t needed);

static void *SimBrewMalloc(size_t size)
{
	if(g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnMalloc){
		uint32_t req = (uint32_t)(size ? size : 1);
		void *p = g_Zeebo.pHelpers->pfnMalloc(req);
		if(!p && g_Zeebo.bRunning){
			if(Zeebo_PurgeMemoryForAlloc(req)){
				p = g_Zeebo.pHelpers->pfnMalloc(req);
			}
		}
		return p;
	}
	return malloc(size);
}

static void SimBrewFree(void *mem)
{
	if(!mem) return;
	if(g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnFree){
		g_Zeebo.pHelpers->pfnFree(mem);
	}else{
		free(mem);
	}
}

static void *SimBrewRealloc(void *mem, size_t newSize)
{
	if(g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnRealloc){
		uint32_t req = (uint32_t)(newSize ? newSize : 1);
		void *p = g_Zeebo.pHelpers->pfnRealloc(mem, req);
		if(!p && g_Zeebo.bRunning){
			if(Zeebo_PurgeMemoryForAlloc(req)){
				p = g_Zeebo.pHelpers->pfnRealloc(mem, req);
			}
		}
		return p;
	}
	return realloc(mem, newSize);
}

static void *SimBrewCalloc(size_t numObj, size_t sizeObj)
{
	size_t total = numObj * sizeObj;
	void *p = SimBrewMalloc(total);
	if(p) memset(p, 0, total);
	return p;
}

static RwMemoryFunctions s_simBrewMemFuncs = {
	SimBrewMalloc,
	SimBrewFree,
	SimBrewRealloc,
	SimBrewCalloc
};
#endif

RwMemoryFunctions *psGetMemoryFunctions(void)
{
#ifdef AEE_SIMULATOR
	if(g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnMalloc && g_Zeebo.pHelpers->pfnFree && g_Zeebo.pHelpers->pfnRealloc){
		return &s_simBrewMemFuncs;
	}
#endif
	return nil;
}

RwBool psInstallFileSystem(void)
{
	return TRUE;
}

RwBool psNativeTextureSupport(void)
{
	return TRUE;
}

RwBool psInitialize(void)
{
	RsGlobal.appName      = "GTA III Zeebo";
	RsGlobal.width        = 640;
	RsGlobal.height       = 480;
	RsGlobal.maximumWidth = 640;
	RsGlobal.maximumHeight= 480;
	RsGlobal.maxFPS       = 30;
	RsGlobal.quit         = FALSE;

	CFileMgr::Initialise();
	C_PcSave::SetSaveDirectory(_psGetUserFilesFolder());

	return TRUE;
}

void psTerminate(void)
{
	if(s_hidDevice && s_hidDevice->pvt){
		s_hidDevice->pvt->Release(s_hidDevice);
		s_hidDevice = NULL;
	}
}

RwBool psSelectDevice(void)
{
	return TRUE;
}

void _InputTranslateShiftKeyUpDown(RsKeyCodes *rs)
{
	(void)rs;
}

long _InputInitialiseMouse(void)
{
	return 0;
}

void _InputInitialiseJoys(void)
{
}

void HandleExit(void)
{
	if(!RsGlobal.quit) return;
	g_Zeebo.bRunning = FALSE;
	if(g_Zeebo.pShell){
		ISHELL_CloseApplet(g_Zeebo.pShell, TRUE);
	}
}

void _psSelectScreenVM(RwInt32 videoMode)
{
	(void)videoMode;
}

void InitialiseLanguage(void)
{
}

RwBool _psSetVideoMode(RwInt32 subSystem, RwInt32 videoMode)
{
	(void)subSystem;
	(void)videoMode;
	return TRUE;
}

static char s_vmName[] = "640x480x16";
static char *s_vmList[1] = { s_vmName };

RwChar **_psGetVideoModeList(void)
{
	return s_vmList;
}

RwInt32 _psGetNumVideModes(void)
{
	return 1;
}

// ---------------------------------------------------------------------------
// Z-Pad / IHID polling -> RsPad / RsKeyboard events
// ---------------------------------------------------------------------------
// Standard BREW / Tectoy IHID Joystick Button & Axis UIDs (AEEHIDDevice_Joystick.h)
#define ZEEBO_UID_DPAD_UP         0x0106c3feu
#define ZEEBO_UID_DPAD_LEFT       0x0106c3ffu
#define ZEEBO_UID_DPAD_DOWN       0x0106c400u
#define ZEEBO_UID_DPAD_RIGHT      0x0106c401u
#define ZEEBO_UID_START           0x0106c402u
#define ZEEBO_UID_BACK            0x0106c403u
#define ZEEBO_UID_LTHUMB          0x0106c404u
#define ZEEBO_UID_RTHUMB          0x0106c405u
#define ZEEBO_UID_ZL              0x0106c406u
#define ZEEBO_UID_L2              0x0106c407u
#define ZEEBO_UID_ZR              0x0106c408u
#define ZEEBO_UID_R2              0x0106c409u
#define ZEEBO_UID_BTN_1           0x0106c40au
#define ZEEBO_UID_BTN_2           0x0106c40bu
#define ZEEBO_UID_BTN_3           0x0106c40cu
#define ZEEBO_UID_BTN_4           0x0106c40du

#define ZEEBO_UID_AXIS_RX         0x0106c4ceu
#define ZEEBO_UID_AXIS_RY         0x0106c4cfu
#define ZEEBO_UID_AXIS_LX         0x0106c4d0u
#define ZEEBO_UID_AXIS_LY         0x0106c4d1u

static int32_t s_axisMin[25];
static int32_t s_axisMax[25];
static int32_t s_axisUid[25];
static int     s_idxLX = 1;
static int     s_idxLY = 2;
static int     s_idxRX = 3;
static int     s_idxRY = 6;
static boolean s_axesQueried = FALSE;

static void QueryPadAxes(IHIDDevice *dev)
{
	for(int i = 0; i < 25; i++){
		s_axisMin[i] = 0;
		s_axisMax[i] = 0;
		s_axisUid[i] = 0;
	}
	s_idxLX = 1;
	s_idxLY = 2;
	s_idxRX = 3;
	s_idxRY = 6;
	s_axesQueried = FALSE;
	if(!dev || !dev->pvt) return;

	boolean hasMin = (dev->pvt->GetMinPositionInfo && dev->pvt->GetMinPositionInfo(dev, s_axisMin) == AEE_SUCCESS);
	boolean hasMax = (dev->pvt->GetMaxPositionInfo && dev->pvt->GetMaxPositionInfo(dev, s_axisMax) == AEE_SUCCESS);
	boolean hasUid = (dev->pvt->GetAxesInfo && dev->pvt->GetAxesInfo(dev, s_axisUid) == AEE_SUCCESS);

	if(hasUid){
		for(int i = 1; i < 25; i++){
			uint32_t u = (uint32_t)s_axisUid[i];
			if(u == ZEEBO_UID_AXIS_LX)      s_idxLX = i;
			else if(u == ZEEBO_UID_AXIS_LY) s_idxLY = i;
			else if(u == ZEEBO_UID_AXIS_RX) s_idxRX = i;
			else if(u == ZEEBO_UID_AXIS_RY) s_idxRY = i;
		}
	}
	s_axesQueried = (hasMin && hasMax) ? TRUE : FALSE;
}

static int NormalizeHidAxis(int slot, const int32_t *posInfo)
{
	if(slot < 1 || slot >= 25) return 0;
	int32_t raw = posInfo[slot];

	if(s_axesQueried){
		int32_t mn = s_axisMin[slot];
		int32_t mx = s_axisMax[slot];
		// In BREW 4.0.2 Simulator IHID.dll, unpresent/unmapped axes report min==max (0..0).
		// Return 0 (centered) instead of computing 0 - 128 = -128 (constant UP/LEFT drift!).
		if(mx <= mn) return 0;
		int32_t center = (mn + mx) / 2;
		int32_t half   = (mx - mn) / 2;
		if(half <= 0) return 0;
		int32_t norm = ((raw - center) * 127) / half;
		if(norm < -128) norm = -128;
		else if(norm > 127) norm = 127;
		return (int)norm;
	}

#ifdef AEE_SIMULATOR
	// If GetMinPositionInfo/GetMaxPositionInfo is unavailable in Simulator and raw is 0, treat as centered
	if(raw == 0) return 0;
#endif
	int v = (int)(raw - 128);
	if(v < -128) v = -128;
	else if(v > 127) v = 127;
	return v;
}

static int MapHidButtonToZeeboIndex(const AEEHIDButtonInfo &btn)
{
	switch(btn.dwUID){
	case ZEEBO_UID_BTN_2:      return 0;  // Z-Pad Button 1 (South / Cross)
	case ZEEBO_UID_BTN_3:      return 1;  // Z-Pad Button 2 (East / Circle)
	case ZEEBO_UID_BTN_1:      return 2;  // Z-Pad Button 4 (West / Square)
	case ZEEBO_UID_BTN_4:      return 3;  // Z-Pad Button 3 (North / Triangle)
	case ZEEBO_UID_ZR:         return 4;  // ZR (RightShoulder1)
	case ZEEBO_UID_L2:         return 5;  // L2 (LeftShoulder2)
	case ZEEBO_UID_ZL:         return 6;  // ZL (LeftShoulder1)
	case ZEEBO_UID_R2:         return 7;  // R2 (RightShoulder2)
	case ZEEBO_UID_RTHUMB:     return 8;  // R3
	case ZEEBO_UID_BACK:       return 9;  // Back / Select
	case ZEEBO_UID_LTHUMB:     return 10; // L3
	case ZEEBO_UID_START:      return 11; // HOME / Start
	case ZEEBO_UID_DPAD_UP:    return 12; // D-Pad UP
	case ZEEBO_UID_DPAD_LEFT:  return 13; // D-Pad LEFT
	case ZEEBO_UID_DPAD_DOWN:  return 14; // D-Pad DOWN
	case ZEEBO_UID_DPAD_RIGHT: return 15; // D-Pad RIGHT
	default:
		break;
	}
	return (btn.dwID < 32) ? (int)btn.dwID : -1;
}

static void TryOpenPad(void)
{
	if(s_hidDevice || !g_Zeebo.pHID || !g_Zeebo.pHID->pvt) return;
	uint32_t now = Zeebo_TimeMs();
	if(now < s_nextRescanMs) return;
	s_nextRescanMs = now + 1000u;

	int handles[4] = { 0 };
	int needed = 0;
	if(g_Zeebo.pHID->pvt->GetConnectedDevices(g_Zeebo.pHID, (int)AEEHID_UID_JOYSTICK_DEVICE, handles, 4, &needed) == AEE_SUCCESS && needed > 0 && handles[0] != 0){
		IHIDDevice *dev = NULL;
		if(g_Zeebo.pHID->pvt->CreateDevice(g_Zeebo.pHID, handles[0], &dev) == AEE_SUCCESS && dev){
			s_hidDevice = dev;
			s_hidBtnState = 0;
			QueryPadAxes(dev);
		}
	}
}

static int MapZeeboKey(uint16_t keyCode)
{
	switch(keyCode){
	case AVK_UP:     return rsUP;
	case AVK_DOWN:   return rsDOWN;
	case AVK_LEFT:   return rsLEFT;
	case AVK_RIGHT:  return rsRIGHT;
	case AVK_SELECT: return rsENTER;
	case AVK_CLR:    return rsESC;
	case AVK_SOFT1:  return ' ';
	case AVK_SOFT2:  return rsLSHIFT;
	case AVK_1:      return '1';
	case AVK_2:      return '2';
	case AVK_3:      return '3';
	case AVK_4:      return '4';
	default:         return rsNULL;
	}
}

void CPad::AffectFromZeebo(void)
{
	TryOpenPad();
	if(!s_hidDevice || !s_hidDevice->pvt) return;

	// Drain queued button press/release events from IHIDDevice (re1zeebo Z-Pad layout)
	AEEHIDButtonInfo btn;
	uint32_t ts = 0;
	boolean dropped = FALSE;
	while(s_hidDevice->pvt->GetNextButtonEvent(s_hidDevice, &btn, &ts, &dropped) == AEE_SUCCESS){
		int idx = MapHidButtonToZeeboIndex(btn);
		if(idx >= 0 && idx < 32){
			if(btn.dwState) s_hidBtnState |=  (1u << idx);
			else            s_hidBtnState &= ~(1u << idx);
		}
	}
	s_prevHidBtn = s_hidBtnState;

	// Toggle memory diagnostics panel on Start + D-Pad Down
	static bool s_prevMemCombo = false;
	const uint32_t kMemComboMask = (1u << 11) | (1u << 14);
	bool memCombo = ((s_hidBtnState & kMemComboMask) == kMemComboMask);
	if(memCombo && !s_prevMemCombo){
		ZeeboMemPanel::Toggle();
	}
	s_prevMemCombo = memCombo;

	// Map Z-Pad button indices (zeebx-emu BUTTON_UIDS order) -> CControllerState
	PCTempJoyState.Cross          = (s_hidBtnState & (1u << 0))  ? 255 : 0; // b1 / A
	PCTempJoyState.Circle         = (s_hidBtnState & (1u << 1))  ? 255 : 0; // b2 / B
	PCTempJoyState.Square         = (s_hidBtnState & (1u << 2))  ? 255 : 0; // b3 / X
	PCTempJoyState.Triangle       = (s_hidBtnState & (1u << 3))  ? 255 : 0; // b4 / Y
	PCTempJoyState.RightShoulder1 = (s_hidBtnState & (1u << 4))  ? 255 : 0; // zr
	PCTempJoyState.LeftShoulder2  = (s_hidBtnState & (1u << 5))  ? 255 : 0; // l2
	PCTempJoyState.LeftShoulder1  = (s_hidBtnState & (1u << 6))  ? 255 : 0; // zl
	PCTempJoyState.RightShoulder2 = (s_hidBtnState & (1u << 7))  ? 255 : 0; // r2
	PCTempJoyState.RightShock     = (s_hidBtnState & (1u << 8))  ? 255 : 0; // rthumb
	PCTempJoyState.Select         = (s_hidBtnState & (1u << 9))  ? 255 : 0; // back / HOME
	PCTempJoyState.LeftShock      = (s_hidBtnState & (1u << 10)) ? 255 : 0; // lthumb
	PCTempJoyState.Start          = (!memCombo && (s_hidBtnState & (1u << 11))) ? 255 : 0; // start
	PCTempJoyState.DPadUp         = (s_hidBtnState & (1u << 12)) ? 255 : 0; // D-Pad UP
	PCTempJoyState.DPadLeft       = (s_hidBtnState & (1u << 13)) ? 255 : 0; // D-Pad LEFT
	PCTempJoyState.DPadDown       = (s_hidBtnState & (1u << 14)) ? 255 : 0; // D-Pad DOWN
	PCTempJoyState.DPadRight      = (s_hidBtnState & (1u << 15)) ? 255 : 0; // D-Pad RIGHT

	// Read analog stick axes normalized via GetMinPositionInfo/GetMaxPositionInfo/GetAxesInfo
	int32_t posInfo[25];
	for(int i = 0; i < 25; i++) posInfo[i] = 128;
	if(s_hidDevice->pvt->GetPositionState(s_hidDevice, posInfo) == AEE_SUCCESS){
		int lx = NormalizeHidAxis(s_idxLX, posInfo);
		int ly = NormalizeHidAxis(s_idxLY, posInfo);
		int rx = NormalizeHidAxis(s_idxRX, posInfo);
		int ry = NormalizeHidAxis(s_idxRY, posInfo);
#ifdef AEE_SIMULATOR
		// BREW 4.0.2 Simulator IHID.dll inverts Y and Rz axes (INVERT:Y / INVERT:Rz in hid_devices.cfg
		// so UP=+32767 and DOWN=-32768), whereas Zeebo/CPad expects UP=-128 and DOWN=+127.
		ly = -ly;
		ry = -ry;
#endif
		const int kDeadzone = 16;
		if(lx > -kDeadzone && lx < kDeadzone) lx = 0;
		if(ly > -kDeadzone && ly < kDeadzone) ly = 0;
		if(rx > -kDeadzone && rx < kDeadzone) rx = 0;
		if(ry > -kDeadzone && ry < kDeadzone) ry = 0;
		if(lx < -128) lx = -128; else if(lx > 127) lx = 127;
		if(ly < -128) ly = -128; else if(ly > 127) ly = 127;
		if(rx < -128) rx = -128; else if(rx > 127) rx = 127;
		if(ry < -128) ry = -128; else if(ry > 127) ry = 127;
		PCTempJoyState.LeftStickX  = (int16)lx;
		PCTempJoyState.LeftStickY  = (int16)ly;
		PCTempJoyState.RightStickX = (int16)rx;
		PCTempJoyState.RightStickY = (int16)ry;
	}
}

// ---------------------------------------------------------------------------
// Startup Splash Screens (Pandora Studios logo -> Warning screen)
// ---------------------------------------------------------------------------
enum StartupStage {
	STARTUP_NONE    = 0,
	STARTUP_PANDORA = 1,
	STARTUP_WARNING = 2,
};

static StartupStage s_startupStage  = STARTUP_NONE;
static unsigned int s_splashTex     = 0;
static uint32_t     s_splashStartMs = 0;

static unsigned int CreateRleSplashTexture(int w, int h, const uint32_t *rle, uint32_t runs)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || w <= 0 || h <= 0 || w > 1024 || h > 512) return 0;

	const int texW = 1024;
	const int texH = 512;
	uint16_t *rgb565 = (uint16_t*)calloc((size_t)texW * (size_t)texH, sizeof(uint16_t));
	if(!rgb565) return 0;

	size_t totalPixels = (size_t)w * (size_t)h;
	size_t out = 0;
	for(uint32_t r = 0; r < runs && out < totalPixels; ++r){
		uint32_t count = rle[r * 2 + 0];
		uint32_t px    = rle[r * 2 + 1];
		uint16_t c565  = (uint16_t)((((px >> 0) & 0xF8u) << 8) | (((px >> 8) & 0xFCu) << 3) | (((px >> 16) & 0xF8u) >> 3));
		if(out + count > totalPixels) count = (uint32_t)(totalPixels - out);
		for(uint32_t k = 0; k < count; ++k){
			size_t x = out % (size_t)w;
			size_t y = out / (size_t)w;
			rgb565[y * (size_t)texW + x] = c565;
			out++;
		}
	}

	unsigned int texId = 0;
	gl->glGenTextures(1, &texId);
	if(texId){
		gl->glBindTexture(GL_TEXTURE_2D, texId);
		rw::gles1::notifyTextureBound(texId);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texW, texH, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, rgb565);
	}
	free(rgb565);
	return texId;
}

static void FreeStartupSplash(void)
{
	if(s_splashTex){
		if(const IGLVtbl *gl = ZeeboGL()){
			gl->glDeleteTextures(1, &s_splashTex);
		}
		s_splashTex = 0;
	}
	s_startupStage = STARTUP_NONE;
}

static void StartWarningSplash(void)
{
	FreeStartupSplash();
	s_splashTex = CreateRleSplashTexture(ZEEBO_SPLASH_W, ZEEBO_SPLASH_H,
	                                     ZEEBO_SPLASH_RLE, ZEEBO_SPLASH_RLE_RUNS);
	if(s_splashTex){
		s_startupStage  = STARTUP_WARNING;
		s_splashStartMs = Zeebo_TimeMs();
	}
}

static void InitStartupSplash(void)
{
	s_splashTex = CreateRleSplashTexture(ZEEBO_PANDORA_W, ZEEBO_PANDORA_H,
	                                     ZEEBO_PANDORA_RLE, ZEEBO_PANDORA_RLE_RUNS);
	if(s_splashTex){
		s_startupStage  = STARTUP_PANDORA;
		s_splashStartMs = Zeebo_TimeMs();
	}else{
		StartWarningSplash();
	}
}

static void SkipStartupSplash(void)
{
	if(s_startupStage == STARTUP_PANDORA){
		StartWarningSplash();
	}else if(s_startupStage == STARTUP_WARNING){
		FreeStartupSplash();
	}
}

static bool TickStartupSplash(void)
{
	if(s_startupStage == STARTUP_NONE) return false;

	uint32_t prevBtns = s_hidBtnState;
	CPad::GetPad(0)->AffectFromZeebo();
	if((s_hidBtnState & ~prevBtns) != 0){
		SkipStartupSplash();
		return (s_startupStage != STARTUP_NONE);
	}

	uint32_t totalMs   = (s_startupStage == STARTUP_PANDORA) ? 2800u : 4200u;
	uint32_t fadeOutMs = 500u;
	uint32_t elapsed   = Zeebo_TimeMs() - s_splashStartMs;

	if(elapsed >= totalMs){
		if(s_startupStage == STARTUP_PANDORA){
			StartWarningSplash();
			return (s_startupStage != STARTUP_NONE);
		}
		FreeStartupSplash();
		return false;
	}

	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !s_splashTex){
		FreeStartupSplash();
		return false;
	}

	uint32_t alpha = 255u;
	if(elapsed < 400u){
		alpha = (elapsed * 255u) / 400u;
	}else if(elapsed > (totalMs - fadeOutMs)){
		uint32_t rem = totalMs - elapsed;
		alpha = (rem * 255u) / fadeOutMs;
	}
	if(alpha > 255u) alpha = 255u;
	GLfixed fxAlpha = (GLfixed)((alpha * 65536u) / 255u);

	gl->glDisable(GL_DEPTH_TEST);
	gl->glDisable(GL_CULL_FACE);
	gl->glDisable(GL_FOG);
	gl->glDisable(GL_ALPHA_TEST);
	gl->glDisable(GL_BLEND);
	gl->glClearColorx(0, 0, 0, 65536);
	gl->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	gl->glEnable(GL_TEXTURE_2D);
	gl->glBindTexture(GL_TEXTURE_2D, s_splashTex);
	rw::gles1::notifyTextureBound(s_splashTex);

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPushMatrix();
	gl->glLoadIdentity();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPushMatrix();
	gl->glLoadIdentity();

	rw::gles1::setLightingState(false, false, false);
	rw::gles1::setClientStates(true, false, false, true);
	gl->glColor4x(fxAlpha, fxAlpha, fxAlpha, 65536);

	const float uMax = 640.0f / 1024.0f;
	const float vMax = 480.0f / 512.0f;
	const float verts[8] = { -1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f };
	const float uvs[8]   = {  0.0f, 0.0f, uMax, 0.0f,  0.0f, vMax, uMax,  vMax };

	gl->glVertexPointer(2, GL_FLOAT, 0, verts);
	gl->glTexCoordPointer(2, GL_FLOAT, 0, uvs);
	gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPopMatrix();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPopMatrix();

	if(!rw::gles1::isTextureEnabled()){
		gl->glDisable(GL_TEXTURE_2D);
	}
	psCameraShowRaster(Scene.camera);
	return true;
}

// ---------------------------------------------------------------------------
// BREW Applet Lifecycle Entry Points (called from zeebo_brew.cpp)
// ---------------------------------------------------------------------------
extern "C" boolean Zeebo_OnAppStart(void)
{
	Zeebo_Log("[GTA3] Zeebo_OnAppStart\n");
	if(RsInitialize()){
		Zeebo_Log("[GTA3] RsInitialize ok, calling rsINITIALIZE\n");
		if(!RsEventHandler(rsINITIALIZE, nil)){
			Zeebo_Log("[GTA3] rsINITIALIZE failed\n");
			return FALSE;
		}
		rw::EngineOpenParams openParams;
		openParams.width       = RsGlobal.width;
		openParams.height      = RsGlobal.height;
		openParams.windowtitle = RsGlobal.appName;

		Zeebo_Log("[GTA3] calling rsRWINITIALIZE\n");
		if(!RsEventHandler(rsRWINITIALIZE, &openParams)){
			Zeebo_Log("[GTA3] rsRWINITIALIZE failed\n");
			return FALSE;
		}
		s_rwInitialized = TRUE;
		Zeebo_Log("[GTA3] calling rsSELECTDEVICE\n");
		RsEventHandler(rsSELECTDEVICE, nil);
		InitStartupSplash();
		Zeebo_Log("[GTA3] Zeebo_OnAppStart done!\n");
		return TRUE;
	}
	return FALSE;
}

#ifdef AEE_SIMULATOR
extern "C" void Zeebo_SimFileLog(const char* fmt, ...);
#endif

extern "C" void Zeebo_OnFrameTick(void)
{
	if(!s_rwInitialized || RsGlobal.quit){
		RsGlobal.quit = TRUE;
		HandleExit();
		return;
	}

	if(TickStartupSplash()){
		return;
	}

	if(RsGlobal.width <= 0 || RsGlobal.height <= 0){
		RsGlobal.width         = 640;
		RsGlobal.height        = 480;
		RsGlobal.maximumWidth  = 640;
		RsGlobal.maximumHeight = 480;
	}
	static RwUInt32 s_lastLoggedState = 0xFFFFFFFFu;
	if(gGameState != s_lastLoggedState){
		s_lastLoggedState = gGameState;
		Zeebo_Log("[GTA3] gGameState -> %u\n", (unsigned)gGameState);
	}
	switch(gGameState){
	case GS_START_UP:
		gGameState = GS_INIT_LOGO_MPEG;
		break;

	case GS_INIT_LOGO_MPEG:
		if(Zeebo_MovieStart("movies/Logo.mpg")){
			gGameState = GS_LOGO_MPEG;
		}else{
			gGameState = GS_INIT_ONCE;
		}
		break;

	case GS_LOGO_MPEG:
	{
		uint32_t prevBtns = s_hidBtnState;
		CPad::GetPad(0)->AffectFromZeebo();
		if((s_hidBtnState & ~prevBtns) != 0){
			Zeebo_MovieSkip();
		}
		if(!Zeebo_MovieTick()){
			gGameState = GS_INIT_ONCE;
		}else{
			psCameraShowRaster(Scene.camera);
		}
		break;
	}

	case GS_INIT_ONCE:
		LoadingScreen(nil, nil, "loadsc0");
		if(!CGame::InitialiseOnceAfterRW()){
			RsGlobal.quit = TRUE;
		}else{
			gGameState = GS_INIT_INTRO_MPEG;
		}
		break;

	case GS_INIT_INTRO_MPEG:
		if(Zeebo_MovieStart("movies/GTAtitles.mpg")){
			gGameState = GS_INTRO_MPEG;
		}else{
			gGameState = GS_INIT_FRONTEND;
		}
		break;

	case GS_INTRO_MPEG:
	{
		uint32_t prevBtns = s_hidBtnState;
		CPad::GetPad(0)->AffectFromZeebo();
		if((s_hidBtnState & ~prevBtns) != 0){
			Zeebo_MovieSkip();
		}
		if(!Zeebo_MovieTick()){
			gGameState = GS_INIT_FRONTEND;
		}else{
			psCameraShowRaster(Scene.camera);
		}
		break;
	}

	case GS_INIT_FRONTEND:
		LoadingScreen(nil, nil, "loadsc0");
		FrontEndMenuManager.m_bGameNotLoaded = true;
		CMenuManager::m_bStartUpFrontEndRequested = true;
		gGameState = GS_FRONTEND;
		break;

	case GS_FRONTEND:
	{
		static bool s_feLogged = false;
		if(!s_feLogged){
			s_feLogged = true;
			Zeebo_Log("[GTA3] GS_FRONTEND w=%d h=%d ar=%.3f\n",
			          (int)RsGlobal.width, (int)RsGlobal.height, (double)CDraw::GetAspectRatio());
		}
		RsEventHandler(rsFRONTENDIDLE, nil);
		static int s_afterFe = 0;
		if(s_afterFe < 3){
			++s_afterFe;
			Zeebo_Log("[GTA3] after rsFRONTENDIDLE #%d w=%d h=%d\n", s_afterFe, (int)RsGlobal.width, (int)RsGlobal.height);
		}
		if(!FrontEndMenuManager.m_bMenuActive || FrontEndMenuManager.m_bWantToLoad){
			gGameState = GS_INIT_PLAYING_GAME;
		}
		if(FrontEndMenuManager.m_bWantToLoad){
			InitialiseGame();
			FrontEndMenuManager.m_bGameNotLoaded = false;
			gGameState = GS_PLAYING_GAME;
		}
		break;
	}

	case GS_INIT_PLAYING_GAME:
		InitialiseGame();
		FrontEndMenuManager.m_bGameNotLoaded = false;
		gGameState = GS_PLAYING_GAME;
		break;

	case GS_PLAYING_GAME:
#ifdef AEE_SIMULATOR
	{
		static int s_lastActive = -1;
		static int s_lastScreen = -1;
		if ((int)FrontEndMenuManager.m_bMenuActive != s_lastActive || FrontEndMenuManager.m_nCurrScreen != s_lastScreen) {
			s_lastActive = (int)FrontEndMenuManager.m_bMenuActive;
			s_lastScreen = FrontEndMenuManager.m_nCurrScreen;
			Zeebo_SimFileLog("[MENU] active=%d screen=%d option=%d\n", s_lastActive, s_lastScreen, FrontEndMenuManager.m_nCurrOption);
		}
	}
#endif
		RsEventHandler(rsIDLE, (void*)TRUE);
		break;

	default:
		break;
	}

	if(FrontEndMenuManager.m_bWantToRestart){
		FrontEndMenuManager.UnloadTextures();
		CPad::ResetCheats();
		CPad::StopPadsShaking();
		DMAudio.ChangeMusicMode(MUSICMODE_DISABLE);

		if(FrontEndMenuManager.m_bWantToLoad){
			CGame::ShutDownForRestart();
			CGame::InitialiseWhenRestarting();
			DMAudio.ChangeMusicMode(MUSICMODE_GAME);
			LoadSplash(GetLevelSplashScreen(CGame::currLevel));
			FrontEndMenuManager.m_bWantToLoad = false;
		}else{
			if(gGameState == GS_PLAYING_GAME)
				CGame::ShutDown();
			CTimer::Stop();
			if(FrontEndMenuManager.m_bFirstTime == true){
				gGameState = GS_INIT_FRONTEND;
			}else{
				gGameState = GS_INIT_PLAYING_GAME;
			}
		}
		FrontEndMenuManager.m_bFirstTime = false;
		FrontEndMenuManager.m_bWantToRestart = false;
	}
}

extern "C" void Zeebo_OnAppStop(void)
{
	FreeStartupSplash();
	Zeebo_MovieStop();
	if(s_rwInitialized){
		if(gGameState == GS_PLAYING_GAME)
			CGame::ShutDown();
		DMAudio.Terminate();
		RsEventHandler(rsRWTERMINATE, nil);
		RsEventHandler(rsTERMINATE, nil);
		s_rwInitialized = FALSE;
	}
	RsTerminate();
}

extern "C" boolean Zeebo_OnKeyEvent(AEEEvent evt, uint16_t keyCode, uint32_t dwParam)
{
	(void)evt;
	(void)keyCode;
	(void)dwParam;
	return FALSE;
}

extern "C" void Zeebo_PurgeAudioCache(void);
extern "C" uint32_t g_zeeboMemPurges = 0;

extern "C" int Zeebo_PurgeMemoryForAlloc(size_t needed)
{
	(void)needed;
	static bool s_inPurge = false;
	if(s_inPurge) return 0;
	s_inPurge = true;
	g_zeeboMemPurges++;

	Zeebo_PurgeAudioCache();
	if(s_rwInitialized && gGameState == GS_PLAYING_GAME){
		for(int i = 0; i < 12; i++){
			if(!CStreaming::RemoveLeastUsedModel()){
				break;
			}
		}
	}

	s_inPurge = false;
	return 1;
}

#endif // __ZEEBO__
