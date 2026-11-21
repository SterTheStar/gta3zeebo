// sampman_zeebo.cpp - GTA III (re3) audio backend faithful to Qualcomm BREW 4.0.2 SDK (AEEIMedia.h / AEEMediaUtil.h).
// Supports:
//   - All SFX banks & ped comments from audio/sfx.SDT + audio/SFX.RAW (in-memory WAV MMD_BUFFER + LRU PCM cache)
//   - All streamed audio: radio stations, music, cutscenes, police radio (MMD_FILE_NAME, with MMD_BUFFER fallback on simulator)
//   - Official IMedia async state machine via IMedia_RegisterNotify, GetState(pbStateChanging), MM_PARM_VOLUME (0..AEE_MAX_VOLUME),
//     MM_PARM_PAN (0..MM_MAX_PAN), MM_PARM_POS, and EVT_APP_SUSPEND cleanup.
#include "common.h"
#ifdef __ZEEBO__

#include "sampman.h"
#include "AudioManager.h"
#include "MusicManager.h"
#include "Timer.h"
#include "platform/zeebo/zeebo_brew.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cSampleManager SampleManager;
bool8 _bSampmanInitialised = FALSE;

uint32 BankStartOffset[MAX_SFX_BANKS];
uint32 nNumMP3s = 0;

