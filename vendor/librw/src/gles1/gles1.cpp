#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#ifdef RW_GLES1
#include "rwgles1.h"
#include "zeebo_gles1.h"

extern "C" {
uint32_t g_zeeboDrawCalls = 0;
uint32_t g_zeeboTris = 0;
}

namespace rw {
namespace gles1 {

static void *s_eglDisplay = NULL;
static void *s_eglSurface = NULL;
static void *s_eglContext = NULL;
static int   s_screenW    = 640;
static int   s_screenH    = 480;

struct RenderStateCache {
	Texture *textureRaster;
	int32    texAddressU;
	int32    texAddressV;
	int32    texFilter;
	bool32   zTest;
	bool32   zWrite;
	bool32   vertexAlpha;
	bool32   textureAlpha;
	int32    srcBlend;
	int32    dstBlend;
	int32    cullMode;
	bool32   fogEnable;
	uint32   fogColor;
	int32    fogType;
	float32  fogDensity;
	int32    alphaTestFunc;
	int32    alphaTestRef;
	bool32   gsAlphaTest;
	int32    gsAlphaTestRef;
	int32    shadeMode;
	bool32   texPerspective;
	int32    stencilEnable;
	int32    stencilFail;
	int32    stencilZFail;
	int32    stencilPass;
	int32    stencilFunc;
	int32    stencilRef;
	int32    stencilMask;
	int32    stencilWriteMask;
};

static RenderStateCache s_state;

struct GLStateCache {
	bool   texEnabled;
	uint32 boundTexId;
	bool   blendEnabled;
	bool   alphaTestEnabled;
	uint32 lastAlphaFunc;
	GLfixed lastAlphaRef;
	bool   arrVert;
	bool   arrNorm;
	bool   arrColor;
	bool   arrTex;
	bool   lighting;
	bool   rescaleNormal;
	bool   colorMaterial;
	bool   colorValid;
	uint32 lastColorRGBA;
};

static GLStateCache s_gl;

static inline void UpdateDepthState(const IGLVtbl *gl)
{
	if(!gl) return;
	if(s_state.zTest){
		gl->glEnable(GL_DEPTH_TEST);
		gl->glDepthFunc(GL_LEQUAL);
	}else if(s_state.zWrite){
		// OpenGL ES skips depth writes when GL_DEPTH_TEST is disabled (needed by DrawRadarMask)
		gl->glEnable(GL_DEPTH_TEST);
		gl->glDepthFunc(GL_ALWAYS);
	}else{
		gl->glDisable(GL_DEPTH_TEST);
	}
}

static inline void UpdateAlphaTestState(const IGLVtbl *gl)
{
	if(!gl) return;
	bool hasAlpha = (s_state.vertexAlpha != 0) || (s_state.textureAlpha != 0);
	bool want = hasAlpha && (s_state.alphaTestFunc == ALPHAGREATEREQUAL || s_state.alphaTestFunc == ALPHALESS);
	if(s_gl.alphaTestEnabled != want){
		s_gl.alphaTestEnabled = want;
		if(want) gl->glEnable(GL_ALPHA_TEST);
		else     gl->glDisable(GL_ALPHA_TEST);
	}
	if(want){
		uint32 fn = (s_state.alphaTestFunc == ALPHALESS) ? GL_LESS : GL_GEQUAL;
		int32 r = s_state.alphaTestRef;
		if(r < 0) r = 0; else if(r > 255) r = 255;
		GLfixed ref = (GLfixed)((uint32)r * 257u + ((uint32)r >> 7));
		if(s_gl.lastAlphaFunc != fn || s_gl.lastAlphaRef != ref){
			s_gl.lastAlphaFunc = fn;
			s_gl.lastAlphaRef  = ref;
			gl->glAlphaFuncx(fn, ref);
		}
	}
}

static inline void UpdateBlendState(const IGLVtbl *gl)
{
	if(!gl) return;
	bool want = (s_state.vertexAlpha != 0) || (s_state.textureAlpha != 0);
	if(s_gl.blendEnabled != want){
		s_gl.blendEnabled = want;
		if(want) gl->glEnable(GL_BLEND);
		else     gl->glDisable(GL_BLEND);
	}
	UpdateAlphaTestState(gl);
}

void notifyTextureBound(uint32 texid)
{
	s_gl.boundTexId = texid;
}

bool isTextureEnabled(void)
{
	return s_gl.texEnabled;
}

void setClientStates(bool vert, bool norm, bool color, bool tex)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;
	if(s_gl.arrVert != vert){
		s_gl.arrVert = vert;
		if(vert) gl->glEnableClientState(GL_VERTEX_ARRAY);
		else     gl->glDisableClientState(GL_VERTEX_ARRAY);
	}
	if(s_gl.arrNorm != norm){
		s_gl.arrNorm = norm;
		if(norm) gl->glEnableClientState(GL_NORMAL_ARRAY);
		else     gl->glDisableClientState(GL_NORMAL_ARRAY);
	}
	if(s_gl.arrColor != color){
		s_gl.arrColor = color;
		if(color) gl->glEnableClientState(GL_COLOR_ARRAY);
		else      gl->glDisableClientState(GL_COLOR_ARRAY);
	}
	// Drawing with GL_COLOR_ARRAY clobbers GL_CURRENT_COLOR in GLES 1.1; invalidate constant color cache
	if(color){
		s_gl.colorValid = false;
	}
	if(s_gl.arrTex != tex){
		s_gl.arrTex = tex;
		if(tex) gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		else    gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
}

void setLightingState(bool lighting, bool rescaleNormal, bool colorMaterial)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;
	if(s_gl.lighting != lighting){
		s_gl.lighting = lighting;
		if(lighting) gl->glEnable(GL_LIGHTING);
		else         gl->glDisable(GL_LIGHTING);
	}
	if(s_gl.rescaleNormal != rescaleNormal){
		s_gl.rescaleNormal = rescaleNormal;
		if(rescaleNormal) gl->glEnable(GL_RESCALE_NORMAL);
		else              gl->glDisable(GL_RESCALE_NORMAL);
	}
	if(s_gl.colorMaterial != colorMaterial){
		s_gl.colorMaterial = colorMaterial;
		if(colorMaterial) gl->glEnable(GL_COLOR_MATERIAL);
		else              gl->glDisable(GL_COLOR_MATERIAL);
	}
}

void setConstantColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	uint32 packed = ((uint32)r) | ((uint32)g << 8) | ((uint32)b << 16) | ((uint32)a << 24);
	if(s_gl.colorValid && s_gl.lastColorRGBA == packed)
		return;
	s_gl.colorValid = true;
	s_gl.lastColorRGBA = packed;
	if(const IGLVtbl *gl = ZeeboGL()){
		// Map 0..255 -> 0..65536 (exact: (v * 65536 + 127) / 255, fast: (v << 8) + v + (v >> 7))
		gl->glColor4x(
			(GLfixed)((uint32)r * 257u + ((uint32)r >> 7)),
			(GLfixed)((uint32)g * 257u + ((uint32)g >> 7)),
			(GLfixed)((uint32)b * 257u + ((uint32)b >> 7)),
			(GLfixed)((uint32)a * 257u + ((uint32)a >> 7))
		);
	}
}

