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

#ifdef RW_GLES1
#include "rwgles1.h"
#include "zeebo_gles1.h"

extern "C" {
extern uint32_t g_zeeboDrawCalls;
extern uint32_t g_zeeboTris;
}

namespace rw {
namespace gles1 {

static uint32 PrimTypeMap(PrimitiveType primType)
{
	switch(primType){
	case PRIMTYPELINELIST:     return GL_LINES;
	case PRIMTYPEPOLYLINE:     return GL_LINE_STRIP;
	case PRIMTYPETRILIST:      return GL_TRIANGLES;
	case PRIMTYPETRISTRIP:     return GL_TRIANGLE_STRIP;
	case PRIMTYPETRIFAN:       return GL_TRIANGLE_FAN;
	default:                   return GL_TRIANGLES;
	}
}

static void DrawIm2DVerts(uint32 glPrim, RWDEVICE::Im2DVertex *verts, int32 numVertices, uint16 *indices, int32 numIndices)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !verts || numVertices <= 0) return;

	Camera *cam = engine->currentCamera;
	int w = (cam && cam->frameBuffer) ? cam->frameBuffer->width  : 640;
	int h = (cam && cam->frameBuffer) ? cam->frameBuffer->height : 480;
	if(w <= 0) w = 640;
	if(h <= 0) h = 480;

	// Orthographic projection mapping x:[0,w]->[-1,1], y:[0,h]->[1,-1], z:[-1,1]->[-1,1]
	gl->glMatrixMode(GL_PROJECTION);
	gl->glPushMatrix();
	gl->glLoadIdentity();
	gl->glOrthox(0, (GLfixed)(w << 16), (GLfixed)(h << 16), 0, (GLfixed)(1 << 16), (GLfixed)(-1 << 16));

	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPushMatrix();
	gl->glLoadIdentity();

	bool hadCull = ((int32)(uintptr)GetRenderState(CULLMODE) != CULLNONE);
	bool hadFog  = (GetRenderState(FOGENABLE) != nil);
	if(hadCull) gl->glDisable(GL_CULL_FACE);
	if(hadFog)  gl->glDisable(GL_FOG);

	bool hasTex = isTextureEnabled();
	setLightingState(false, false, false);
	setClientStates(true, false, true, hasTex);

	const int stride = (int)sizeof(RWDEVICE::Im2DVertex);
	gl->glVertexPointer(3, GL_FLOAT, stride, &verts[0].x);
	gl->glColorPointer(4, GL_UNSIGNED_BYTE, stride, &verts[0].r);
	if(hasTex){
		gl->glTexCoordPointer(2, GL_FLOAT, stride, &verts[0].u);
	}

	if(indices && numIndices > 0){
		gl->glDrawElements(glPrim, numIndices, GL_UNSIGNED_SHORT, indices);
		g_zeeboDrawCalls++;
		g_zeeboTris += (glPrim == GL_TRIANGLES) ? (uint32_t)(numIndices / 3) : (numIndices > 2 ? (uint32_t)(numIndices - 2) : 0);
	}else{
		gl->glDrawArrays(glPrim, 0, numVertices);
		g_zeeboDrawCalls++;
		g_zeeboTris += (glPrim == GL_TRIANGLES) ? (uint32_t)(numVertices / 3) : (numVertices > 2 ? (uint32_t)(numVertices - 2) : 0);
	}

	if(hadCull) gl->glEnable(GL_CULL_FACE);
	if(hadFog)  gl->glEnable(GL_FOG);

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPopMatrix();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPopMatrix();
}

void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	(void)numVertices;
	RWDEVICE::Im2DVertex *v = (RWDEVICE::Im2DVertex*)vertices;
	uint16 idx[2] = { (uint16)vert1, (uint16)vert2 };
	DrawIm2DVerts(GL_LINES, v, numVertices > 0 ? numVertices : 2, idx, 2);
}