namespace {

static FILE* s_fpSfxRaw = NULL;
static int32 s_sfxRawSize = 0;

// LRU cache for raw PCM chunks read from SFX.RAW to minimize disk I/O
#define ZEEBO_SFX_CACHE_SLOTS       32
#define ZEEBO_SFX_CACHE_MAX_BYTES   (1 * 1024 * 1024)
#define ZEEBO_MAX_SINGLE_SFX_BYTES  (384 * 1024)
#define ZEEBO_MAX_ACTIVE_SFX_VOICES 12
#define ZEEBO_SIM_MAX_STREAM_BYTES  (16 * 1024 * 1024)

struct SfxCacheEntry {
	uint32 sfxId;
	uint32 size;
	uint32 lastUsed;
	uint8* pcm;
};

static SfxCacheEntry s_sfxCache[ZEEBO_SFX_CACHE_SLOTS];
static uint32 s_sfxCacheBytes = 0;
static uint32 s_sfxCacheTick  = 1;

static void ClearSfxCache(void)
{
	for (int i = 0; i < ZEEBO_SFX_CACHE_SLOTS; i++) {
		if (s_sfxCache[i].pcm) {
			free(s_sfxCache[i].pcm);
			s_sfxCache[i].pcm = NULL;
		}
		s_sfxCache[i].sfxId = 0xFFFFFFFFu;
		s_sfxCache[i].size = 0;
		s_sfxCache[i].lastUsed = 0;
	}
	s_sfxCacheBytes = 0;
}

static const uint8* GetSfxPcm(uint32 sfxId, uint32 offset, uint32 size)
{
	if (!s_fpSfxRaw || size == 0 || size > ZEEBO_MAX_SINGLE_SFX_BYTES)
		return NULL;
	if (s_sfxRawSize > 0 && (int32)(offset + size) > s_sfxRawSize)
		return NULL;

	for (int i = 0; i < ZEEBO_SFX_CACHE_SLOTS; i++) {
		if (s_sfxCache[i].pcm && s_sfxCache[i].sfxId == sfxId && s_sfxCache[i].size == size) {
			s_sfxCache[i].lastUsed = ++s_sfxCacheTick;
			return s_sfxCache[i].pcm;
		}
	}

	while (s_sfxCacheBytes + size > ZEEBO_SFX_CACHE_MAX_BYTES) {
		int oldest = -1;
		uint32 oldestTick = 0xFFFFFFFFu;
		for (int i = 0; i < ZEEBO_SFX_CACHE_SLOTS; i++) {
			if (s_sfxCache[i].pcm && s_sfxCache[i].lastUsed < oldestTick) {
				oldestTick = s_sfxCache[i].lastUsed;
				oldest = i;
			}
		}
		if (oldest < 0) break;
		s_sfxCacheBytes -= s_sfxCache[oldest].size;
		free(s_sfxCache[oldest].pcm);
		s_sfxCache[oldest].pcm = NULL;
		s_sfxCache[oldest].sfxId = 0xFFFFFFFFu;
		s_sfxCache[oldest].size = 0;
	}

	int slot = -1;
	uint32 oldestTick = 0xFFFFFFFFu;
	for (int i = 0; i < ZEEBO_SFX_CACHE_SLOTS; i++) {
		if (!s_sfxCache[i].pcm) {
			slot = i;
			break;
		}
		if (s_sfxCache[i].lastUsed < oldestTick) {
			oldestTick = s_sfxCache[i].lastUsed;
			slot = i;
		}
	}
	if (slot < 0) return NULL;

	if (s_sfxCache[slot].pcm) {
		s_sfxCacheBytes -= s_sfxCache[slot].size;
		free(s_sfxCache[slot].pcm);
		s_sfxCache[slot].pcm = NULL;
		s_sfxCache[slot].size = 0;
	}

	uint8* buf = (uint8*)malloc(size);
	if (!buf) return NULL;

	if (fseek(s_fpSfxRaw, (long)offset, SEEK_SET) != 0 || fread(buf, 1, size, s_fpSfxRaw) != size) {
		free(buf);
		return NULL;
	}

	s_sfxCache[slot].sfxId = sfxId;
	s_sfxCache[slot].size = size;
	s_sfxCache[slot].lastUsed = ++s_sfxCacheTick;
	s_sfxCache[slot].pcm = buf;
	s_sfxCacheBytes += size;
	return buf;
}

static void WriteWavHeader(uint8* dst, uint32 pcmBytes, uint32 sampleRate, uint16 channels, uint16 bitsPerSample)
{
	uint32 byteRate = sampleRate * channels * (bitsPerSample / 8);
	uint16 blockAlign = (uint16)(channels * (bitsPerSample / 8));
	uint32 chunkSize = 36u + pcmBytes;

	memcpy(dst + 0, "RIFF", 4);
	memcpy(dst + 4, &chunkSize, 4);
	memcpy(dst + 8, "WAVE", 4);
	memcpy(dst + 12, "fmt ", 4);
	uint32 subchunk1 = 16;
	uint16 audioFmt = 1; // PCM
	memcpy(dst + 16, &subchunk1, 4);
	memcpy(dst + 20, &audioFmt, 2);
	memcpy(dst + 22, &channels, 2);
	memcpy(dst + 24, &sampleRate, 4);
	memcpy(dst + 28, &byteRate, 4);
	memcpy(dst + 32, &blockAlign, 2);
	memcpy(dst + 34, &bitsPerSample, 2);
	memcpy(dst + 36, "data", 4);
	memcpy(dst + 40, &pcmBytes, 4);
}

// ---------------------------------------------------------------------------
// BREW 4.0.2 IMedia state machine wrapper (RegisterNotify + GetState)
// ---------------------------------------------------------------------------
enum BrewPlayState {
	BPS_IDLE = 0,
	BPS_STARTING,
	BPS_PLAYING,
	BPS_PAUSED,
	BPS_DONE,
	BPS_ABORTED
};

struct BrewMediaHandle {
	IMedia*       media;
	BrewPlayState state;
	uint32        cmdMs;
	uint32        startMs;
	uint32        startPosMs;
	uint32        pausedPosMs;
	int32         lengthMs;
	uint32        pendingSeekMs;
	bool          seekDone;
	bool          looping;
	int           lastBrewVol;
	int           lastBrewPan;
};

static void OnBrewMediaNotify(void* pUser, AEEMediaCmdNotify* pn)
{
	BrewMediaHandle* mh = (BrewMediaHandle*)pUser;
	if (!mh || !pn || !mh->media || pn->pIMedia != mh->media)
		return;

	switch (pn->nCmd) {
	case MM_CMD_PLAY:
		switch (pn->nStatus) {
		case MM_STATUS_START:
			mh->state = BPS_PLAYING;
			mh->startMs = Zeebo_TimeMs();
			if (mh->pendingSeekMs > 0 && !mh->seekDone && mh->media->pvt) {
				mh->seekDone = true;
				mh->media->pvt->Seek(mh->media, MM_SEEK_START, (int32_t)mh->pendingSeekMs);
			}
			break;
		case MM_STATUS_REPEAT:
			mh->state = BPS_PLAYING;
			mh->startPosMs = 0;
			mh->startMs = Zeebo_TimeMs();
			break;
		case MM_STATUS_PAUSE:
			mh->state = BPS_PAUSED;
			break;
		case MM_STATUS_RESUME:
			mh->state = BPS_PLAYING;
			mh->startMs = Zeebo_TimeMs();
			break;
		case MM_STATUS_DONE:
			mh->state = BPS_DONE;
			break;
		case MM_STATUS_ABORT:
			mh->state = BPS_ABORTED;
			break;
		default:
			break;
		}
		break;

	case MM_CMD_GETTOTALTIME:
		if (pn->nStatus == MM_STATUS_DONE && pn->pCmdData) {
			int32 t = (int32)(uintptr_t)pn->pCmdData;
			if (t > 0) mh->lengthMs = t;
		}
		break;

	default:
		break;
	}
}

static void BrewMediaReset(BrewMediaHandle& mh)
{
	mh.media = NULL;
	mh.state = BPS_IDLE;
	mh.cmdMs = 0;
	mh.startMs = 0;
	mh.startPosMs = 0;
	mh.pausedPosMs = 0;
	mh.lengthMs = 0;
	mh.pendingSeekMs = 0;
	mh.seekDone = false;
	mh.looping = false;
	mh.lastBrewVol = -1;
	mh.lastBrewPan = -1;
}

static void BrewMediaRelease(BrewMediaHandle& mh)
{
	IMedia* m = mh.media;
	if (m && m->pvt) {
		// Always clear notification callback before stopping/releasing (prevents stale callbacks)
		m->pvt->RegisterNotify(m, NULL, NULL);
		boolean changing = FALSE;
		int st = m->pvt->GetState(m, &changing);
		if (st == MM_STATE_PLAY || st == MM_STATE_PLAY_PAUSE || changing) {
			m->pvt->Stop(m);
		}
		m->pvt->Release(m);
	}
	BrewMediaReset(mh);
}

static bool BrewMediaIsActive(BrewMediaHandle& mh)
{
	IMedia* m = mh.media;
	if (!m || !m->pvt) return false;

	if (mh.state == BPS_DONE || mh.state == BPS_ABORTED)
		return false;
	if (mh.state == BPS_PAUSED)
		return true;

	boolean changing = FALSE;
	int st = m->pvt->GetState(m, &changing);
	if (st == MM_STATE_PLAY || st == MM_STATE_PLAY_PAUSE || changing)
		return true;

	// Async startup grace window: IMedia_Play() may still be transitioning from READY -> PLAY
	if (mh.state == BPS_STARTING) {
		uint32 elapsed = Zeebo_TimeMs() - mh.cmdMs;
		if (elapsed < 250u)
			return true;
	}

#ifdef AEE_SIMULATOR
	// Win32 simulator IMedia may not transition GetState() to MM_STATE_PLAY for some buffer/file types,
	// so keep it alive within its known duration unless MM_STATUS_DONE / MM_STATUS_ABORT arrived.
	if (mh.looping)
		return true;
	if (mh.lengthMs > 0) {
		int32 pos = (int32)(Zeebo_TimeMs() - mh.startMs) + (int32)mh.startPosMs;
		if (pos < mh.lengthMs)
			return true;
	}
#endif
	return false;
}

static int ClampBrewVol(int v)
{
	if (v < 0) return 0;
	if (v > AEE_MAX_VOLUME) return AEE_MAX_VOLUME;
	return v;
}

static int GtaPanToBrewPan(uint8 gtaPan)
{
	// GTA III pan: 0 (left) .. 63/64 (center) .. 127 (right) -> BREW 0..MM_MAX_PAN (128)
	int p = ((int)gtaPan * MM_MAX_PAN) / 127;
	if (p < 0) p = 0;
	if (p > MM_MAX_PAN) p = MM_MAX_PAN;
	return p;
}

// ---------------------------------------------------------------------------
// SFX voice channels
// ---------------------------------------------------------------------------
struct ZeeboSfxChannel {
	BrewMediaHandle mh;
	uint8*          wavBuf;
	uint32          wavCap;
	uint32          sfxId;
	uint32          freq;
	uint32          volume;      // 0..127
	uint32          pan;         // 0..127
	uint32          loopCount;   // 0 = infinite loop, 1 = once
	float           posX, posY, posZ;
	float           minDist, maxDist;
	bool            is3D;
	bool            inited;
	uint32          startTick;
};

static ZeeboSfxChannel s_channels[NUM_CHANNELS];
static uint32 s_voiceTick = 1;

static void ReleaseChannelMedia(ZeeboSfxChannel& ch)
{
	BrewMediaRelease(ch.mh);
	if (ch.wavBuf && ch.wavCap > 32768u) {
		free(ch.wavBuf);
		ch.wavBuf = NULL;
		ch.wavCap = 0;
	}
}

static int ComputeChannelBrewVol(const ZeeboSfxChannel& ch, uint8 masterVol, uint8 fadeVol)
{
	uint32 scaled = ((uint32)fadeVol * (uint32)ch.volume * (uint32)masterVol) >> 14;
	if (scaled > MAX_VOLUME) scaled = MAX_VOLUME;
	float v = (float)scaled / (float)MAX_VOLUME;

	if (ch.is3D && ch.maxDist > 0.01f) {
		float dist = sqrtf(ch.posX * ch.posX + ch.posY * ch.posY + ch.posZ * ch.posZ);
		float refDist = (ch.minDist > 0.5f) ? ch.minDist : 0.5f;
		float maxDist = (ch.maxDist > refDist + 0.1f) ? ch.maxDist : (refDist + 0.1f);
		if (dist >= maxDist) {
			v = 0.0f;
		} else if (dist > refDist) {
			// Match OpenAL AL_INVERSE_DISTANCE_CLAMPED over the main range, then taper smoothly in the last 25%
			float gain = refDist / dist;
			float fadeStart = refDist + (maxDist - refDist) * 0.75f;
			if (dist > fadeStart) {
				float t = (maxDist - dist) / (maxDist - fadeStart);
				if (t < 0.0f) t = 0.0f;
				gain *= t;
			}
			v *= gain;
		}
	}

	return ClampBrewVol((int)(v * (float)AEE_MAX_VOLUME + 0.5f));
}

static int ComputeChannelBrewPan(const ZeeboSfxChannel& ch)
{
	if (!ch.is3D) {
		return GtaPanToBrewPan((uint8)ch.pan);
	}
	float dist = sqrtf(ch.posX * ch.posX + ch.posY * ch.posY + ch.posZ * ch.posZ);
	if (dist < 0.001f) {
		return MM_MAX_PAN / 2;
	}
	// AudioManager passes camera-space coords where -fX is right (matches sampman_oal / sampman_miles)
	float normX = -ch.posX / dist;
	float refDist = (ch.minDist > 0.5f) ? ch.minDist : 0.5f;
	if (dist < refDist) {
		normX *= (dist / refDist);
	}
	int bp = (MM_MAX_PAN / 2) + (int)(normX * 48.0f + (normX >= 0.0f ? 0.5f : -0.5f));
	if (bp < 0) bp = 0;
	if (bp > MM_MAX_PAN) bp = MM_MAX_PAN;
	return bp;
}

static void ApplyChannelBrewParams(ZeeboSfxChannel& ch, uint8 masterVol, uint8 fadeVol)
{
	if (!ch.mh.media || !ch.mh.media->pvt) return;
	int bv = ComputeChannelBrewVol(ch, masterVol, fadeVol);
	if (bv != ch.mh.lastBrewVol) {
		ch.mh.lastBrewVol = bv;
		ch.mh.media->pvt->SetMediaParm(ch.mh.media, MM_PARM_VOLUME, bv, 0);
	}
	int bp = ComputeChannelBrewPan(ch);
	if (bp != ch.mh.lastBrewPan) {
		ch.mh.lastBrewPan = bp;
		ch.mh.media->pvt->SetMediaParm(ch.mh.media, MM_PARM_PAN, bp, 0);
	}
}

static void ReapFinishedAndLimitVoices(uint8 masterVol, uint8 fadeVol)
{
	int activeCount = 0;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (s_channels[i].mh.media) {
			if (!BrewMediaIsActive(s_channels[i].mh)) {
				ReleaseChannelMedia(s_channels[i]);
			} else {
				activeCount++;
			}
		}
	}
	while (activeCount >= ZEEBO_MAX_ACTIVE_SFX_VOICES) {
		int victim = -1;
		int lowestScore = 100000;
		uint32 oldestTick = 0xFFFFFFFFu;
		for (int i = 0; i < NUM_CHANNELS; i++) {
			if (s_channels[i].mh.media) {
				int bv = ComputeChannelBrewVol(s_channels[i], masterVol, fadeVol);
				int score = bv + (s_channels[i].is3D ? 0 : 10000) + ((s_channels[i].loopCount != 0) ? 200 : 0);
				if (score < lowestScore || (score == lowestScore && s_channels[i].startTick < oldestTick)) {
					lowestScore = score;
					oldestTick = s_channels[i].startTick;
					victim = i;
				}
			}
		}
		if (victim < 0) break;
		ReleaseChannelMedia(s_channels[victim]);
		activeCount--;
	}
}