static uint32 BlendMap(int32 rwBlend)
{
	switch(rwBlend){
	case BLENDZERO:         return GL_ZERO;
	case BLENDONE:          return GL_ONE;
	case BLENDSRCCOLOR:     return GL_SRC_COLOR;
	case BLENDINVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
	case BLENDSRCALPHA:     return GL_SRC_ALPHA;
	case BLENDINVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
	case BLENDDESTALPHA:    return GL_DST_ALPHA;
	case BLENDINVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
	case BLENDDESTCOLOR:    return GL_DST_COLOR;
	case BLENDINVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
	case BLENDSRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
	default:                return GL_ONE;
	}
}

static bool InitEGL(int width, int height)
{
	(void)width;
	(void)height;
	if(!g_Zeebo.pEGL || !g_Zeebo.pEGL->pvt || !ZeeboGL())
		return false;

	const IEGLVtbl *egl = g_Zeebo.pEGL->pvt;
	s_eglDisplay = egl->eglGetDisplay(g_Zeebo.pDisplay ? (int)(uintptr_t)g_Zeebo.pDisplay : ZEEBO_EGL_DEFAULT_DISPLAY);
	if(!s_eglDisplay) return false;

	int major = 0, minor = 0;
	if(!egl->eglInitialize(s_eglDisplay, &major, &minor)) return false;

	static const int cfgAttribs[] = {
		ZEEBO_EGL_RED_SIZE,   5,
		ZEEBO_EGL_GREEN_SIZE, 6,
		ZEEBO_EGL_BLUE_SIZE,  5,
		ZEEBO_EGL_DEPTH_SIZE, 16,
		ZEEBO_EGL_NONE
	};
	void *cfg = NULL;
	int numCfg = 0;
	if(!egl->eglChooseConfig(s_eglDisplay, cfgAttribs, &cfg, 1, &numCfg) || numCfg < 1)
		return false;

	s_eglSurface = egl->eglCreateWindowSurface(s_eglDisplay, cfg, NULL, NULL);
	if(!s_eglSurface){
		void *nativeWin = Zeebo_GetNativeWindow();
		if(nativeWin)
			s_eglSurface = egl->eglCreateWindowSurface(s_eglDisplay, cfg, nativeWin, NULL);
	}
	if(!s_eglSurface) return false;

	s_eglContext = egl->eglCreateContext(s_eglDisplay, cfg, ZEEBO_EGL_NO_CONTEXT, NULL);
	if(!s_eglContext) return false;

	if(!egl->eglMakeCurrent(s_eglDisplay, s_eglSurface, s_eglSurface, s_eglContext))
		return false;

	if(const IGLVtbl *gl = ZeeboGL()){
		gl->glViewport(0, 0, s_screenW, s_screenH);
		gl->glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
		gl->glEnable(GL_DEPTH_TEST);
		gl->glDepthFunc(GL_LEQUAL);
		gl->glDisable(GL_BLEND);
		gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl->glDisable(GL_ALPHA_TEST);
		gl->glDisable(GL_CULL_FACE);
		gl->glFrontFace(GL_CCW);
		gl->glTexEnvx(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		gl->glFogx(GL_FOG_MODE, 0x2601 /* GL_LINEAR */);
	}
	memset(&s_gl, 0, sizeof(s_gl));
	return true;
}

static void ShutdownEGL(void)
{
	if(g_Zeebo.pEGL && g_Zeebo.pEGL->pvt && s_eglDisplay){
		const IEGLVtbl *egl = g_Zeebo.pEGL->pvt;
		egl->eglMakeCurrent(s_eglDisplay, ZEEBO_EGL_NO_SURFACE, ZEEBO_EGL_NO_SURFACE, ZEEBO_EGL_NO_CONTEXT);
		if(s_eglContext) egl->eglDestroyContext(s_eglDisplay, s_eglContext);
		if(s_eglSurface) egl->eglDestroySurface(s_eglDisplay, s_eglSurface);
		egl->eglTerminate(s_eglDisplay);
	}
	s_eglDisplay = NULL;
	s_eglSurface = NULL;
	s_eglContext = NULL;
}

static void initStateDefaults(void)
{
	memset(&s_state, 0, sizeof(s_state));
	s_state.zTest        = 1;
	s_state.zWrite       = 1;
	s_state.srcBlend     = BLENDSRCALPHA;
	s_state.dstBlend     = BLENDINVSRCALPHA;
	s_state.cullMode     = CULLNONE;
	s_state.texAddressU  = Texture::WRAP;
	s_state.texAddressV  = Texture::WRAP;
	s_state.texFilter    = Texture::LINEAR;
	s_state.alphaTestFunc = ALPHAGREATEREQUAL;
	s_state.alphaTestRef  = 0;
	s_state.gsAlphaTest   = 0;
	s_state.gsAlphaTestRef = 128;
	s_state.shadeMode    = 0;
	s_state.fogType      = 0;
	s_state.fogDensity   = 1.0f;
}

static void beginUpdate(Camera *cam)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !cam) return;

	engine->currentCamera = cam;

	Raster *fb = cam->frameBuffer;
	int w = fb ? fb->width : s_screenW;
	int h = fb ? fb->height : s_screenH;
	if(w <= 0) w = s_screenW;
	if(h <= 0) h = s_screenH;

	gl->glViewport(0, 0, w, h);

	// Projection matrix (column-major)
	Matrix proj;
	float32 *p = (float32*)&proj;
	memset(p, 0, sizeof(float32)*16);

	float32 viewWindowX = cam->viewWindow.x;
	float32 viewWindowY = cam->viewWindow.y;
	float32 n = cam->nearPlane;
	float32 f = cam->farPlane;

	if(cam->projection == Camera::PERSPECTIVE){
		float32 recipZ = 1.0f / (f - n);
		p[0]  = -1.0f / viewWindowX;
		p[5]  =  1.0f / viewWindowY;
		p[8]  =  cam->viewOffset.x / viewWindowX;
		p[9]  = -cam->viewOffset.y / viewWindowY;
		p[10] = (f + n) * recipZ;
		p[11] = 1.0f;
		p[14] = -2.0f * f * n * recipZ;
	}else{
		float32 recipZ = 1.0f / (f - n);
		p[0]  = -1.0f / viewWindowX;
		p[5]  =  1.0f / viewWindowY;
		p[10] = 2.0f * recipZ;
		p[12] =  cam->viewOffset.x / viewWindowX;
		p[13] = -cam->viewOffset.y / viewWindowY;
		p[14] = -(f + n) * recipZ;
		p[15] = 1.0f;
	}

	GLfixed fx[16];
	for(int i = 0; i < 16; i++) fx[i] = ZeeboToFixed(p[i]);
	gl->glMatrixMode(GL_PROJECTION);
	gl->glLoadMatrixx(fx);

	// View matrix
	Matrix view;
	Matrix::invertOrthonormal(&view, cam->getFrame()->getLTM());
	// RW camera looks down +Z; GL looks down -Z so negate Z row/col or use standard RW->GL
	RawMatrix rm;
	convMatrix(&rm, &view);
	float32 *vm = (float32*)&rm;
	for(int i = 0; i < 16; i++) fx[i] = ZeeboToFixed(vm[i]);
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glLoadMatrixx(fx);
	invalidateLightCache();

	// Always program linear fog parameters so enabling GL_FOG mid-frame never falls back to GL_EXP
	gl->glFogx(GL_FOG_MODE, 0x2601 /* GL_LINEAR */);
	gl->glFogx(GL_FOG_START, ZeeboToFixed(cam->fogPlane));
	gl->glFogx(GL_FOG_END,   ZeeboToFixed(cam->farPlane));
	{
		GLfixed fogCol[4] = {
			(GLfixed)(((s_state.fogColor >> 16) & 0xFFu) * 257u),
			(GLfixed)(((s_state.fogColor >>  8) & 0xFFu) * 257u),
			(GLfixed)(((s_state.fogColor >>  0) & 0xFFu) * 257u),
			(GLfixed)(((s_state.fogColor >> 24) & 0xFFu) * 257u)
		};
		gl->glFogxv(GL_FOG_COLOR, fogCol);
	}
	if(s_state.fogEnable){
		gl->glEnable(GL_FOG);
	}else{
		gl->glDisable(GL_FOG);
	}
}