void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	(void)numVertices;
	RWDEVICE::Im2DVertex *v = (RWDEVICE::Im2DVertex*)vertices;
	uint16 idx[3] = { (uint16)vert1, (uint16)vert2, (uint16)vert3 };
	DrawIm2DVerts(GL_TRIANGLES, v, numVertices > 0 ? numVertices : 3, idx, 3);
}

void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices)
{
	DrawIm2DVerts(PrimTypeMap(primType), (RWDEVICE::Im2DVertex*)vertices, numVertices, nil, 0);
}

void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices, int32 numVertices, void *indices, int32 numIndices)
{
	DrawIm2DVerts(PrimTypeMap(primType), (RWDEVICE::Im2DVertex*)vertices, numVertices, (uint16*)indices, numIndices);
}

// ---------------------------------------------------------------------------
// Im3D (zero-copy directly from RWDEVICE::Im3DVertex)
// ---------------------------------------------------------------------------
static RWDEVICE::Im3DVertex *s_im3dVerts = nil;
static int32                 s_im3dNum   = 0;
static bool                  s_im3dPushed = false;

void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags)
{
	(void)flags;
	s_im3dVerts = (RWDEVICE::Im3DVertex*)vertices;
	s_im3dNum   = numVertices;

	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;

	if(world){
		gl->glMatrixMode(GL_MODELVIEW);
		gl->glPushMatrix();
		RawMatrix rm;
		convMatrix(&rm, world);
		GLfixed fx[16];
		const float32 *m = (const float32*)&rm;
		for(int i = 0; i < 16; i++) fx[i] = ZeeboToFixed(m[i]);
		gl->glMultMatrixx(fx);
		s_im3dPushed = true;
	}else{
		s_im3dPushed = false;
	}
}

static inline void BindIm3DPointers(const IGLVtbl *gl)
{
	bool hasTex = isTextureEnabled();
	setLightingState(false, false, false);
	setClientStates(true, false, true, hasTex);

	const int stride = (int)sizeof(RWDEVICE::Im3DVertex);
	gl->glVertexPointer(3, GL_FLOAT, stride, &s_im3dVerts[0].position.x);
	gl->glColorPointer(4, GL_UNSIGNED_BYTE, stride, &s_im3dVerts[0].r);
	if(hasTex){
		gl->glTexCoordPointer(2, GL_FLOAT, stride, &s_im3dVerts[0].u);
	}
}

void im3DRenderPrimitive(PrimitiveType primType)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !s_im3dVerts || s_im3dNum <= 0) return;

	BindIm3DPointers(gl);
	uint32 mode = PrimTypeMap(primType);
	gl->glDrawArrays(mode, 0, s_im3dNum);
	g_zeeboDrawCalls++;
	g_zeeboTris += (mode == GL_TRIANGLES) ? (uint32_t)(s_im3dNum / 3) : (s_im3dNum > 2 ? (uint32_t)(s_im3dNum - 2) : 0);
}

void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !s_im3dVerts || s_im3dNum <= 0 || !indices || numIndices <= 0) return;

	BindIm3DPointers(gl);
	uint32 mode = PrimTypeMap(primType);
	gl->glDrawElements(mode, numIndices, GL_UNSIGNED_SHORT, indices);
	g_zeeboDrawCalls++;
	g_zeeboTris += (mode == GL_TRIANGLES) ? (uint32_t)(numIndices / 3) : (numIndices > 2 ? (uint32_t)(numIndices - 2) : 0);
}

void im3DEnd(void)
{
	if(s_im3dPushed){
		if(const IGLVtbl *gl = ZeeboGL()){
			gl->glMatrixMode(GL_MODELVIEW);
			gl->glPopMatrix();
		}
		s_im3dPushed = false;
	}
	s_im3dVerts = nil;
	s_im3dNum   = 0;
}

} // namespace gles1
} // namespace rw

#endif // RW_GLES1
