// zeebo_gles1.h - OpenGL ES 1.0/1.1 constants and BREW IGL bridge for librw.
#pragma once

#include "zeebo_brew.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Standard OpenGL ES 1.0/1.1 constants
#define GL_FALSE                    0
#define GL_TRUE                     1
#define GL_ZERO                     0
#define GL_ONE                      1

#define GL_POINTS                   0x0000
#define GL_LINES                    0x0001
#define GL_LINE_LOOP                0x0002
#define GL_LINE_STRIP               0x0003
#define GL_TRIANGLES                0x0004
#define GL_TRIANGLE_STRIP           0x0005
#define GL_TRIANGLE_FAN             0x0006

#define GL_NEVER                    0x0200
#define GL_LESS                     0x0201
#define GL_EQUAL                    0x0202
#define GL_LEQUAL                   0x0203
#define GL_GREATER                  0x0204
#define GL_NOTEQUAL                 0x0205
#define GL_GEQUAL                   0x0206
#define GL_ALWAYS                   0x0207

#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307
#define GL_SRC_ALPHA_SATURATE       0x0308

#define GL_FRONT                    0x0404
#define GL_BACK                     0x0405
#define GL_FRONT_AND_BACK           0x0408

#define GL_CW                       0x0900
#define GL_CCW                      0x0901

#define GL_CULL_FACE                0x0B44
#define GL_LIGHTING                 0x0B50
#define GL_LIGHT_MODEL_AMBIENT      0x0B53
#define GL_LIGHT_MODEL_TWO_SIDE     0x0B52
#define GL_DEPTH_TEST               0x0B71
#define GL_STENCIL_TEST             0x0B90
#define GL_NORMALIZE                0x0BA1
#define GL_ALPHA_TEST               0x0BC0
#define GL_BLEND                    0x0BE2
#define GL_SCISSOR_TEST             0x0C11
#define GL_FOG                      0x0B60
#define GL_FOG_DENSITY              0x0B62
#define GL_FOG_START                0x0B63
#define GL_FOG_END                  0x0B64
#define GL_FOG_MODE                 0x0B65
#define GL_FOG_COLOR                0x0B66

#define GL_LINEAR_MIPMAP_NEAREST    0x2701
#define GL_NEAREST_MIPMAP_NEAREST   0x2700
#define GL_NEAREST_MIPMAP_LINEAR    0x2702
#define GL_LINEAR_MIPMAP_LINEAR     0x2703

#define GL_TEXTURE_2D               0x0DE1
#define GL_COLOR_BUFFER_BIT         0x00004000
#define GL_DEPTH_BUFFER_BIT         0x00000100
#define GL_STENCIL_BUFFER_BIT       0x00000400

#define GL_BYTE                     0x1400
#define GL_UNSIGNED_BYTE            0x1401
#define GL_SHORT                    0x1402
#define GL_UNSIGNED_SHORT           0x1403
#define GL_FLOAT                    0x1406
#define GL_FIXED                    0x140C

#define GL_ALPHA                    0x1906
#define GL_RGB                      0x1907
#define GL_RGBA                     0x1908
#define GL_LUMINANCE                0x1909
#define GL_LUMINANCE_ALPHA          0x190A

#define GL_UNSIGNED_SHORT_4_4_4_4   0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1   0x8034
#define GL_UNSIGNED_SHORT_5_6_5     0x8363

#define GL_NEAREST                  0x2600
#define GL_LINEAR                   0x2601
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_REPEAT                   0x2901

#define GL_MODELVIEW                0x1700
#define GL_PROJECTION               0x1701
#define GL_TEXTURE                  0x1702

#define GL_VERTEX_ARRAY             0x8074
#define GL_NORMAL_ARRAY             0x8075
#define GL_COLOR_ARRAY              0x8076
#define GL_TEXTURE_COORD_ARRAY      0x8078

#define GL_TEXTURE0                 0x84C0
#define GL_TEXTURE1                 0x84C1

