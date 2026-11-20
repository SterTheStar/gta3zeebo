// sampman_zeebo.cpp - Full GTA III (re3) audio backend for Zeebo / BREW 4.0.2 (IMediaUtil / IMedia).
// Supports:
//   - All SFX banks & ped comments from audio/sfx.SDT + audio/SFX.RAW (via in-memory WAV MMD_BUFFER + LRU cache)
//   - All streamed audio: radio stations, music, cutscenes, police radio (audio/*.wav & audio/*.mp3 via MMD_FILE_NAME)
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

// Small LRU cache for decoded/read raw PCM chunks from SFX.RAW to avoid repeated disk reads
#define ZEEBO_SFX_CACHE_SLOTS 48
#define ZEEBO_SFX_CACHE_MAX_BYTES (2 * 1024 * 1024)
#define ZEEBO_MAX_SINGLE_SFX_BYTES (512 * 1024)
#define ZEEBO_MAX_ACTIVE_SFX_VOICES 14

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
	s_sfxCacheTick = 1;
}

static const uint8* GetSfxPcm(uint32 sfxId, uint32 offset, uint32 size)
{
	if (!s_fpSfxRaw || size == 0 || size > ZEEBO_MAX_SINGLE_SFX_BYTES)
		return NULL;
	if ((int32)(offset + size) > s_sfxRawSize && s_sfxRawSize > 0)
		return NULL;

	// Hit check
	for (int i = 0; i < ZEEBO_SFX_CACHE_SLOTS; i++) {
		if (s_sfxCache[i].pcm && s_sfxCache[i].sfxId == sfxId && s_sfxCache[i].size == size) {
			s_sfxCache[i].lastUsed = ++s_sfxCacheTick;
			return s_sfxCache[i].pcm;
		}
	}

	// Evict LRU entries while over memory budget
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

	// Pick free slot or oldest slot
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
	uint32 riffSize = 36u + pcmBytes;

	memcpy(dst + 0,  "RIFF", 4);
	memcpy(dst + 4,  &riffSize, 4);
	memcpy(dst + 8,  "WAVE", 4);
	memcpy(dst + 12, "fmt ", 4);
	uint32 fmtSize = 16;
	uint16 audioFmt = 1; // PCM
	memcpy(dst + 16, &fmtSize, 4);
	memcpy(dst + 20, &audioFmt, 2);
	memcpy(dst + 22, &channels, 2);
	memcpy(dst + 24, &sampleRate, 4);
	memcpy(dst + 28, &byteRate, 4);
	memcpy(dst + 32, &blockAlign, 2);
	memcpy(dst + 34, &bitsPerSample, 2);
	memcpy(dst + 36, "data", 4);
	memcpy(dst + 40, &pcmBytes, 4);
}

struct ZeeboSfxChannel {
	IMedia* media;
	uint8*  wavBuf;
	uint32  wavCap;
	uint32  sfxId;
	uint32  freq;
	uint32  volume;      // 0..127
	uint32  pan;         // 0..127
	uint32  loopCount;   // 0 = infinite loop, 1 = once
	float   posX, posY, posZ;
	float   minDist, maxDist;
	bool    is3D;
	bool    inited;
	int     lastBrewVol;
	uint32  startTick;
};

static ZeeboSfxChannel s_channels[NUM_CHANNELS];
static uint32 s_voiceTick = 1;

static void ReleaseChannelMedia(ZeeboSfxChannel& ch)
{
	if (ch.media && ch.media->pvt) {
		ch.media->pvt->Stop(ch.media);
		ch.media->pvt->Release(ch.media);
	}
	ch.media = NULL;
	ch.lastBrewVol = -1;
}

static bool IsMediaPlaying(IMedia* m)
{
	if (!m || !m->pvt) return false;
	boolean changing = FALSE;
	int st = m->pvt->GetState(m, &changing);
	return (st == (int)MM_STATE_PLAY);
}

static int ComputeChannelBrewVol(const ZeeboSfxChannel& ch, uint8 masterVol, uint8 fadeVol)
{
	float v = (float)ch.volume / 127.0f;
	v *= ((float)masterVol / 127.0f) * ((float)fadeVol / 127.0f);
	if (ch.is3D && ch.maxDist > 0.01f) {
		float dist = sqrtf(ch.posX * ch.posX + ch.posY * ch.posY + ch.posZ * ch.posZ);
		if (dist >= ch.maxDist) {
			v = 0.0f;
		} else if (dist > ch.minDist && ch.maxDist > ch.minDist) {
			float att = 1.0f - (dist - ch.minDist) / (ch.maxDist - ch.minDist);
			if (att < 0.0f) att = 0.0f;
			v *= att;
		}
	}
	int bv = (int)(v * 100.0f + 0.5f);
	if (bv < 0) bv = 0;
	if (bv > 100) bv = 100;
	return bv;
}