// ---------------------------------------------------------------------------
// Streamed audio (radio, music, cutscenes, police radio)
// ---------------------------------------------------------------------------
struct ZeeboStream {
	BrewMediaHandle mh;
	uint8           fileId;
	uint8           preloadedFileId;
	uint8           volume;     // 0..127
	uint8           pan;        // 0..127
	uint8           effectFlag;
	bool            preloaded;
	char            path[260];
#ifdef AEE_SIMULATOR
	uint8*          simBuf;
	uint32          simBufSize;
#endif
};

static ZeeboStream s_streams[MAX_STREAMS];
static int32 s_streamLengths[TOTAL_STREAMED_SOUNDS];

static bool TestFileExists(const char* path)
{
	if (!path || !*path) return false;
	if (g_Zeebo.pFileMgr && g_Zeebo.pFileMgr->pvt &&
	    g_Zeebo.pFileMgr->pvt->Test(g_Zeebo.pFileMgr, path) == AEE_SUCCESS)
		return true;
#ifdef AEE_SIMULATOR
	FILE* fp = fopen(path, "rb");
	if (fp) {
		fclose(fp);
		return true;
	}
#endif
	return false;
}

static bool ResolveAudioPath(const char* rawName, char* outPath, size_t outSize)
{
	if (!rawName || !*rawName) return false;

	char norm[260];
	size_t n = 0;
	for (size_t i = 0; rawName[i] && n + 1 < sizeof(norm); i++) {
		char c = rawName[i];
		norm[n++] = (c == '\\') ? '/' : c;
	}
	norm[n] = '\0';

	const char* rel = norm;
	if ((rel[0] == 'a' || rel[0] == 'A') &&
	    (rel[1] == 'u' || rel[1] == 'U') &&
	    (rel[2] == 'd' || rel[2] == 'D') &&
	    (rel[3] == 'i' || rel[3] == 'I') &&
	    (rel[4] == 'o' || rel[4] == 'O') &&
	    rel[5] == '/') {
		rel += 6;
	}

	char base[260];
	strncpy(base, rel, sizeof(base) - 1);
	base[sizeof(base) - 1] = '\0';
	char* dot = strrchr(base, '.');
	char ext[16] = "";
	if (dot) {
		strncpy(ext, dot, sizeof(ext) - 1);
		ext[sizeof(ext) - 1] = '\0';
		*dot = '\0';
	}

	const char* exts[3];
	if (ext[0]) {
		exts[0] = ext;
		exts[1] = (tolower((unsigned char)ext[1]) == 'm') ? ".wav" : ".mp3";
		exts[2] = (tolower((unsigned char)ext[1]) == 'm') ? ".WAV" : ".MP3";
	} else {
		exts[0] = ".wav";
		exts[1] = ".mp3";
		exts[2] = ".WAV";
	}

	static const char* kDirs[] = { "audio/", "AUDIO/", "" };
	for (int d = 0; d < 3; d++) {
		for (int e = 0; e < 3; e++) {
			snprintf(outPath, outSize, "%s%s%s", kDirs[d], base, exts[e]);
			outPath[outSize - 1] = '\0';
			if (TestFileExists(outPath)) return true;
		}
		if (ext[0]) {
			char lowBase[260];
			size_t j = 0;
			for (; base[j] && j + 1 < sizeof(lowBase); j++)
				lowBase[j] = (char)tolower((unsigned char)base[j]);
			lowBase[j] = '\0';
			snprintf(outPath, outSize, "%s%s%s", kDirs[d], lowBase, exts[0]);
			outPath[outSize - 1] = '\0';
			if (TestFileExists(outPath)) return true;
		}
	}

	snprintf(outPath, outSize, "audio/%s%s", base, ext[0] ? ext : ".wav");
	outPath[outSize - 1] = '\0';
	return false;
}

