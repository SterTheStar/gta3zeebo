// zeebo_movie.cpp - Full MPEG-1 (video + MP2 audio) player for Zeebo (pl_mpeg + GLES1 + BREW IMedia).
#ifdef __ZEEBO__

#include "zeebo_movie.h"
#include "zeebo_brew.h"
#include "zeebo_gles1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

// Provided by vendor/librw/src/gles1/gles1.cpp
namespace rw {
namespace gles1 {
void notifyTextureBound(unsigned int texid);
void setClientStates(bool vert, bool norm, bool color, bool tex);
void setLightingState(bool lighting, bool rescaleNormal, bool colorMaterial);
bool isTextureEnabled(void);
}
}

namespace {

static plm_t*     s_plm          = NULL;
static uint8_t*   s_fileBuf      = NULL;
static size_t     s_fileSize     = 0;
static uint16_t*  s_rgb565       = NULL;
static uint32_t   s_texId        = 0;
static int        s_vidW         = 0;
static int        s_vidH         = 0;
static int        s_texW         = 512;
static int        s_texH         = 256;
static bool       s_frameDirty   = false;
static bool       s_playing      = false;
static bool       s_skipReq      = false;
static uint32_t   s_lastMs       = 0;

static IMedia*    s_audioMedia   = NULL;
static uint8_t*   s_audioWav     = NULL;
static uint32_t   s_audioWavSize = 0;

static int NextPow2(int v)
{
	int p = 1;
	while (p < v) p <<= 1;
	return p;
}

static inline uint8_t ClampU8(int v)
{
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Fast integer YCbCr 4:2:0 -> RGB565 conversion (zero floats per pixel)
static void ConvertYCbCrToRGB565(plm_frame_t* frame, uint16_t* dst, int w, int h)
{
	const uint8_t* yPlane  = frame->y.data;
	const uint8_t* cbPlane = frame->cb.data;
	const uint8_t* crPlane = frame->cr.data;
	const int yStride  = (int)frame->y.width;
	const int cStride  = (int)frame->cb.width;

	for (int y = 0; y < h; y++) {
		const uint8_t* yRow  = yPlane  + y * yStride;
		const uint8_t* cbRow = cbPlane + (y >> 1) * cStride;
		const uint8_t* crRow = crPlane + (y >> 1) * cStride;
		uint16_t* out = dst + y * w;

		for (int x = 0; x < w; x++) {
			int Y  = (int)yRow[x] - 16;
			int Cb = (int)cbRow[x >> 1] - 128;
			int Cr = (int)crRow[x >> 1] - 128;

			int c = 298 * Y;
			uint8_t r = ClampU8((c + 409 * Cr + 128) >> 8);
			uint8_t g = ClampU8((c - 100 * Cb - 208 * Cr + 128) >> 8);
			uint8_t b = ClampU8((c + 516 * Cb + 128) >> 8);

			out[x] = (uint16_t)(((uint32_t)(r >> 3) << 11) |
			                    ((uint32_t)(g >> 2) << 5)  |
			                    ((uint32_t)(b >> 3)));
		}
	}
}

static void OnVideoFrame(plm_t* plm, plm_frame_t* frame, void* user)
{
	(void)plm; (void)user;
	if (!frame || !s_rgb565) return;
	ConvertYCbCrToRGB565(frame, s_rgb565, s_vidW, s_vidH);
	s_frameDirty = true;
}

static void WriteMovieWavHeader(uint8_t* dst, uint32_t pcmBytes, uint32_t sampleRate, uint16_t channels)
{
	uint16_t bits = 16;
	uint32_t byteRate = sampleRate * channels * 2u;
	uint16_t blockAlign = (uint16_t)(channels * 2u);
	uint32_t riffSize = 36u + pcmBytes;
	uint32_t fmtSize = 16;
	uint16_t audioFmt = 1;

	memcpy(dst + 0,  "RIFF", 4);
	memcpy(dst + 4,  &riffSize, 4);
	memcpy(dst + 8,  "WAVE", 4);
	memcpy(dst + 12, "fmt ", 4);
	memcpy(dst + 16, &fmtSize, 4);
	memcpy(dst + 20, &audioFmt, 2);
	memcpy(dst + 22, &channels, 2);
	memcpy(dst + 24, &sampleRate, 4);
	memcpy(dst + 28, &byteRate, 4);
	memcpy(dst + 32, &blockAlign, 2);
	memcpy(dst + 34, &bits, 2);
	memcpy(dst + 36, "data", 4);
	memcpy(dst + 40, &pcmBytes, 4);
}

static void StartMovieAudio(const uint8_t* fileData, size_t fileSize)
{
	if (!g_Zeebo.pMediaUtil || !g_Zeebo.pMediaUtil->pvt) return;

	plm_t* aplm = plm_create_with_memory((uint8_t*)fileData, fileSize, 0);
	if (!aplm) return;

	plm_set_video_enabled(aplm, FALSE);
	plm_set_audio_enabled(aplm, TRUE);

	int numAudio = plm_get_num_audio_streams(aplm);
	if (numAudio <= 0) {
		plm_destroy(aplm);
		return;
	}

	int sampleRate = plm_get_samplerate(aplm);
	if (sampleRate <= 0) sampleRate = 44100;

	// Decode MP2 audio track into a mono 16-bit PCM WAV buffer (halves RAM usage on 64MB Zeebo)
	size_t capSamples = (size_t)sampleRate * 40u; // up to 40s initial buffer (~3.5MB max)
	if (capSamples > 2000000u) capSamples = 2000000u;
	uint8_t* wav = (uint8_t*)malloc(44u + capSamples * sizeof(int16_t));
	if (!wav) {
		plm_destroy(aplm);
		return;
	}

	int16_t* pcmOut = (int16_t*)(wav + 44u);
	size_t totalSamples = 0;

	plm_samples_t* samples = NULL;
	while ((samples = plm_decode_audio(aplm)) != NULL) {
		uint32_t cnt = samples->count;
		if (totalSamples + cnt > capSamples) {
			cnt = (uint32_t)(capSamples - totalSamples);
		}
		for (uint32_t i = 0; i < cnt; i++) {
			float mono = 0.5f * (samples->interleaved[i * 2] + samples->interleaved[i * 2 + 1]);
			int32_t s = (int32_t)(mono * 32767.0f);
			if (s > 32767) s = 32767;
			else if (s < -32768) s = -32768;
			pcmOut[totalSamples++] = (int16_t)s;
		}
		if (totalSamples >= capSamples) break;
	}

	plm_destroy(aplm);

	if (totalSamples == 0) {
		free(wav);
		return;
	}

	uint32_t pcmBytes = (uint32_t)(totalSamples * sizeof(int16_t));
	WriteMovieWavHeader(wav, pcmBytes, (uint32_t)sampleRate, 1);

	s_audioWav = wav;
	s_audioWavSize = 44u + pcmBytes;

	AEEMediaData md;
	md.clsData = MMD_BUFFER;
	md.pData   = s_audioWav;
	md.dwSize  = s_audioWavSize;

	IMedia* m = NULL;
	int rc = g_Zeebo.pMediaUtil->pvt->CreateMedia(g_Zeebo.pMediaUtil, &md, &m);
	if (rc == AEE_SUCCESS && m && m->pvt) {
		s_audioMedia = m;
		m->pvt->SetMediaParm(m, (int)MM_PARM_PLAY_REPEAT, 1, 0);
		m->pvt->SetMediaParm(m, (int)MM_PARM_VOLUME, 100, 0);
		m->pvt->Play(m);
		Zeebo_Log("[MOV] Audio started (%u Hz, %u samples)\n", (unsigned)sampleRate, (unsigned)totalSamples);
	} else {
		if (m && m->pvt) m->pvt->Release(m);
		free(s_audioWav);
		s_audioWav = NULL;
		s_audioWavSize = 0;
	}
}

static void DrawMovieQuad(void)
{
	const IGLVtbl* gl = ZeeboGL();
	if (!gl || !s_texId) return;

	gl->glViewport(0, 0, 640, 480);
	gl->glClearColorx(0, 0, 0, 65536);
	gl->glClear(GL_COLOR_BUFFER_BIT);

	gl->glDisable(GL_DEPTH_TEST);
	gl->glDisable(GL_CULL_FACE);
	gl->glDisable(GL_FOG);
	gl->glDisable(GL_BLEND);
	gl->glDisable(GL_ALPHA_TEST);
	gl->glEnable(GL_TEXTURE_2D);

	gl->glBindTexture(GL_TEXTURE_2D, s_texId);
	rw::gles1::notifyTextureBound(s_texId);

	if (s_frameDirty && s_rgb565) {
		gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_vidW, s_vidH,
		                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, s_rgb565);
		s_frameDirty = false;
	}

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPushMatrix();
	gl->glLoadIdentity();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPushMatrix();
	gl->glLoadIdentity();

	rw::gles1::setLightingState(false, false, false);
	rw::gles1::setClientStates(true, false, false, true);
	gl->glColor4x(65536, 65536, 65536, 65536);

	float uMax = (float)s_vidW / (float)s_texW;
	float vMax = (float)s_vidH / (float)s_texH;

	// Letterbox to 4:3 screen (640x480)
	float vidAspect = (s_vidH > 0) ? ((float)s_vidW / (float)s_vidH) : (4.0f / 3.0f);
	float scrAspect = 640.0f / 480.0f;
	float sx = 1.0f, sy = 1.0f;
	if (vidAspect > scrAspect) {
		sy = scrAspect / vidAspect;
	} else if (vidAspect < scrAspect) {
		sx = vidAspect / scrAspect;
	}

	const float verts[8] = {
		-sx,  sy,
		 sx,  sy,
		-sx, -sy,
		 sx, -sy
	};
	const float uvs[8] = {
		0.0f, 0.0f,
		uMax, 0.0f,
		0.0f, vMax,
		uMax, vMax
	};

	gl->glVertexPointer(2, GL_FLOAT, 0, verts);
	gl->glTexCoordPointer(2, GL_FLOAT, 0, uvs);
	gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPopMatrix();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPopMatrix();

	if (!rw::gles1::isTextureEnabled()) {
		gl->glDisable(GL_TEXTURE_2D);
	}
}

} // namespace

