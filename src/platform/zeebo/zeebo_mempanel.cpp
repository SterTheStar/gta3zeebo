// zeebo_mempanel.cpp - Top-left on-screen memory & performance diagnostics panel for Zeebo.
#ifdef __ZEEBO__

#include "common.h"
#include "Font.h"
#include "Sprite2d.h"
#include "Streaming.h"
#include "Pools.h"
#include "Text.h"

#include "platform/zeebo/zeebo_brew.h"
#include "platform/zeebo/zeebo_mempanel.h"

#include <stdio.h>
#ifdef AEE_SIMULATOR
#include <windows.h>
#endif

extern "C" {
extern uint32_t g_zeeboMemPurges;
}

namespace ZeeboMemPanel {

#ifdef AEE_SIMULATOR
static bool     s_enabled      = true;
#else
static bool     s_enabled      = false;
#endif
static uint32_t s_lastQueryMs  = 0;

// Cached BREW heap stats (refreshed at ~4 Hz to avoid walking the heap every frame)
static bool     s_hasHeapStats = false;
static uint32_t s_heapTotal    = 0;
static uint32_t s_heapFree     = 0;
static uint32_t s_heapUsed     = 0;
static uint32_t s_heapLargest  = 0;
static uint32_t s_heapMinFree  = 0xFFFFFFFFu;
#ifdef AEE_SIMULATOR
static uint32_t s_procPrivBytes = 0;
static uint32_t s_procWorkSet   = 0;
#endif

// Latest profiling metrics fed from Idle()
static uint32_t s_realFps      = 0;
static uint32_t s_frameMs      = 0;
static uint32_t s_drawCalls    = 0;
static uint32_t s_tris         = 0;

void SetEnabled(bool enabled) { s_enabled = enabled; }
bool IsEnabled(void)          { return s_enabled; }
void Toggle(void)             { s_enabled = !s_enabled; }

void UpdateFrameStats(uint32_t realFps, uint32_t frameMs, uint32_t drawCalls, uint32_t tris)
{
	s_realFps   = realFps;
	s_frameMs   = frameMs;
	s_drawCalls = drawCalls;
	s_tris      = tris;
}

static void RefreshHeapStats(void)
{
	uint32_t now = Zeebo_TimeMs();
	if (s_lastQueryMs != 0 && (now - s_lastQueryMs) < 250u) {
		return;
	}
	s_lastQueryMs = now;

	if (g_Zeebo.pHelpers && g_Zeebo.pHelpers->pfnGetRAMFree) {
		uint32_t total = 0;
		uint32_t largest = 0;
		uint32_t freeBytes = g_Zeebo.pHelpers->pfnGetRAMFree(&total, &largest);
		if (total > 0 || freeBytes > 0) {
			s_hasHeapStats = true;
			s_heapTotal    = total;
			s_heapFree     = freeBytes;
			s_heapUsed     = (total >= freeBytes) ? (total - freeBytes) : 0;
			s_heapLargest  = largest;
			if (freeBytes < s_heapMinFree) {
				s_heapMinFree = freeBytes;
			}
		}
	}

#ifdef AEE_SIMULATOR
	typedef struct {
		DWORD  cb;
		DWORD  PageFaultCount;
		SIZE_T PeakWorkingSetSize;
		SIZE_T WorkingSetSize;
		SIZE_T QuotaPeakPagedPoolUsage;
		SIZE_T QuotaPagedPoolUsage;
		SIZE_T QuotaPeakNonPagedPoolUsage;
		SIZE_T QuotaNonPagedPoolUsage;
		SIZE_T PagefileUsage;
		SIZE_T PeakPagefileUsage;
		SIZE_T PrivateUsage;
	} SIM_PMC_EX;
	typedef BOOL (WINAPI *PFN_GetProcessMemoryInfo)(HANDLE, void*, DWORD);
	static PFN_GetProcessMemoryInfo s_pfnGPMI = NULL;
	static bool s_gpmiChecked = false;
	if (!s_gpmiChecked) {
		s_gpmiChecked = true;
		HMODULE hK32 = GetModuleHandleA("kernel32.dll");
		if (hK32) {
			s_pfnGPMI = (PFN_GetProcessMemoryInfo)GetProcAddress(hK32, "K32GetProcessMemoryInfo");
		}
		if (!s_pfnGPMI) {
			HMODULE hPsapi = LoadLibraryA("psapi.dll");
			if (hPsapi) {
				s_pfnGPMI = (PFN_GetProcessMemoryInfo)GetProcAddress(hPsapi, "GetProcessMemoryInfo");
			}
		}
	}
	if (s_pfnGPMI) {
		SIM_PMC_EX pmc;
		memset(&pmc, 0, sizeof(pmc));
		pmc.cb = sizeof(pmc);
		if (s_pfnGPMI(GetCurrentProcess(), &pmc, sizeof(pmc))) {
			s_procPrivBytes = (uint32_t)(pmc.PrivateUsage > 0xFFFFFFFFu ? 0xFFFFFFFFu : pmc.PrivateUsage);
			s_procWorkSet   = (uint32_t)(pmc.WorkingSetSize > 0xFFFFFFFFu ? 0xFFFFFFFFu : pmc.WorkingSetSize);
		}
	}
#endif
}

static void FormatBytes(uint32_t bytes, char *out, size_t outSize)
{
	if (bytes >= 1024u * 1024u) {
		float mb = (float)bytes / (1024.0f * 1024.0f);
		snprintf(out, outSize, "%.1fM", mb);
	} else {
		unsigned kb = (unsigned)(bytes / 1024u);
		snprintf(out, outSize, "%uK", kb);
	}
}

static void PrintLine(float x, float y, const char *text, const CRGBA &col)
{
	wchar ustr[128];
	AsciiToUnicode(text, ustr);
	CFont::SetColor(col);
	CFont::PrintString(x, y, ustr);
}

void Draw(void)
{
	if (!s_enabled) {
		return;
	}

	RefreshHeapStats();

	const float padX   = 8.0f;
	const float padY   = 8.0f;
	const float panelW = 280.0f;
	const float lineH  = 15.0f;
	const float barH   = 6.5f;
#ifdef AEE_SIMULATOR
	const float numLines = 7.0f;
#else
	const float numLines = 6.0f;
#endif
	const float panelH = 14.0f + barH + numLines * lineH + 8.0f;

	const float x0 = padX;
	const float y0 = padY;
	const float x1 = x0 + panelW;
	const float y1 = y0 + panelH;

	// Choose accent / status color based on heap pressure
	uint32_t usedPct = 0;
	if (s_hasHeapStats && s_heapTotal > 0) {
		usedPct = (uint32_t)(((uint64_t)s_heapUsed * 100u) / s_heapTotal);
		if (usedPct > 100u) usedPct = 100u;
	}

	CRGBA statusCol(90, 235, 110, 255); // Green: healthy
	if (s_hasHeapStats) {
		if (usedPct >= 85u || s_heapFree < 2u * 1024u * 1024u) {
			statusCol = CRGBA(255, 75, 75, 255);   // Red: critical
		} else if (usedPct >= 70u || s_heapFree < 4u * 1024u * 1024u) {
			statusCol = CRGBA(255, 210, 60, 255);  // Yellow: warning
		}
	}

	// Translucent dark background
	CSprite2d::DrawRect(CRect(x0, y0, x1, y1), CRGBA(8, 10, 14, 180));

	// Heap usage progress bar
	const float bx0 = x0 + 8.0f;
	const float bx1 = x1 - 8.0f;
	const float by0 = y0 + 7.0f;
	const float by1 = by0 + barH;
	CSprite2d::DrawRect(CRect(bx0, by0, bx1, by1), CRGBA(45, 50, 60, 220));
	if (s_hasHeapStats && s_heapTotal > 0) {
		float frac = (float)s_heapUsed / (float)s_heapTotal;
		if (frac < 0.0f) frac = 0.0f;
		if (frac > 1.0f) frac = 1.0f;
		CSprite2d::DrawRect(CRect(bx0, by0, bx0 + (bx1 - bx0) * frac, by1), statusCol);
	}

	// Configure compact crisp font
	CFont::SetPropOff();
	CFont::SetBackgroundOff();
	CFont::SetFontStyle(FONT_BANK);
	CFont::SetCentreOff();
	CFont::SetRightJustifyOff();
	CFont::SetJustifyOff();
	CFont::SetBackGroundOnlyTextOff();
	CFont::SetWrapx(SCREEN_WIDTH);
	CFont::SetScale(0.47f, 0.73f);
	CFont::SetDropShadowPosition(1);
	CFont::SetDropColor(CRGBA(0, 0, 0, 220));

	const float tx = x0 + 8.0f;
	float ty = by1 + 5.0f;
	char line[96];

	const CRGBA colWhite(235, 240, 245, 255);
	const CRGBA colCyan(120, 215, 255, 255);
	const CRGBA colDim(185, 195, 205, 255);

	// Line 1: Heap Used / Total
	if (s_hasHeapStats) {
		char usedBuf[16], totBuf[16];
		FormatBytes(s_heapUsed, usedBuf, sizeof(usedBuf));
		FormatBytes(s_heapTotal, totBuf, sizeof(totBuf));
		snprintf(line, sizeof(line), "HEAP %s/%s (%u)", usedBuf, totBuf, (unsigned)usedPct);
		PrintLine(tx, ty, line, statusCol);
	} else {
		PrintLine(tx, ty, "HEAP N/A", colDim);
	}
	ty += lineH;

	// Line 2: Free RAM + Largest Contiguous Block
	if (s_hasHeapStats) {
		char freeBuf[16], maxBuf[16];
		FormatBytes(s_heapFree, freeBuf, sizeof(freeBuf));
		FormatBytes(s_heapLargest, maxBuf, sizeof(maxBuf));
		snprintf(line, sizeof(line), "FREE %s  MAX %s", freeBuf, maxBuf);
		PrintLine(tx, ty, line, colWhite);
	} else {
		PrintLine(tx, ty, "FREE N/A  MAX N/A", colDim);
	}
	ty += lineH;

	// Line 3: Lowest Free RAM seen + OOM Purge count
	if (s_hasHeapStats && s_heapMinFree != 0xFFFFFFFFu) {
		char lowBuf[16];
		FormatBytes(s_heapMinFree, lowBuf, sizeof(lowBuf));
		snprintf(line, sizeof(line), "LOW  %s  PRG %u", lowBuf, (unsigned)g_zeeboMemPurges);
		PrintLine(tx, ty, line, g_zeeboMemPurges > 0 ? CRGBA(255, 180, 70, 255) : colDim);
	} else {
		snprintf(line, sizeof(line), "LOW  N/A  PRG %u", (unsigned)g_zeeboMemPurges);
		PrintLine(tx, ty, line, colDim);
	}
	ty += lineH;

	// Line 4: Streaming Memory Used / Limit
	{
		char stUsed[16], stAvail[16];
		FormatBytes((uint32_t)CStreaming::ms_memoryUsed, stUsed, sizeof(stUsed));
		FormatBytes((uint32_t)CStreaming::ms_memoryAvailable, stAvail, sizeof(stAvail));
		snprintf(line, sizeof(line), "STRM %s/%s", stUsed, stAvail);
		PrintLine(tx, ty, line, colCyan);
	}
	ty += lineH;

	// Line 5: Real FPS / Frame ms / Draw Calls / Triangles
	{
		float kTris = (float)s_tris / 1000.0f;
		snprintf(line, sizeof(line), "FPS %u (%ums) DC %u %.1fK",
			(unsigned)s_realFps, (unsigned)s_frameMs, (unsigned)s_drawCalls, kTris);
		PrintLine(tx, ty, line, colWhite);
	}
	ty += lineH;

	// Line 6: Active Entity Pools (Vehicles / Peds / Objects)
	{
		int nVeh = CPools::GetVehiclePool() ? CPools::GetVehiclePool()->GetNoOfUsedSpaces() : 0;
		int nPed = CPools::GetPedPool()     ? CPools::GetPedPool()->GetNoOfUsedSpaces()     : 0;
		int nObj = CPools::GetObjectPool()  ? CPools::GetObjectPool()->GetNoOfUsedSpaces()  : 0;
		snprintf(line, sizeof(line), "VEH %d  PED %d  OBJ %d", nVeh, nPed, nObj);
		PrintLine(tx, ty, line, colDim);
	}
#ifdef AEE_SIMULATOR
	ty += lineH;
	{
		char privBuf[16], wsBuf[16];
		FormatBytes(s_procPrivBytes, privBuf, sizeof(privBuf));
		FormatBytes(s_procWorkSet, wsBuf, sizeof(wsBuf));
		snprintf(line, sizeof(line), "SIM  %s  WS %s", privBuf, wsBuf);
		PrintLine(tx, ty, line, colDim);
	}
#endif
}

} // namespace ZeeboMemPanel

#endif // __ZEEBO__