static void endUpdate(Camera *cam)
{
	(void)cam;
}

static void clearCamera(Camera *cam, RGBA *col, uint32 mode)
{
	(void)cam;
	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;

	uint32 mask = 0;
	if(mode & Camera::CLEARIMAGE){
		if(col){
			gl->glClearColorx(
				ZeeboToFixed(col->red   / 255.0f),
				ZeeboToFixed(col->green / 255.0f),
				ZeeboToFixed(col->blue  / 255.0f),
				ZeeboToFixed(col->alpha / 255.0f)
			);
		}
		mask |= GL_COLOR_BUFFER_BIT;
	}
	if(mode & Camera::CLEARZ){
		gl->glDepthMask(GL_TRUE);
		gl->glClearDepthx(ZeeboToFixed(1.0f));
		mask |= GL_DEPTH_BUFFER_BIT;
	}
	if(mode & Camera::CLEARSTENCIL){
		gl->glClearStencil(0);
		mask |= GL_STENCIL_BUFFER_BIT;
	}
	if(mask){
		gl->glClear(mask);
	}
	if(!s_state.zWrite){
		gl->glDepthMask(GL_FALSE);
	}
}

static void showRaster(Raster *raster, uint32 flags)
{
	(void)raster;
	(void)flags;
	if(g_Zeebo.pEGL && g_Zeebo.pEGL->pvt && s_eglDisplay && s_eglSurface){
		g_Zeebo.pEGL->pvt->eglSwapBuffers(s_eglDisplay, s_eglSurface);
	}
}