#define GL_TEXTURE_ENV              0x2300
#define GL_TEXTURE_ENV_MODE         0x2200
#define GL_TEXTURE_ENV_COLOR        0x2201
#define GL_MODULATE                 0x2100
#define GL_DECAL                    0x2101
#define GL_REPLACE                  0x1E01
#define GL_ADD                      0x0104

#define GL_AMBIENT                  0x1200
#define GL_DIFFUSE                  0x1201
#define GL_SPECULAR                 0x1202
#define GL_POSITION                 0x1203
#define GL_SPOT_DIRECTION           0x1204
#define GL_SPOT_EXPONENT            0x1205
#define GL_SPOT_CUTOFF              0x1206
#define GL_CONSTANT_ATTENUATION     0x1207
#define GL_LINEAR_ATTENUATION       0x1208
#define GL_QUADRATIC_ATTENUATION    0x1209
#define GL_EMISSION                 0x1600
#define GL_SHININESS                0x1601
#define GL_AMBIENT_AND_DIFFUSE      0x1602
#define GL_COLOR_MATERIAL           0x0B57
#define GL_LIGHT0                   0x4000

#define GL_SMOOTH                   0x1D01
#define GL_FLAT                     0x1D00
#define GL_DONT_CARE                0x1100
#define GL_FASTEST                  0x1101
#define GL_NICEST                   0x1102
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50

// EGL constants
#define ZEEBO_EGL_DEFAULT_DISPLAY   0
#define ZEEBO_EGL_RED_SIZE          0x3024
#define ZEEBO_EGL_GREEN_SIZE        0x3023
#define ZEEBO_EGL_BLUE_SIZE         0x3022
#define ZEEBO_EGL_ALPHA_SIZE        0x3021
#define ZEEBO_EGL_DEPTH_SIZE        0x3025
#define ZEEBO_EGL_NONE              0x3038
#define ZEEBO_EGL_NO_CONTEXT        ((void*)0)
#define ZEEBO_EGL_NO_SURFACE        ((void*)0)
#define ZEEBO_EGL_NO_DISPLAY        ((void*)0)

typedef int32_t GLfixed;

