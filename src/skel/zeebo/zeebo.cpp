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

#include "../../platform/zeebo/zeebo_brew.h"
#include "../../platform/zeebo/zeebo_movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long _dwOperatingSystemVersion = 0;
size_t _dwMemAvailPhys = 64 * 1024 * 1024;
RwUInt32 gGameState = 0;

const char *_psGetUserFilesFolder(void)
{
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

RwMemoryFunctions *psGetMemoryFunctions(void)
{
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
		if(btn.dwID < 32){
			if(btn.dwState) s_hidBtnState |=  (1u << btn.dwID);
			else            s_hidBtnState &= ~(1u << btn.dwID);
		}
	}
	s_prevHidBtn = s_hidBtnState;

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
	PCTempJoyState.Start          = (s_hidBtnState & (1u << 11)) ? 255 : 0; // start
	PCTempJoyState.DPadUp         = (s_hidBtnState & (1u << 12)) ? 255 : 0; // D-Pad UP
	PCTempJoyState.DPadLeft       = (s_hidBtnState & (1u << 13)) ? 255 : 0; // D-Pad LEFT
	PCTempJoyState.DPadDown       = (s_hidBtnState & (1u << 14)) ? 255 : 0; // D-Pad DOWN
	PCTempJoyState.DPadRight      = (s_hidBtnState & (1u << 15)) ? 255 : 0; // D-Pad RIGHT

	// Read analog stick axes (AEEHIDPositionInfo: 25 words; X=slot 1, Y=slot 2, center=128)
	int32_t posInfo[25];
	for(int i = 0; i < 25; i++) posInfo[i] = 128;
	if(s_hidDevice->pvt->GetPositionState(s_hidDevice, posInfo) == AEE_SUCCESS){
		int lx = (int)(posInfo[1] - 128);
		int ly = (int)(posInfo[2] - 128);
		const int kDeadzone = 16;
		if(lx > -kDeadzone && lx < kDeadzone) lx = 0;
		if(ly > -kDeadzone && ly < kDeadzone) ly = 0;
		if(lx < -128) lx = -128; else if(lx > 127) lx = 127;
		if(ly < -128) ly = -128; else if(ly > 127) ly = 127;
		PCTempJoyState.LeftStickX = (int16)lx;
		PCTempJoyState.LeftStickY = (int16)ly;
	}
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
		Zeebo_Log("[GTA3] Zeebo_OnAppStart done!\n");
		return TRUE;
	}
	return FALSE;
}

extern "C" void Zeebo_OnFrameTick(void)
{
	if(!s_rwInitialized || RsGlobal.quit){
		RsGlobal.quit = TRUE;
		HandleExit();
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
		RsEventHandler(rsIDLE, (void*)TRUE);
		break;

	default:
		break;
	}
}

extern "C" void Zeebo_OnAppStop(void)
{
	Zeebo_MovieStop();
	if(s_rwInitialized){
		RsEventHandler(rsRWTERMINATE, nil);
		RsEventHandler(rsTERMINATE, nil);
		s_rwInitialized = FALSE;
	}
	RsTerminate();
}

extern "C" boolean Zeebo_OnKeyEvent(AEEEvent evt, uint16_t keyCode, uint32_t dwParam)
{
	(void)dwParam;
	if(Zeebo_MovieIsPlaying() && (evt == EVT_KEY_PRESS || evt == EVT_KEY)){
		Zeebo_MovieSkip();
		return TRUE;
	}
	int rsKey = MapZeeboKey(keyCode);
	if(rsKey == rsNULL) return FALSE;
	RsKeyStatus ks;
	ks.keyCharCode = rsKey;
	if(evt == EVT_KEY_PRESS || evt == EVT_KEY){
		RsEventHandler(rsKEYDOWN, &ks);
		return TRUE;
	}else if(evt == EVT_KEY_RELEASE){
		RsEventHandler(rsKEYUP, &ks);
		return TRUE;
	}
	return FALSE;
}

#endif // __ZEEBO__