static bool32 rasterRenderFast(Raster *raster, int32 x, int32 y)
{
	(void)raster;
	(void)x;
	(void)y;
	return 0;
}

static uint32 WrapMap(int32 addr)
{
	return (addr == Texture::CLAMP || addr == Texture::BORDER) ? 0x812Fu /* GL_CLAMP_TO_EDGE */ : 0x2901u /* GL_REPEAT */;
}

static uint32 FilterMap(int32 flt)
{
	(void)flt;
	// Nearest-neighbor filtering (1 texel fetch/px vs 4 for bilinear) for Adreno 130 / Zeebo fillrate
	return GL_NEAREST;
}

static void ApplyBoundTexParams(const IGLVtbl *gl, Raster *r)
{
	if(!gl || !r) return;
	Gles1Raster *nat = GETGLES1RASTEREXT(r);
	if(!nat->texid) return;
	uint32 wrapU  = WrapMap(s_state.texAddressU);
	uint32 wrapV  = WrapMap(s_state.texAddressV);
	uint32 magFlt = FilterMap(s_state.texFilter);
	uint32 minFlt = magFlt;
	if(nat->numLevels > 1){
		minFlt = (magFlt == GL_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
	}
	if(nat->addressU != wrapU){
		nat->addressU = wrapU;
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLfixed)wrapU);
	}
	if(nat->addressV != wrapV){
		nat->addressV = wrapV;
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLfixed)wrapV);
	}
	if(nat->minFilter != minFlt){
		nat->minFilter = minFlt;
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLfixed)minFlt);
	}
	if(nat->magFilter != magFlt){
		nat->magFilter = magFlt;
		gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLfixed)magFlt);
	}
}