extern "C" {

int Zeebo_MovieStart(const char* path)
{
	Zeebo_MovieStop();
	if (!path) return 0;

	FILE* fp = fopen(path, "rb");
	if (!fp) {
		Zeebo_Log("[MOV] Could not open '%s'\n", path);
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz <= 0 || sz > 8 * 1024 * 1024) {
		fclose(fp);
		return 0;
	}

	s_fileBuf = (uint8_t*)malloc((size_t)sz);
	if (!s_fileBuf) {
		fclose(fp);
		return 0;
	}
	if (fread(s_fileBuf, 1, (size_t)sz, fp) != (size_t)sz) {
		fclose(fp);
		free(s_fileBuf);
		s_fileBuf = NULL;
		return 0;
	}
	fclose(fp);
	s_fileSize = (size_t)sz;

	s_plm = plm_create_with_memory(s_fileBuf, s_fileSize, 0);
	if (!s_plm || !plm_has_headers(s_plm)) {
		Zeebo_Log("[MOV] Invalid MPEG-1 headers in '%s'\n", path);
		Zeebo_MovieStop();
		return 0;
	}

	s_vidW = plm_get_width(s_plm);
	s_vidH = plm_get_height(s_plm);
	if (s_vidW <= 0 || s_vidH <= 0) {
		Zeebo_MovieStop();
		return 0;
	}
	s_texW = NextPow2(s_vidW);
	s_texH = NextPow2(s_vidH);

	s_rgb565 = (uint16_t*)calloc((size_t)s_vidW * (size_t)s_vidH, sizeof(uint16_t));
	if (!s_rgb565) {
		Zeebo_MovieStop();
		return 0;
	}

	if (const IGLVtbl* gl = ZeeboGL()) {
		gl->glGenTextures(1, &s_texId);
		gl->glBindTexture(GL_TEXTURE_2D, s_texId);
		rw::gles1::notifyTextureBound(s_texId);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		uint16_t* zeroTex = (uint16_t*)calloc((size_t)s_texW * (size_t)s_texH, sizeof(uint16_t));
		gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, s_texW, s_texH, 0,
		                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, zeroTex);
		free(zeroTex);
	}

	plm_set_audio_enabled(s_plm, FALSE);
	plm_set_video_enabled(s_plm, TRUE);
	plm_set_loop(s_plm, FALSE);
	plm_set_video_decode_callback(s_plm, OnVideoFrame, NULL);

	StartMovieAudio(s_fileBuf, s_fileSize);

	s_frameDirty = false;
	s_skipReq = false;
	s_playing = true;
	s_lastMs = Zeebo_TimeMs();

	// Decode initial frame immediately so first draw isn't black
	plm_decode(s_plm, 0.04);

	Zeebo_Log("[MOV] Started '%s' (%dx%d, tex %dx%d, %.2f fps)\n",
	          path, s_vidW, s_vidH, s_texW, s_texH, plm_get_framerate(s_plm));
	return 1;
}