static int ComputeStreamBrewVol(uint8 streamVol, uint8 masterVol, uint8 fadeVol)
{
	uint32 scaled = ((uint32)fadeVol * (uint32)streamVol * (uint32)masterVol) >> 14;
	if (scaled > MAX_VOLUME) scaled = MAX_VOLUME;
	return ClampBrewVol((int)((scaled * AEE_MAX_VOLUME + MAX_VOLUME / 2) / MAX_VOLUME));
}

static void ReleaseStreamMedia(ZeeboStream& st)
{
	BrewMediaRelease(st.mh);
#ifdef AEE_SIMULATOR
	if (st.simBuf) {
		free(st.simBuf);
		st.simBuf = NULL;
		st.simBufSize = 0;
	}
#endif
}

static void OnZeeboAudioSuspend(void)
{
	for (int i = 0; i < NUM_CHANNELS; i++) {
		ReleaseChannelMedia(s_channels[i]);
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		ReleaseStreamMedia(s_streams[i]);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// cSampleManager implementation
// ---------------------------------------------------------------------------

extern "C" void Zeebo_PurgeAudioCache(void)
{
	ClearSfxCache();
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (s_channels[i].mh.media && !BrewMediaIsActive(s_channels[i].mh)) {
			ReleaseChannelMedia(s_channels[i]);
		}
		if (!s_channels[i].mh.media && s_channels[i].wavBuf) {
			free(s_channels[i].wavBuf);
			s_channels[i].wavBuf = NULL;
			s_channels[i].wavCap = 0;
		}
	}
}

cSampleManager::cSampleManager(void)
{
	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = FALSE;
	m_bInitialised = FALSE;
	m_nNumberOfProviders = 1;
}

cSampleManager::~cSampleManager(void)
{
	Terminate();
}

void cSampleManager::SetSpeakerConfig(int32 nConfig) { (void)nConfig; }

uint32 cSampleManager::GetMaximumSupportedChannels(void)
{
	return ZEEBO_MAX_ACTIVE_SFX_VOICES;
}

uint32 cSampleManager::GetNum3DProvidersAvailable() { return 1; }
void cSampleManager::SetNum3DProvidersAvailable(uint32 num) { (void)num; }

char *cSampleManager::Get3DProviderName(uint8 id)
{
	(void)id;
	static char name[64] = "Zeebo BREW IMedia";
	return name;
}

void cSampleManager::Set3DProviderName(uint8 id, char *name) { (void)id; (void)name; }
int8 cSampleManager::GetCurrent3DProviderIndex(void) { return 0; }
int8 cSampleManager::SetCurrent3DProvider(uint8 nProvider) { (void)nProvider; return 0; }

bool8 cSampleManager::IsMP3RadioChannelAvailable(void)
{
	return FALSE;
}

void cSampleManager::ReleaseDigitalHandle(void) {}
void cSampleManager::ReacquireDigitalHandle(void) {}

bool8 cSampleManager::Initialise(void)
{
	if (_bSampmanInitialised) return TRUE;

	memset(s_channels, 0, sizeof(s_channels));
	memset(s_streams, 0, sizeof(s_streams));
	memset(s_sfxCache, 0, sizeof(s_sfxCache));
	for (int i = 0; i < NUM_CHANNELS; i++) {
		BrewMediaReset(s_channels[i].mh);
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		BrewMediaReset(s_streams[i].mh);
		s_streams[i].volume = MAX_VOLUME;
		s_streams[i].pan = 63;
	}
	for (int i = 0; i < TOTAL_STREAMED_SOUNDS; i++) {
		s_streamLengths[i] = 180000;
	}

	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = 0;

	if (!InitialiseSampleBanks()) {
		Zeebo_Log("[AUD] InitialiseSampleBanks failed; SFX disabled\n");
	}

	g_Zeebo.pfnAudioSuspend = OnZeeboAudioSuspend;
	m_bInitialised = TRUE;
	_bSampmanInitialised = TRUE;
	Zeebo_Log("[AUD] cSampleManager::Initialise ok (pMediaUtil=%p, fpSfxRaw=%p)\n",
	          g_Zeebo.pMediaUtil, s_fpSfxRaw);
	return TRUE;
}

void cSampleManager::Terminate(void)
{
	if (g_Zeebo.pfnAudioSuspend == OnZeeboAudioSuspend) {
		g_Zeebo.pfnAudioSuspend = NULL;
	}
	for (int i = 0; i < NUM_CHANNELS; i++) {
		ReleaseChannelMedia(s_channels[i]);
		if (s_channels[i].wavBuf) {
			free(s_channels[i].wavBuf);
			s_channels[i].wavBuf = NULL;
			s_channels[i].wavCap = 0;
		}
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		ReleaseStreamMedia(s_streams[i]);
	}
	ClearSfxCache();
	if (s_fpSfxRaw) {
		fclose(s_fpSfxRaw);
		s_fpSfxRaw = NULL;
	}
	m_bInitialised = FALSE;
	_bSampmanInitialised = FALSE;
}

bool8 cSampleManager::CheckForAnAudioFileOnCD(void) { return TRUE; }
char cSampleManager::GetCDAudioDriveLetter(void) { return '\0'; }

void cSampleManager::UpdateEffectsVolume(void)
{
	for (int i = 0; i < NUM_CHANNELS; i++) {
		ZeeboSfxChannel& ch = s_channels[i];
		if (ch.mh.media && ch.mh.media->pvt) {
			int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
			if (bv != ch.mh.lastBrewVol) {
				ch.mh.lastBrewVol = bv;
				ch.mh.media->pvt->SetMediaParm(ch.mh.media, MM_PARM_VOLUME, bv, 0);
			}
		}
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		ZeeboStream& st = s_streams[i];
		if (st.effectFlag && st.mh.media && st.mh.media->pvt) {
			int bv = ComputeStreamBrewVol(st.volume, m_nEffectsVolume, m_nEffectsFadeVolume);
			if (bv != st.mh.lastBrewVol) {
				st.mh.lastBrewVol = bv;
				st.mh.media->pvt->SetMediaParm(st.mh.media, MM_PARM_VOLUME, bv, 0);
			}
		}
	}
}

void cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	m_nEffectsVolume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
	m_nMusicVolume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	for (int i = 0; i < MAX_STREAMS; i++) {
		ZeeboStream& st = s_streams[i];
		if (!st.effectFlag && st.mh.media && st.mh.media->pvt) {
			int bv = ComputeStreamBrewVol(st.volume, m_nMusicVolume, m_nMusicFadeVolume);
			if (bv != st.mh.lastBrewVol) {
				st.mh.lastBrewVol = bv;
				st.mh.media->pvt->SetMediaParm(st.mh.media, MM_PARM_VOLUME, bv, 0);
			}
		}
	}
}

void cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	m_nEffectsFadeVolume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
	m_nMusicFadeVolume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	SetMusicMasterVolume(m_nMusicVolume);
}

void cSampleManager::SetMonoMode(uint8 nMode)
{
	m_nMonoMode = nMode;
}

bool8 cSampleManager::LoadSampleBank(uint8 nBank)
{
	(void)nBank;
	return TRUE;
}

void cSampleManager::UnloadSampleBank(uint8 nBank) { (void)nBank; }

int8 cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	(void)nBank;
	return LOADING_STATUS_LOADED;
}

uint8 cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	(void)nComment;
	return LOADING_STATUS_LOADED;
}

int32 cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	(void)nComment;
	return 0;
}

bool8 cSampleManager::LoadPedComment(uint32 nComment)
{
	(void)nComment;
	return TRUE;
}

int32 cSampleManager::GetBankContainingSound(uint32 offset)
{
	(void)offset;
	return 0;
}

uint32 cSampleManager::GetSampleBaseFrequency(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES) return 22050;
	return m_aSamples[nSample].nFrequency ? m_aSamples[nSample].nFrequency : 22050;
}

uint32 cSampleManager::GetSampleLoopStartOffset(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES) return 0;
	return m_aSamples[nSample].nLoopStart;
}

int32 cSampleManager::GetSampleLoopEndOffset(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES) return -1;
	return m_aSamples[nSample].nLoopEnd;
}