static void setRenderState(int32 state, void *pvalue)
{
	const IGLVtbl *gl = ZeeboGL();
	uintptr value = (uintptr)pvalue;

	switch(state){
	case TEXTURERASTER:
		if(s_state.textureRaster == (Texture*)pvalue){
			// Still ensure boundTexId wasn't clobbered by a texture upload
			Raster *r = (Raster*)pvalue;
			if(r){
				Gles1Raster *nat = GETGLES1RASTEREXT(r);
				if(gl && s_gl.boundTexId != nat->texid){
					gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
					s_gl.boundTexId = nat->texid;
				}
			}
			break;
		}
		s_state.textureRaster = (Texture*)pvalue;
		if(gl){
			Raster *r = (Raster*)pvalue;
			if(r){
				Gles1Raster *nat = GETGLES1RASTEREXT(r);
				s_state.textureAlpha = nat->hasAlpha ? 1 : 0;
				if(!s_gl.texEnabled){
					s_gl.texEnabled = true;
					gl->glEnable(GL_TEXTURE_2D);
				}
				if(s_gl.boundTexId != nat->texid){
					s_gl.boundTexId = nat->texid;
					gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
				}
				ApplyBoundTexParams(gl, r);
			}else{
				s_state.textureAlpha = 0;
				if(s_gl.texEnabled){
					s_gl.texEnabled = false;
					gl->glDisable(GL_TEXTURE_2D);
				}
			}
			UpdateBlendState(gl);
		}
		break;
	case TEXTUREADDRESS:
		if(s_state.texAddressU == (int32)value && s_state.texAddressV == (int32)value) break;
		s_state.texAddressU = (int32)value;
		s_state.texAddressV = (int32)value;
		if(gl && s_state.textureRaster) ApplyBoundTexParams(gl, (Raster*)s_state.textureRaster);
		break;
	case TEXTUREADDRESSU:
		if(s_state.texAddressU == (int32)value) break;
		s_state.texAddressU = (int32)value;
		if(gl && s_state.textureRaster) ApplyBoundTexParams(gl, (Raster*)s_state.textureRaster);
		break;
	case TEXTUREADDRESSV:
		if(s_state.texAddressV == (int32)value) break;
		s_state.texAddressV = (int32)value;
		if(gl && s_state.textureRaster) ApplyBoundTexParams(gl, (Raster*)s_state.textureRaster);
		break;
	case TEXTUREFILTER:
		if(s_state.texFilter == (int32)value) break;
		s_state.texFilter = (int32)value;
		if(gl && s_state.textureRaster) ApplyBoundTexParams(gl, (Raster*)s_state.textureRaster);
		break;
	case ZTESTENABLE:
		if(s_state.zTest == (bool32)value) break;
		s_state.zTest = (bool32)value;
		UpdateDepthState(gl);
		break;
	case ZWRITEENABLE:
		if(s_state.zWrite == (bool32)value) break;
		s_state.zWrite = (bool32)value;
		if(gl) gl->glDepthMask(value ? GL_TRUE : GL_FALSE);
		UpdateDepthState(gl);
		break;
	case VERTEXALPHA:
		if(s_state.vertexAlpha == (bool32)value) break;
		s_state.vertexAlpha = (bool32)value;
		UpdateBlendState(gl);
		break;
	case SRCBLEND:
		if(s_state.srcBlend == (int32)value) break;
		s_state.srcBlend = (int32)value;
		if(gl) gl->glBlendFunc(BlendMap(s_state.srcBlend), BlendMap(s_state.dstBlend));
		break;
	case DESTBLEND:
		if(s_state.dstBlend == (int32)value) break;
		s_state.dstBlend = (int32)value;
		if(gl) gl->glBlendFunc(BlendMap(s_state.srcBlend), BlendMap(s_state.dstBlend));
		break;
	case CULLMODE:
		if(s_state.cullMode == (int32)value) break;
		s_state.cullMode = (int32)value;
		if(gl){
			if(value == CULLNONE){
				gl->glDisable(GL_CULL_FACE);
			}else{
				gl->glEnable(GL_CULL_FACE);
				gl->glCullFace(value == CULLBACK ? GL_BACK : GL_FRONT);
			}
		}
		break;
	case FOGENABLE:
		if(s_state.fogEnable == (bool32)value) break;
		s_state.fogEnable = (bool32)value;
		if(gl){
			if(value){
				gl->glFogx(GL_FOG_MODE, 0x2601 /* GL_LINEAR */);
				if(engine->currentCamera){
					gl->glFogx(GL_FOG_START, ZeeboToFixed(engine->currentCamera->fogPlane));
					gl->glFogx(GL_FOG_END,   ZeeboToFixed(engine->currentCamera->farPlane));
				}
				gl->glEnable(GL_FOG);
			}else{
				gl->glDisable(GL_FOG);
			}
		}
		break;
	case FOGCOLOR:
		if(s_state.fogColor == (uint32)value) break;
		s_state.fogColor = (uint32)value;
		if(gl){
			GLfixed fogCol[4] = {
				(GLfixed)(((s_state.fogColor >> 16) & 0xFFu) * 257u),
				(GLfixed)(((s_state.fogColor >>  8) & 0xFFu) * 257u),
				(GLfixed)(((s_state.fogColor >>  0) & 0xFFu) * 257u),
				(GLfixed)(((s_state.fogColor >> 24) & 0xFFu) * 257u)
			};
			gl->glFogxv(GL_FOG_COLOR, fogCol);
		}
		break;
	case ALPHATESTFUNC:
		if(s_state.alphaTestFunc == (int32)value) break;
		s_state.alphaTestFunc = (int32)value;
		UpdateAlphaTestState(gl);
		break;
	case ALPHATESTREF:
		if(s_state.alphaTestRef == (int32)value) break;
		s_state.alphaTestRef = (int32)value;
		UpdateAlphaTestState(gl);
		break;
	case GSALPHATEST:
		s_state.gsAlphaTest = (bool32)value;
		break;
	case GSALPHATESTREF:
		s_state.gsAlphaTestRef = (int32)value;
		break;
	default:
		break;
	}
}