static void ReapFinishedAndLimitVoices(uint8 masterVol, uint8 fadeVol)
{
	int activeCount = 0;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		if (s_channels[i].media) {
			if (!IsMediaPlaying(s_channels[i].media)) {
				ReleaseChannelMedia(s_channels[i]);
			} else {
				activeCount++;
			}
		}
	}
	while (activeCount >= ZEEBO_MAX_ACTIVE_SFX_VOICES) {
		int victim = -1;
		int lowestVol = 1000;
		uint32 oldestTick = 0xFFFFFFFFu;
		for (int i = 0; i < NUM_CHANNELS; i++) {
			if (s_channels[i].media) {
				int bv = ComputeChannelBrewVol(s_channels[i], masterVol, fadeVol);
				if (bv < lowestVol || (bv == lowestVol && s_channels[i].startTick < oldestTick)) {
					lowestVol = bv;
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

// Streamed audio (radio, music, cutscenes, police radio)
struct ZeeboStream {
	IMedia* media;
	uint8   fileId;
	uint8   preloadedFileId;
	bool    hasPreloaded;
	uint8   volume; // 0..127
	uint8   pan;
	bool    paused;
	uint32  startMs;
	uint32  startPosMs;
	uint32  pausedPosMs;
	int32   lengthMs;
	int     lastBrewVol;
	char    path[260];
};

static ZeeboStream s_streams[MAX_STREAMS];
static int32 s_streamLengths[TOTAL_STREAMED_SOUNDS];

static bool TestFileExists(const char* path)
{
	if (!path || !path[0]) return false;
	if (g_Zeebo.pFileMgr && g_Zeebo.pFileMgr->pvt) {
		return g_Zeebo.pFileMgr->pvt->Test(g_Zeebo.pFileMgr, path) == AEE_SUCCESS;
	}
	FILE* f = fopen(path, "rb");
	if (f) { fclose(f); return true; }
	return false;
}

static bool ResolveAudioPath(const char* rawName, char* outPath, size_t outSize)
{
	if (!rawName || !outPath || outSize < 8) return false;

	// Normalize backslashes -> forward slashes
	char base[260];
	size_t i = 0;
	for (; rawName[i] && i + 1 < sizeof(base); i++) {
		base[i] = (rawName[i] == '\\') ? '/' : rawName[i];
	}
	base[i] = '\0';

	// 1. Exact path
	if (TestFileExists(base)) {
		strncpy(outPath, base, outSize - 1);
		outPath[outSize - 1] = '\0';
		return true;
	}

	// 2. Lowercase path
	char lower[260];
	for (size_t j = 0; j <= i; j++) {
		lower[j] = (char)tolower((unsigned char)base[j]);
	}
	if (TestFileExists(lower)) {
		strncpy(outPath, lower, outSize - 1);
		outPath[outSize - 1] = '\0';
		return true;
	}

	// 3. Try swapping .wav <-> .mp3 (GTA III PC has many .mp3 files referenced as .wav in StreamedNameTable)
	char alt[260];
	strncpy(alt, lower, sizeof(alt) - 1);
	alt[sizeof(alt) - 1] = '\0';
	size_t len = strlen(alt);
	if (len >= 4) {
		if (strcmp(alt + len - 4, ".wav") == 0) {
			strcpy(alt + len - 4, ".mp3");
			if (TestFileExists(alt)) {
				strncpy(outPath, alt, outSize - 1);
				outPath[outSize - 1] = '\0';
				return true;
			}
			// Also try uppercase filename with .mp3 / .wav
			char upperBase[260];
			strncpy(upperBase, base, sizeof(upperBase) - 1);
			upperBase[sizeof(upperBase) - 1] = '\0';
			size_t ulen = strlen(upperBase);
			if (ulen >= 4) {
				strcpy(upperBase + ulen - 4, ".mp3");
				if (TestFileExists(upperBase)) {
					strncpy(outPath, upperBase, outSize - 1);
					outPath[outSize - 1] = '\0';
					return true;
				}
			}
		} else if (strcmp(alt + len - 4, ".mp3") == 0) {
			strcpy(alt + len - 4, ".wav");
			if (TestFileExists(alt)) {
				strncpy(outPath, alt, outSize - 1);
				outPath[outSize - 1] = '\0';
				return true;
			}
		}
	}

	return false;
}

static int ComputeStreamBrewVol(uint8 streamVol, uint8 musicMaster, uint8 musicFade)
{
	float v = ((float)streamVol / 127.0f) * ((float)musicMaster / 127.0f) * ((float)musicFade / 127.0f);
	int bv = (int)(v * 100.0f + 0.5f);
	if (bv < 0) bv = 0;
	if (bv > 100) bv = 100;
	return bv;
}

static void ReleaseStreamMedia(ZeeboStream& st)
{
	if (st.media && st.media->pvt) {
		st.media->pvt->Stop(st.media);
		st.media->pvt->Release(st.media);
	}
	st.media = NULL;
	st.paused = false;
	st.lastBrewVol = -1;
}

} // namespace

// ---------------------------------------------------------------------------
// cSampleManager implementation
// ---------------------------------------------------------------------------

cSampleManager::cSampleManager(void)
{
	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;
	m_nMonoMode = FALSE;
	m_bInitialised = FALSE;
	m_nNumberOfProviders = 1;
	memset(m_aAudioProviders, 0, sizeof(m_aAudioProviders));
	memset(m_aSamples, 0, sizeof(m_aSamples));
	m_szCDRomRootPath[0] = '\0';
}

cSampleManager::~cSampleManager(void)
{
	Terminate();
}

#ifdef EXTERNAL_3D_SOUND
void cSampleManager::SetSpeakerConfig(int32 nConfig) { (void)nConfig; }

uint32 cSampleManager::GetMaximumSupportedChannels(void)
{
	return MAXCHANNELS;
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
#endif

bool8 cSampleManager::IsMP3RadioChannelAvailable(void)
{
	return FALSE;
}

void cSampleManager::ReleaseDigitalHandle(void) {}
void cSampleManager::ReacquireDigitalHandle(void) {}

bool8 cSampleManager::Initialise(void)
{
	if (_bSampmanInitialised)
		return TRUE;

	memset(s_channels, 0, sizeof(s_channels));
	for (int i = 0; i < NUM_CHANNELS; i++) {
		s_channels[i].volume = MAX_VOLUME;
		s_channels[i].pan = 63;
		s_channels[i].loopCount = 1;
		s_channels[i].lastBrewVol = -1;
		s_channels[i].is3D = (i < MAXCHANNELS);
	}
	memset(s_streams, 0, sizeof(s_streams));
	for (int i = 0; i < MAX_STREAMS; i++) {
		s_streams[i].volume = MAX_VOLUME;
		s_streams[i].pan = 63;
		s_streams[i].lastBrewVol = -1;
	}
	for (int i = 0; i < TOTAL_STREAMED_SOUNDS; i++) {
		s_streamLengths[i] = 180000; // default 3 min until queried
	}
	ClearSfxCache();

	m_nEffectsVolume = MAX_VOLUME;
	m_nMusicVolume = MAX_VOLUME;
	m_nEffectsFadeVolume = MAX_VOLUME;
	m_nMusicFadeVolume = MAX_VOLUME;

	InitialiseSampleBanks();

	m_bInitialised = TRUE;
	_bSampmanInitialised = TRUE;
	Zeebo_Log("[AUD] cSampleManager::Initialise ok (pMediaUtil=%p, fpSfxRaw=%p)\n",
	          g_Zeebo.pMediaUtil, s_fpSfxRaw);
	return TRUE;
}

void cSampleManager::Terminate(void)
{
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
		if (ch.media && ch.media->pvt) {
			int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
			if (bv != ch.lastBrewVol) {
				ch.lastBrewVol = bv;
				ch.media->pvt->SetMediaParm(ch.media, (int)MM_PARM_VOLUME, bv, 0);
			}
		}
	}
}

void cSampleManager::SetEffectsMasterVolume(uint8 nVolume)
{
	m_nEffectsVolume = nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicMasterVolume(uint8 nVolume)
{
	m_nMusicVolume = nVolume;
	for (int i = 0; i < MAX_STREAMS; i++) {
		if (s_streams[i].media && s_streams[i].media->pvt) {
			int bv = ComputeStreamBrewVol(s_streams[i].volume, m_nMusicVolume, m_nMusicFadeVolume);
			if (bv != s_streams[i].lastBrewVol) {
				s_streams[i].lastBrewVol = bv;
				s_streams[i].media->pvt->SetMediaParm(s_streams[i].media, (int)MM_PARM_VOLUME, bv, 0);
			}
		}
	}
}

void cSampleManager::SetEffectsFadeVolume(uint8 nVolume)
{
	m_nEffectsFadeVolume = nVolume;
	UpdateEffectsVolume();
}

void cSampleManager::SetMusicFadeVolume(uint8 nVolume)
{
	m_nMusicFadeVolume = nVolume;
	SetMusicMasterVolume(m_nMusicVolume);
}

void cSampleManager::SetMonoMode(uint8 nMode)
{
	m_nMonoMode = nMode;
}

bool8 cSampleManager::LoadSampleBank(uint8 nBank)
{
	(void)nBank;
	return (s_fpSfxRaw != NULL) ? TRUE : FALSE;
}

void cSampleManager::UnloadSampleBank(uint8 nBank) { (void)nBank; }

int8 cSampleManager::IsSampleBankLoaded(uint8 nBank)
{
	(void)nBank;
	return (s_fpSfxRaw != NULL) ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
}

uint8 cSampleManager::IsPedCommentLoaded(uint32 nComment)
{
	(void)nComment;
	return (s_fpSfxRaw != NULL) ? LOADING_STATUS_LOADED : LOADING_STATUS_NOT_LOADED;
}

int32 cSampleManager::_GetPedCommentSlot(uint32 nComment)
{
	(void)nComment;
	return 0;
}

bool8 cSampleManager::LoadPedComment(uint32 nComment)
{
	(void)nComment;
	return (s_fpSfxRaw != NULL) ? TRUE : FALSE;
}

int32 cSampleManager::GetBankContainingSound(uint32 offset)
{
	(void)offset;
	return SFX_BANK_0;
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
	return m_aSamples[nSample].nSize >> 1; // 16-bit samples
}

bool8 cSampleManager::UpdateReverb(void) { return FALSE; }
void cSampleManager::SetChannelReverbFlag(uint32 nChannel, bool8 nReverbFlag) { (void)nChannel; (void)nReverbFlag; }

bool8 cSampleManager::InitialiseChannel(uint32 nChannel, uint32 nSfx, uint8 nBank)
{
	(void)nBank;
	if (nChannel >= NUM_CHANNELS || nSfx >= TOTAL_AUDIO_SAMPLES || !s_fpSfxRaw)
		return FALSE;

	ZeeboSfxChannel& ch = s_channels[nChannel];
	ReleaseChannelMedia(ch);
	ch.sfxId = nSfx;
	ch.freq = m_aSamples[nSfx].nFrequency ? m_aSamples[nSfx].nFrequency : 22050;
	ch.loopCount = 1;
	ch.inited = true;
	return TRUE;
}

#ifdef EXTERNAL_3D_SOUND
void cSampleManager::SetChannelEmittingVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	if (ch.media && ch.media->pvt) {
		int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
		if (bv != ch.lastBrewVol) {
			ch.lastBrewVol = bv;
			ch.media->pvt->SetMediaParm(ch.media, (int)MM_PARM_VOLUME, bv, 0);
		}
	}
}

void cSampleManager::SetChannel3DPosition(uint32 nChannel, float fX, float fY, float fZ)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.posX = fX; ch.posY = fY; ch.posZ = fZ;
	if (ch.media && ch.media->pvt) {
		int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
		if (bv != ch.lastBrewVol) {
			ch.lastBrewVol = bv;
			ch.media->pvt->SetMediaParm(ch.media, (int)MM_PARM_VOLUME, bv, 0);
		}
	}
}

void cSampleManager::SetChannel3DDistances(uint32 nChannel, float fMax, float fMin)
{
	if (nChannel >= NUM_CHANNELS) return;
	s_channels[nChannel].maxDist = fMax;
	s_channels[nChannel].minDist = fMin;
}
#endif

void cSampleManager::SetChannelVolume(uint32 nChannel, uint32 nVolume)
{
	if (nChannel >= NUM_CHANNELS) return;
	ZeeboSfxChannel& ch = s_channels[nChannel];
	ch.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	if (ch.media && ch.media->pvt) {
		int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
		if (bv != ch.lastBrewVol) {
			ch.lastBrewVol = bv;
			ch.media->pvt->SetMediaParm(ch.media, (int)MM_PARM_VOLUME, bv, 0);
		}
	}
}

void cSampleManager::SetChannelPan(uint32 nChannel, uint32 nPan)
{
	if (nChannel >= NUM_CHANNELS) return;
	s_channels[nChannel].pan = nPan;
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
	if (!ch.media) return FALSE;
	if (!IsMediaPlaying(ch.media)) {
		ReleaseChannelMedia(ch);
		return FALSE;
	}
	return TRUE;
}

void cSampleManager::StartChannel(uint32 nChannel)
{
	if (nChannel >= NUM_CHANNELS || !g_Zeebo.pMediaUtil || !g_Zeebo.pMediaUtil->pvt)
		return;

	ZeeboSfxChannel& ch = s_channels[nChannel];
	if (!ch.inited || ch.sfxId >= TOTAL_AUDIO_SAMPLES)
		return;

	int bv = ComputeChannelBrewVol(ch, m_nEffectsVolume, m_nEffectsFadeVolume);
	if (bv <= 0 && ch.loopCount != 0)
		return; // skip inaudible one-shots

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
		ch.media = m;
		ch.startTick = ++s_voiceTick;
		ch.lastBrewVol = bv;
		m->pvt->SetMediaParm(m, (int)MM_PARM_PLAY_REPEAT, (ch.loopCount == 0) ? 0 : 1, 0);
		m->pvt->SetMediaParm(m, (int)MM_PARM_VOLUME, bv, 0);
		m->pvt->Play(m);
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
	s_streams[nStream].hasPreloaded = true;
}

void cSampleManager::PauseStream(bool8 nPauseFlag, uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return;
	ZeeboStream& st = s_streams[nStream];
	if (!st.media || !st.media->pvt) return;
	if (nPauseFlag && !st.paused) {
		st.pausedPosMs = (uint32)GetStreamedFilePosition(nStream);
		st.paused = true;
		st.media->pvt->Pause(st.media);
	} else if (!nPauseFlag && st.paused) {
		st.paused = false;
		st.startPosMs = st.pausedPosMs;
		st.startMs = Zeebo_TimeMs();
		st.media->pvt->Resume(st.media);
	}
}

void cSampleManager::StartPreloadedStreamedFile(uint8 nStream)
{
	if (nStream >= MAX_STREAMS) return;
	if (s_streams[nStream].hasPreloaded) {
		StartStreamedFile(s_streams[nStream].preloadedFileId, 0, nStream);
	}
}

bool8 cSampleManager::StartStreamedFile(uint8 nFile, uint32 nPos, uint8 nStream)
{
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
	if (rc != AEE_SUCCESS || !m || !m->pvt) {
		if (m && m->pvt) m->pvt->Release(m);
		Zeebo_Log("[AUD] CreateMedia failed for '%s' rc=%d\n", st.path, rc);
		return FALSE;
	}

	st.media = m;
	st.fileId = nFile;
	st.paused = false;
	st.startPosMs = nPos;
	st.startMs = Zeebo_TimeMs();

	int totalMs = m->pvt->GetTotalTime(m);
	if (totalMs > 0) {
		st.lengthMs = totalMs;
		s_streamLengths[nFile] = totalMs;
	} else {
		st.lengthMs = s_streamLengths[nFile];
	}

	int bv = ComputeStreamBrewVol(st.volume, m_nMusicVolume, m_nMusicFadeVolume);
	st.lastBrewVol = bv;
	m->pvt->SetMediaParm(m, (int)MM_PARM_PLAY_REPEAT, 1, 0);
	m->pvt->SetMediaParm(m, (int)MM_PARM_VOLUME, bv, 0);
	if (nPos > 0) {
		m->pvt->Seek(m, 0 /* MM_SEEK_START */, (int32_t)nPos);
	}
	m->pvt->Play(m);
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
	if (!st.media) return 0;
	if (st.paused) return (int32)st.pausedPosMs;
	uint32 elapsed = Zeebo_TimeMs() - st.startMs;
	return (int32)(st.startPosMs + elapsed);
}

void cSampleManager::SetStreamedVolumeAndPan(uint8 nVolume, uint8 nPan, uint8 nEffectFlag, uint8 nStream)
{
	(void)nEffectFlag;
	if (nStream >= MAX_STREAMS) return;
	ZeeboStream& st = s_streams[nStream];
	st.volume = (nVolume > MAX_VOLUME) ? MAX_VOLUME : nVolume;
	st.pan = nPan;
	if (st.media && st.media->pvt) {
		int bv = ComputeStreamBrewVol(st.volume, m_nMusicVolume, m_nMusicFadeVolume);
		if (bv != st.lastBrewVol) {
			st.lastBrewVol = bv;
			st.media->pvt->SetMediaParm(st.media, (int)MM_PARM_VOLUME, bv, 0);
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
	if (!st.media) return FALSE;
	if (st.paused) return TRUE;
	if (!IsMediaPlaying(st.media)) {
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

	fread(m_aSamples, sizeof(tSample), TOTAL_AUDIO_SAMPLES, fpSdt);
	fclose(fpSdt);
	Zeebo_Log("[AUD] Loaded sfx.SDT (%d samples) & SFX.RAW (%d bytes)\n",
	          (int)TOTAL_AUDIO_SAMPLES, (int)s_sfxRawSize);
	return TRUE;
}

#endif // __ZEEBO__