uint32 cSampleManager::GetSampleLength(uint32 nSample)
{
	if (nSample >= TOTAL_AUDIO_SAMPLES) return 0;
	return m_aSamples[nSample].nSize / 2;
}

bool8 cSampleManager::UpdateReverb(void) { return FALSE; }
void cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag) { (void)nChannel; (void)nReverbFlag; }

bool8 cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	(void)nBank;
	if (nChannel >= NUM_CHANNELS || nSfx >= TOTAL_AUDIO_SAMPLES)
		return FALSE;

	ReleaseChannelMedia(s_channels[nChannel]);

	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.sfxId = nSfx;
	ch.freq = m_aSamples[nSfx].nFrequency ? m_aSamples[nSfx].nFrequency : 22050;
	ch.volume = MAX_VOLUME;
	ch.pan = 63;
	ch.loopCount = 1;
	ch.posX = ch.posY = ch.posZ = 0.0f;
	ch.minDist = 2.0f;
	ch.maxDist = 100.0f;
	ch.is3D = false;
	ch.inited = true;
	return TRUE;
}

void cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	ApplyChannelBrewParams(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.is3D = true;
	ch.posX = fX;
	ch.posY = fY;
	ch.posZ = fZ;
	ApplyChannelBrewParams(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.is3D = true;
	ch.maxDist = fMax;
	ch.minDist = fMin;
	ApplyChannelBrewParams(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	ApplyChannelBrewParams(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.pan = (nPan > MAX_VOLUME) ? MAX_VOLUME : nPan;
	ApplyChannelBrewParams(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
}

void cSampleManager::SetChannelFrequency(uint32 nChannel, uint32 nFreq)
{
	if (nChannel >= NUM_CHANNELS) return;
	s_channels[nChannel].freq = nFreq;
}

void cSampleManager::SetChannelLoopPoints(uint32 nChannel, uint32 nLoopStart, int32 nLoopEnd)
{
	(void)nChannel; (void)nLoopStart; (void)nLoopEnd;
}

void cSampleManager::SetChannelLoopCount(uint32 nChannel, uint32 nLoopCount)
{
	if (nChannel >= NUM_CHANNELS) return;
	s_channels[nChannel].loopCount = nLoopCount;
}

bool8 cSampleManager::GetChannelUsedFlag(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS) return FALSE;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	if (!ch.mh.media) return FALSE;
	if (!BrewMediaIsActive(ch.mh)) {
		ReleaseChannelMedia(ch);
		return FALSE;
	}
	return TRUE;
}

void cSampleManager::StartChannel(uint32 nChannel)
{
#ifdef AEE_SIMULATOR
	(void)nChannel;
	return;
#endif
	if (nChannel >= NUM_CHANNELS || !g_Zeebo.pMediaUtil || !g_Zeebo.pMediaUtil->pvt)
		return;

	ZeeboSfxChannel& ch = s_channels[nChannel];
	if (!ch.inited || ch.sfxId >= TOTAL_AUDIO_SAMPLES)
		return;

	int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
	if (bv <= 0 && ch.loopCount != 0)
		return;

	const tSample& smp = m_aSamples[ch.sfxId];
	const uint8* pcm = GetSfxPcm(ch.sfxId, smp.nOffset, smp.nSize);
	if (!pcm || smp.nSize == 0)
		return;

	ReleaseChannelMedia(ch);
	ReapFinishedAndLimitVoices(m_nEffectsVolume, m_nEffectsFadeVolume);

	uint32 wavSize = 44u + smp.nSize;
	if (wavSize > ch.wavCap) {
		uint8* nb = (uint8*)realloc(ch.wavBuf, wavSize);
		if (!nb) return;
		ch.wavBuf = nb;
		ch.wavCap = wavSize;
	}

	uint32 rate = ch.freq ? ch.freq : (smp.nFrequency ? smp.nFrequency : 22050u);
	if (rate < 8000u) rate = 8000u;
	if (rate > 48000u) rate = 48000u;
	WriteWavHeader(ch.wavBuf, smp.nSize, rate, 1, 16);
	memcpy(ch.wavBuf + 44, pcm, smp.nSize);

	AEEMediaData md;
	md.clsData = MMD_BUFFER;
	md.pData   = ch.wavBuf;
	md.dwSize  = wavSize;

	IMedia* m = NULL;
	if (g_Zeebo.pMediaUtil->pvt->CreateMedia(g_Zeebo.pMediaUtil, &md, &m) == AEE_SUCCESS && m && m->pvt) {
		BrewMediaReset(ch.mh);
		ch.mh.media = m;
		ch.mh.state = BPS_STARTING;
		ch.mh.cmdMs = Zeebo_TimeMs();
		ch.mh.startMs = ch.mh.cmdMs;
		ch.mh.lengthMs = (int32)((uint64_t)smp.nSize * 1000ULL / ((uint64_t)rate * 2ULL));
		ch.mh.looping = (ch.loopCount == 0);
		ch.mh.lastBrewVol = bv;
		ch.mh.lastBrewPan = ComputeChannelBrewPan(ch);
		ch.startTick = ++s_voiceTick;

		m->pvt->RegisterNotify(m, OnBrewMediaNotify, &ch.mh);
		if (ch.loopCount == 0) {
			m->pvt->SetMediaParm(m, MM_PARM_PLAY_REPEAT, 0, 0);
		}
		m->pvt->SetMediaParm(m, MM_PARM_VOLUME, bv, 0);
		if (ch.mh.lastBrewPan != (MM_MAX_PAN / 2)) {
			m->pvt->SetMediaParm(m, MM_PARM_PAN, ch.mh.lastBrewPan, 0);
		}
		if (m->pvt->Play(m) != AEE_SUCCESS) {
			ReleaseChannelMedia(ch);
		}
	} else if (m && m->pvt) {
		m->pvt->Release(m);
	}
}

void cSampleManager::StopChannel(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS) return;
	ReleaseChannelMedia(s_channels[nChannel]);
}

void cSampleManager::PreloadStreamedFile(uint8 nFile, uint8 nStream)
{
	if (nStream >= MAX_STREAMS || nFile >= TOTAL_STREAMED_SOUNDS) return;
	s_streams[nStream].preloadedFileId = nFile;
	s_streams[nStream].preloaded = true;
}

void cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return;
	ZeeboStream& st = s_streams[nStream];
	if (!st.mh.media || !st.mh.media->pvt) return;

	if (nPauseFlag && st.mh.state != BPS_PAUSED) {
		st.mh.pausedPosMs = (uint32)GetStreamedFilePosition(nStream);
		st.mh.state = BPS_PAUSED;
		st.mh.media->pvt->Pause(st.mh.media);
	} else if (!nPauseFlag && st.mh.state == BPS_PAUSED) {
		st.mh.state = BPS_PLAYING;
		st.mh.startPosMs = st.mh.pausedPosMs;
		st.mh.startMs = Zeebo_TimeMs();
		st.mh.media->pvt->Resume(st.mh.media);
	}
}

void cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if (nStream >= MAX_STREAMS || !s_streams[nStream].preloaded) return;
	s_streams[nStream].preloaded = false;
	StartStreamedFile(s_streams[nStream].preloadedFileId, 0, nStream);
}