static void *getRenderState(int32 state)
{
	switch(state){
	case TEXTURERASTER:  return (void*)s_state.textureRaster;
	case TEXTUREADDRESS: return (void*)(uintptr)s_state.texAddressU;
	case TEXTUREADDRESSU:return (void*)(uintptr)s_state.texAddressU;
	case TEXTUREADDRESSV:return (void*)(uintptr)s_state.texAddressV;
	case TEXTUREFILTER:  return (void*)(uintptr)s_state.texFilter;
	case ZTESTENABLE:    return (void*)(uintptr)s_state.zTest;
	case ZWRITEENABLE:   return (void*)(uintptr)s_state.zWrite;
	case VERTEXALPHA:    return (void*)(uintptr)s_state.vertexAlpha;
	case SRCBLEND:       return (void*)(uintptr)s_state.srcBlend;
	case DESTBLEND:      return (void*)(uintptr)s_state.dstBlend;
	case CULLMODE:       return (void*)(uintptr)s_state.cullMode;
	case FOGENABLE:      return (void*)(uintptr)s_state.fogEnable;
	case FOGCOLOR:       return (void*)(uintptr)s_state.fogColor;
	case ALPHATESTFUNC:  return (void*)(uintptr)s_state.alphaTestFunc;
	case ALPHATESTREF:   return (void*)(uintptr)s_state.alphaTestRef;
	case GSALPHATEST:    return (void*)(uintptr)s_state.gsAlphaTest;
	case GSALPHATESTREF: return (void*)(uintptr)s_state.gsAlphaTestRef;
	default:             return nil;
	}
}