typedef struct IGLVtbl {
    uint32_t (*AddRef)(void* po);
    uint32_t (*Release)(void* po);
    int      (*QueryInterface)(void* po, uint32_t id, void** ppNew);
    void (*glActiveTexture)(uint32_t texture);
    void (*glAlphaFuncx)(uint32_t func, GLfixed ref);
    void (*glBindTexture)(uint32_t target, uint32_t texture);
    void (*glBlendFunc)(uint32_t sfactor, uint32_t dfactor);
    void (*glClear)(uint32_t mask);
    void (*glClearColorx)(GLfixed r, GLfixed g, GLfixed b, GLfixed a);
    void (*glClearDepthx)(GLfixed depth);
    void (*glClearStencil)(int s);
    void (*glClientActiveTexture)(uint32_t texture);
    void (*glColor4x)(GLfixed r, GLfixed g, GLfixed b, GLfixed a);
    void (*glColorMask)(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void (*glColorPointer)(int size, uint32_t type, int stride, const void* ptr);
    void (*glCompressedTexImage2D)(uint32_t, int, uint32_t, int, int, int, int, const void*);
    void (*glCompressedTexSubImage2D)(uint32_t, int, int, int, int, int, uint32_t, int, const void*);
    void (*glCopyTexImage2D)(uint32_t, int, uint32_t, int, int, int, int, int);
    void (*glCopyTexSubImage2D)(uint32_t, int, int, int, int, int, int, int);
    void (*glCullFace)(uint32_t mode);
    void (*glDeleteTextures)(int n, const uint32_t* textures);
    void (*glDepthFunc)(uint32_t func);
    void (*glDepthMask)(uint8_t flag);
    void (*glDepthRangex)(GLfixed zNear, GLfixed zFar);
    void (*glDisable)(uint32_t cap);
    void (*glDisableClientState)(uint32_t array);
    void (*glDrawArrays)(uint32_t mode, int first, int count);
    void (*glDrawElements)(uint32_t mode, int count, uint32_t type, const void* indices);
    void (*glEnable)(uint32_t cap);
    void (*glEnableClientState)(uint32_t array);
    void (*glFinish)(void);
    void (*glFlush)(void);
    void (*glFogx)(uint32_t pname, GLfixed param);
    void (*glFogxv)(uint32_t pname, const GLfixed* params);
    void (*glFrontFace)(uint32_t mode);
    void (*glFrustumx)(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
    void (*glGenTextures)(int n, uint32_t* textures);
    uint32_t (*glGetError)(void);
    void (*glGetIntegerv)(uint32_t pname, int* params);
    const uint8_t* (*glGetString)(uint32_t name);
    void (*glHint)(uint32_t target, uint32_t mode);
    void (*glLightModelx)(uint32_t pname, GLfixed param);
    void (*glLightModelxv)(uint32_t pname, const GLfixed* params);
    void (*glLightx)(uint32_t light, uint32_t pname, GLfixed param);
    void (*glLightxv)(uint32_t light, uint32_t pname, const GLfixed* params);
    void (*glLineWidthx)(GLfixed width);
    void (*glLoadIdentity)(void);
    void (*glLoadMatrixx)(const GLfixed* m);
    void (*glLogicOp)(uint32_t opcode);
    void (*glMaterialx)(uint32_t face, uint32_t pname, GLfixed param);
    void (*glMaterialxv)(uint32_t face, uint32_t pname, const GLfixed* params);
    void (*glMatrixMode)(uint32_t mode);
    void (*glMultMatrixx)(const GLfixed* m);
    void (*glMultiTexCoord4x)(uint32_t target, GLfixed s, GLfixed t, GLfixed r, GLfixed q);
    void (*glNormal3x)(GLfixed nx, GLfixed ny, GLfixed nz);
    void (*glNormalPointer)(uint32_t type, int stride, const void* ptr);
    void (*glOrthox)(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f);
    void (*glPixelStorei)(uint32_t pname, int param);
    void (*glPointSizex)(GLfixed size);
    void (*glPolygonOffsetx)(GLfixed factor, GLfixed units);
    void (*glPopMatrix)(void);
    void (*glPushMatrix)(void);
    void (*glReadPixels)(int x, int y, int w, int h, uint32_t format, uint32_t type, void* pixels);
    void (*glRotatex)(GLfixed angle, GLfixed x, GLfixed y, GLfixed z);
    void (*glSampleCoveragex)(GLfixed value, uint8_t invert);
    void (*glScalex)(GLfixed x, GLfixed y, GLfixed z);
    void (*glScissor)(int x, int y, int width, int height);
    void (*glShadeModel)(uint32_t mode);
    void (*glStencilFunc)(uint32_t func, int ref, uint32_t mask);
    void (*glStencilMask)(uint32_t mask);
    void (*glStencilOp)(uint32_t fail, uint32_t zfail, uint32_t zpass);
    void (*glTexCoordPointer)(int size, uint32_t type, int stride, const void* ptr);
    void (*glTexEnvx)(uint32_t target, uint32_t pname, GLfixed param);
    void (*glTexEnvxv)(uint32_t target, uint32_t pname, const GLfixed* params);
    void (*glTexImage2D)(uint32_t target, int level, int internalformat, int width, int height, int border, uint32_t format, uint32_t type, const void* pixels);
    void (*glTexParameterx)(uint32_t target, uint32_t pname, GLfixed param);
    void (*glTexSubImage2D)(uint32_t target, int level, int xoffset, int yoffset, int width, int height, uint32_t format, uint32_t type, const void* pixels);
    void (*glTranslatex)(GLfixed x, GLfixed y, GLfixed z);
    void (*glVertexPointer)(int size, uint32_t type, int stride, const void* ptr);
    void (*glViewport)(int x, int y, int width, int height);
} IGLVtbl;

struct IGL {
    const IGLVtbl* pvt;
};

static inline const IGLVtbl* ZeeboGL(void) {
    return g_Zeebo.pGL ? ((IGL*)g_Zeebo.pGL)->pvt : NULL;
}

static inline GLfixed ZeeboToFixed(float f) {
    return (GLfixed)(f * 65536.0f);
}

#ifdef __cplusplus
}
#endif