bool8 cSampleManager::StartStreamedFile(uint8 nFile, uint32 nPos, uint8 nStream)
{
#ifdef AEE_SIMULATOR
	(void)nFile;
	(void)nPos;
	(void)nStream;
	return FALSE;
#endif
	if (nStream >= MAX_STREAMS || nFile >= TOTAL_STREAMED_SOUNDS)
		return FALSE;
	if (!g_Zeebo.pMediaUtil || !g_Zeebo.pMediaUtil->pvt)
		return FALSE;

	ZeeboStream& st = s_streams[nStream];
	ReleaseStreamMedia(st);

	char resolved[260];
	if (!ResolveAudioPath(StreamedNameTable[nFile], resolved, sizeof(resolved))) {
		Zeebo_Log("[AUD] Stream not found: %s (id=%u)\n", StreamedNameTable[nFile], (unsigned)nFile);
		return FALSE;
	}

	strncpy(st.path, resolved, sizeof(st.path) - 1);
	st.path[sizeof(st.path) - 1] = '\0';

	AEEMediaData md;
	md.clsData = MMD_FILE_NAME;
	md.pData   = st.path;
	md.dwSize  = 0;

	IMedia* m = NULL;
	int rc = g_Zeebo.pMediaUtil->pvt->CreateMedia(g_Zeebo.pMediaUtil, &md, &m);
#ifdef AEE_SIMULATOR
	if (rc != AEE_SUCCESS || !m || !m->pvt) {
		if (m && m->pvt) { m->pvt->Release(m); m = NULL; }
		FILE* fp = fopen(st.path, "rb");
		if (fp) {
			fseek(fp, 0, SEEK_END);
			long fsz = ftell(fp);
			fseek(fp, 0, SEEK_SET);
			if (fsz > 0 && fsz <= ZEEBO_SIM_MAX_STREAM_BYTES) {
				st.simBuf = (uint8*)malloc((size_t)fsz);
				if (st.simBuf) {
					if (fread(st.simBuf, 1, (size_t)fsz, fp) == (size_t)fsz) {
						st.simBufSize = (uint32)fsz;
						md.clsData = MMD_BUFFER;
						md.pData   = st.simBuf;
						md.dwSize  = st.simBufSize;
						rc = g_Zeebo.pMediaUtil->pvt->CreateMedia(g_Zeebo.pMediaUtil, &md, &m);
					} else {
						free(st.simBuf);
						st.simBuf = NULL;
					}
				}
			}
			fclose(fp);
		}
	}
#endif
	if (rc != AEE_SUCCESS || !m || !m->pvt) {
		if (m && m->pvt) m->pvt->Release(m);
#ifdef AEE_SIMULATOR
		if (st.simBuf) { free(st.simBuf); st.simBuf = NULL; st.simBufSize = 0; }
#endif
		Zeebo_Log("[AUD] CreateMedia failed for '%s' rc=%d\n", st.path, rc);
		return FALSE;
	}

	BrewMediaReset(st.mh);
	st.mh.media = m;
	st.mh.state = BPS_STARTING;
	st.mh.cmdMs = Zeebo_TimeMs();
	st.mh.startMs = st.mh.cmdMs;
	st.mh.startPosMs = nPos;
	st.mh.pendingSeekMs = nPos;
	st.mh.seekDone = (nPos == 0);
	st.mh.looping = false;
	st.fileId = nFile;

	m->pvt->RegisterNotify(m, OnBrewMediaNotify, &st.mh);

	int totalMs = m->pvt->GetTotalTime(m);
	if (totalMs > 0) {
		st.mh.lengthMs = totalMs;
		s_streamLengths[nFile] = totalMs;
	} else {
		st.mh.lengthMs = s_streamLengths[nFile];
	}

	int bv = st.effectFlag
		? ComputeStreamBrewVol(st.volume, m_nEffectsVolume, m_nEffectsFadeVolume)
		: ComputeStreamBrewVol(st.volume, m_nMusicVolume, m_nMusicFadeVolume);
	st.mh.lastBrewVol = bv;
	st.mh.lastBrewPan = GtaPanToBrewPan(st.pan);

	m->pvt->SetMediaParm(m, MM_PARM_VOLUME, bv, 0);
	if (st.mh.lastBrewPan != (MM_MAX_PAN / 2)) {
		m->pvt->SetMediaParm(m, MM_PARM_PAN, st.mh.lastBrewPan, 0);
	}

	if (m->pvt->Play(m) != AEE_SUCCESS) {
		ReleaseStreamMedia(st);
		return FALSE;
	}

	Zeebo_Log("[AUD] StartStream[%u] '%s' pos=%u vol=%d\n", (unsigned)nStream, st.path, (unsigned)nPos, bv);
	return TRUE;
}