static int deviceSystem(DeviceReq req, void *arg0, int32 n)
{
	(void)n;
	switch(req){
	case DEVICEOPEN: {
		EngineOpenParams *p = (EngineOpenParams*)arg0;
		initStateDefaults();
		bool ok = InitEGL(p ? p->width : 640, p ? p->height : 480);
		Zeebo_Log("[GLES1] DEVICEOPEN ok=%d\n", ok ? 1 : 0);
		return ok ? 1 : 0;
	}
	case DEVICECLOSE:
		ShutdownEGL();
		return 1;
	case DEVICEINIT:
		Zeebo_Log("[GLES1] DEVICEINIT\n");
		engine->driver[PLATFORM_GL3]->defaultPipeline = makeDefaultPipeline();
		engine->driver[PLATFORM_GL3]->rasterCreate          = rasterCreate;
		engine->driver[PLATFORM_GL3]->rasterLock            = rasterLock;
		engine->driver[PLATFORM_GL3]->rasterUnlock          = rasterUnlock;
		engine->driver[PLATFORM_GL3]->rasterNumLevels       = rasterNumLevels;
		engine->driver[PLATFORM_GL3]->imageFindRasterFormat = imageFindRasterFormat;
		engine->driver[PLATFORM_GL3]->rasterFromImage       = rasterFromImage;
		engine->driver[PLATFORM_GL3]->rasterToImage         = rasterToImage;
		return 1;
	case DEVICEFINALIZE:
		// Must run AFTER Driver::s_plglist[PLATFORM_NULL].construct() (matfxOpen / skinOpen)
		matFXGlobals.pipelines[PLATFORM_GL3] = makeMatFXPipeline();
		skinGlobals.pipelines[PLATFORM_GL3]  = makeSkinPipeline();
		return 1;
	case DEVICETERM:
		return 1;
	case DEVICEGETNUMSUBSYSTEMS:
		return 1;
	case DEVICEGETCURRENTSUBSYSTEM:
		return 0;
	case DEVICESETSUBSYSTEM:
		return 1;
	case DEVICEGETSUBSSYSTEMINFO: {
		SubSystemInfo *info = (SubSystemInfo*)arg0;
		if(info) strncpy(info->name, "Zeebo Adreno 130 (GLES 1.1)", sizeof(info->name));
		return 1;
	}
	case DEVICEGETNUMVIDEOMODES:
		return 1;
	case DEVICEGETCURRENTVIDEOMODE:
		return 0;
	case DEVICESETVIDEOMODE:
		return 1;
	case DEVICEGETVIDEOMODEINFO: {
		VideoMode *vm = (VideoMode*)arg0;
		if(vm){
			vm->width  = s_screenW;
			vm->height = s_screenH;
			vm->depth  = 16;
			vm->flags  = VIDEOMODEEXCLUSIVE;
		}
		return 1;
	}
	default:
		return 1;
	}
}