int Zeebo_MovieTick(void)
{
	if (!s_playing || !s_plm || s_skipReq) {
		Zeebo_MovieStop();
		return 0;
	}

	uint32_t now = Zeebo_TimeMs();
	double dt = (double)(now - s_lastMs) / 1000.0;
	s_lastMs = now;
	if (dt < 0.001) dt = 0.001;
	if (dt > 0.1)   dt = 0.1;

	plm_decode(s_plm, dt);
	DrawMovieQuad();

	if (plm_has_ended(s_plm)) {
		Zeebo_MovieStop();
		return 0;
	}
	return 1;
}

void Zeebo_MovieStop(void)
{
	if (s_audioMedia && s_audioMedia->pvt) {
		s_audioMedia->pvt->Stop(s_audioMedia);
		s_audioMedia->pvt->Release(s_audioMedia);
	}
	s_audioMedia = NULL;
	free(s_audioWav);
	s_audioWav = NULL;
	s_audioWavSize = 0;

	if (s_texId) {
		if (const IGLVtbl* gl = ZeeboGL()) {
			gl->glDeleteTextures(1, &s_texId);
		}
		rw::gles1::notifyTextureBound(0);
		s_texId = 0;
	}

	if (s_plm) {
		plm_destroy(s_plm);
		s_plm = NULL;
	}
	free(s_rgb565);
	s_rgb565 = NULL;
	free(s_fileBuf);
	s_fileBuf = NULL;
	s_fileSize = 0;

	s_playing = false;
	s_skipReq = false;
	s_frameDirty = false;
}

void Zeebo_MovieSkip(void)
{
	if (s_playing) {
		s_skipReq = true;
	}
}

int Zeebo_MovieIsPlaying(void)
{
	return s_playing ? 1 : 0;
}

} // extern "C"

#endif // __ZEEBO__