void cSampleManager::StopStreamedFile(uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return;
	ReleaseStreamMedia(s_streams[nStream]);
}

int32 cSampleManager::GetStreamedFilePosition(uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return 0;
	ZeeboStream& st = s_streams[nStream];
	if (!st.mh.media || !st.mh.media->pvt) return 0;
	if (st.mh.state == BPS_PAUSED) return (int32)st.mh.pausedPosMs;

	int32_t posMs = MM_SEEK_MODE_TIME;
	int32_t curSeek = 0;
	if (st.mh.media->pvt->GetMediaParm(st.mh.media, MM_PARM_POS, &posMs, &curSeek) == AEE_SUCCESS && posMs >= 0) {
		return (int32)posMs;
	}

	uint32 elapsed = Zeebo_TimeMs() - st.mh.startMs;
	return (int32)(st.mh.startPosMs + elapsed);
}

void cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, uint8 nEffectFlag, uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return;
	ZeeboStream& st = s_streams[nStream];
	st.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	st.pan = (nPan > MAX_VOLUME) ? MAX_VOLUME : nPan;
	st.effectFlag = nEffectFlag;
	if (st.mh.media && st.mh.media->pvt) {
		int bv = nEffectFlag
			? ComputeStreamBrewVol(st.volume, m_nEffectsVolume, m_nEffectsFadeVolume)
			: ComputeStreamBrewVol(st.volume, m_nMusicVolume, m_nMusicFadeVolume);
		if (bv != st.mh.lastBrewVol) {
			st.mh.lastBrewVol = bv;
			st.mh.media->pvt->SetMediaParm(st.mh.media, MM_PARM_VOLUME, bv, 0);
		}
		int bp = GtaPanToBrewPan(st.pan);
		if (bp != st.mh.lastBrewPan) {
			st.mh.lastBrewPan = bp;
			st.mh.media->pvt->SetMediaParm(st.mh.media, MM_PARM_PAN, bp, 0);
		}
	}
}

int32 cSampleManager::GetStreamedFileLength(uint8 nStream)
{
	if (nStream >= TOTAL_STREAMED_SOUNDS) return 1000;
	return s_streamLengths[nStream] > 0 ? s_streamLengths[nStream] : 180000;
}

bool8 cSampleManager::IsStreamPlaying(uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return FALSE;
	ZeeboStream& st = s_streams[nStream];
	if (!st.mh.media) return FALSE;
	if (!BrewMediaIsActive(st.mh)) {
		ReleaseStreamMedia(st);
		return FALSE;
	}
	return TRUE;
}

bool8 cSampleManager::InitialiseSampleBanks(void)
{
	static const char* kSdtPaths[] = { "audio/sfx.SDT", "audio/SFX.SDT", "audio/sfx.sdt" };
	static const char* kRawPaths[] = { "audio/SFX.RAW", "audio/sfx.RAW", "audio/sfx.raw" };

	FILE* fpSdt = NULL;
	for (int i = 0; i < 3 && !fpSdt; i++) fpSdt = fopen(kSdtPaths[i], "rb");
	if (!fpSdt) {
		Zeebo_Log("[AUD] Could not open audio/sfx.SDT\n");
		return FALSE;
	}

	s_fpSfxRaw = NULL;
	for (int i = 0; i < 3 && !s_fpSfxRaw; i++) s_fpSfxRaw = fopen(kRawPaths[i], "rb");
	if (!s_fpSfxRaw) {
		fclose(fpSdt);
		Zeebo_Log("[AUD] Could not open audio/SFX.RAW\n");
		return FALSE;
	}

	fseek(s_fpSfxRaw, 0, SEEK_END);
	s_sfxRawSize = (int32)ftell(s_fpSfxRaw);
	fseek(s_fpSfxRaw, 0, SEEK_SET);

	memset(m_aSamples, 0, sizeof(m_aSamples));
	size_t nRead = fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, fpSdt);
	fclose(fpSdt);

	BankStartOffset[0] = 0;
	Zeebo_Log("[AUD] Loaded sfx.SDT (%u samples) & SFX.RAW (%d bytes)\n", (unsigned)nRead, (int)s_sfxRawSize);
	return nRead > 0 ? TRUE : FALSE;
}

#endif // __ZEEBO__