Device renderdevice = {
	-1.0f, 1.0f,
	beginUpdate,
	endUpdate,
	clearCamera,
	showRaster,
	rasterRenderFast,
	setRenderState,
	getRenderState,
	im2DRenderLine,
	im2DRenderTriangle,
	im2DRenderPrimitive,
	im2DRenderIndexedPrimitive,
	im3DTransform,
	im3DRenderPrimitive,
	im3DRenderIndexedPrimitive,
	im3DEnd,
	deviceSystem
};

void registerPlatformPlugins(void)
{
	nativeRasterOffset = Raster::registerPlugin(sizeof(Gles1Raster),
		ID_RASTERGL3,
		[](void *object, int32 offset, int32 size) -> void* {
			(void)size;
			Gles1Raster *nat = PLUGINOFFSET(Gles1Raster, object, offset);
			memset(nat, 0, sizeof(*nat));
			return object;
		},
		[](void *object, int32 offset, int32 size) -> void* {
			(void)size;
			Gles1Raster *nat = PLUGINOFFSET(Gles1Raster, object, offset);
			if(s_state.textureRaster == (Texture*)object){
				s_state.textureRaster = nil;
			}
			if(nat->texid){
				if(s_gl.boundTexId == nat->texid){
					s_gl.boundTexId = 0;
				}
				if(const IGLVtbl *gl = ZeeboGL()){
					gl->glDeleteTextures(1, &nat->texid);
				}
				nat->texid = 0;
			}
			if(nat->pixels){
				rwFree(nat->pixels);
				nat->pixels = nil;
			}
			return object;
		},
		[](void *dst, const void *src, int32 offset, int32 size) -> void* {
			(void)dst; (void)src; (void)offset; (void)size;
			return dst;
		}
	);
	Geometry::registerPlugin(0, ID_NATIVEDATA, nil, destroyGles1NativeData, nil);
}

} // namespace gles1
} // namespace rw

#endif // RW_GLES1
