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

namespace rw {
namespace gles1 {

struct Im2DVertGles1 {
	float32 x, y, z;
	float32 r, g, b, a;
	float32 u, v;
};

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

static void SetupIm2DMatrices(Camera *cam, float32 *sx, float32 *sy)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;

	int w = cam && cam->frameBuffer ? cam->frameBuffer->width  : 640;
	int h = cam && cam->frameBuffer ? cam->frameBuffer->height : 480;
	if(w <= 0) w = 640;
	if(h <= 0) h = 480;

	*sx = 2.0f / (float32)w;
	*sy = 2.0f / (float32)h;

	gl->glMatrixMode(GL_PROJECTION);
	gl->glPushMatrix();
	gl->glLoadIdentity();

	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPushMatrix();
	gl->glLoadIdentity();
}

static void RestoreIm2DMatrices(void)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl) return;
	gl->glMatrixMode(GL_PROJECTION);
	gl->glPopMatrix();
	gl->glMatrixMode(GL_MODELVIEW);
	gl->glPopMatrix();

	if(GetRenderState(ZTESTENABLE)) gl->glEnable(GL_DEPTH_TEST);
	else                            gl->glDisable(GL_DEPTH_TEST);

	int32 cull = (int32)(uintptr)GetRenderState(CULLMODE);
	if(cull == CULLNONE) gl->glDisable(GL_CULL_FACE);
	else                 gl->glEnable(GL_CULL_FACE);

	if(GetRenderState(FOGENABLE))   gl->glEnable(GL_FOG);
	else                            gl->glDisable(GL_FOG);
}

static void DrawIm2DVerts(uint32 glPrim, RWDEVICE::Im2DVertex *verts, int32 numVertices, uint16 *indices, int32 numIndices)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !verts || numVertices <= 0) return;

	static Im2DVertGles1 s_buf[8192];
	float32 sx = 1.0f, sy = 1.0f;
	SetupIm2DMatrices(engine->currentCamera, &sx, &sy);

	auto FillVert = [&](int32 dstIdx, const RWDEVICE::Im2DVertex &src) {
		float32 zndc = 2.0f * src.z - 1.0f;
		if(zndc < -1.0f) zndc = -1.0f;
		else if(zndc > 1.0f) zndc = 1.0f;
		s_buf[dstIdx].x =  src.x * sx - 1.0f;
		s_buf[dstIdx].y =  1.0f - src.y * sy;
		s_buf[dstIdx].z =  zndc;
		s_buf[dstIdx].r =  src.r / 255.0f;
		s_buf[dstIdx].g =  src.g / 255.0f;
		s_buf[dstIdx].b =  src.b / 255.0f;
		s_buf[dstIdx].a =  src.a / 255.0f;
		s_buf[dstIdx].u =  src.u;
		s_buf[dstIdx].v =  src.v;
	};

	int32 outVerts = 0;
	uint32 drawMode = GL_TRIANGLES;

	if(glPrim == GL_LINES){
		drawMode = GL_LINES;
		if(indices && numIndices > 0){
			for(int32 i = 0; i < numIndices && outVerts < 8192; i++){
				FillVert(outVerts++, verts[indices[i]]);
			}
		}else{
			for(int32 i = 0; i < numVertices && outVerts < 8192; i++){
				FillVert(outVerts++, verts[i]);
			}
		}
	}else if(glPrim == GL_LINE_STRIP){
		drawMode = GL_LINES;
		int32 cnt = (indices && numIndices > 0) ? numIndices : numVertices;
		for(int32 i = 0; i + 1 < cnt && outVerts + 2 <= 8192; i++){
			int32 i0 = indices ? indices[i] : i;
			int32 i1 = indices ? indices[i+1] : (i+1);
			FillVert(outVerts++, verts[i0]);
			FillVert(outVerts++, verts[i1]);
		}
	}else if(glPrim == GL_TRIANGLES){
		drawMode = GL_TRIANGLES;
		if(indices && numIndices > 0){
			for(int32 i = 0; i < numIndices && outVerts < 8192; i++){
				FillVert(outVerts++, verts[indices[i]]);
			}
		}else{
			for(int32 i = 0; i < numVertices && outVerts < 8192; i++){
				FillVert(outVerts++, verts[i]);
			}
		}
	}else if(glPrim == GL_TRIANGLE_FAN){
		drawMode = GL_TRIANGLES;
		int32 cnt = (indices && numIndices > 0) ? numIndices : numVertices;
		int32 i0 = indices ? indices[0] : 0;
		for(int32 i = 1; i + 1 < cnt && outVerts + 3 <= 8192; i++){
			int32 i1 = indices ? indices[i] : i;
			int32 i2 = indices ? indices[i+1] : (i+1);
			FillVert(outVerts++, verts[i0]);
			FillVert(outVerts++, verts[i1]);
			FillVert(outVerts++, verts[i2]);
		}
	}else if(glPrim == GL_TRIANGLE_STRIP){
		drawMode = GL_TRIANGLES;
		int32 cnt = (indices && numIndices > 0) ? numIndices : numVertices;
		for(int32 i = 0; i + 2 < cnt && outVerts + 3 <= 8192; i++){
			int32 i0 = indices ? indices[i] : i;
			int32 i1 = indices ? indices[i+1] : (i+1);
			int32 i2 = indices ? indices[i+2] : (i+2);
			if(i & 1){
				FillVert(outVerts++, verts[i1]);
				FillVert(outVerts++, verts[i0]);
				FillVert(outVerts++, verts[i2]);
			}else{
				FillVert(outVerts++, verts[i0]);
				FillVert(outVerts++, verts[i1]);
				FillVert(outVerts++, verts[i2]);
			}
		}
	}

	if(outVerts > 0){
		gl->glDisable(GL_LIGHTING);
		gl->glDisable(GL_CULL_FACE);
		if(GetRenderState(ZTESTENABLE)) gl->glEnable(GL_DEPTH_TEST);
		else                            gl->glDisable(GL_DEPTH_TEST);
		gl->glDisable(GL_FOG);
		gl->glEnableClientState(GL_VERTEX_ARRAY);
		gl->glEnableClientState(GL_COLOR_ARRAY);
		gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);

		gl->glVertexPointer(3, GL_FLOAT, sizeof(Im2DVertGles1), &s_buf[0].x);
		gl->glColorPointer(4, GL_FLOAT, sizeof(Im2DVertGles1), &s_buf[0].r);
		gl->glTexCoordPointer(2, GL_FLOAT, sizeof(Im2DVertGles1), &s_buf[0].u);

		gl->glDrawArrays(drawMode, 0, outVerts);

		gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		gl->glDisableClientState(GL_COLOR_ARRAY);
		gl->glDisableClientState(GL_VERTEX_ARRAY);
	}

	RestoreIm2DMatrices();
}

void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2)
{
	(void)numVertices;
	RWDEVICE::Im2DVertex *v = (RWDEVICE::Im2DVertex*)vertices;
	RWDEVICE::Im2DVertex tmp[2] = { v[vert1], v[vert2] };
	DrawIm2DVerts(GL_LINES, tmp, 2, nil, 0);
}

void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3)
{
	(void)numVertices;
	RWDEVICE::Im2DVertex *v = (RWDEVICE::Im2DVertex*)vertices;
	RWDEVICE::Im2DVertex tmp[3] = { v[vert1], v[vert2], v[vert3] };
	DrawIm2DVerts(GL_TRIANGLES, tmp, 3, nil, 0);
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
// Im3D
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

struct Im3DVertGles1 {
	float32 x, y, z;
	float32 r, g, b, a;
	float32 u, v;
};
static Im3DVertGles1 *s_im3dFloatBuf = nil;
static int32          s_im3dFloatCap = 0;

static bool PrepareIm3DFloatVerts(void)
{
	if(!s_im3dVerts || s_im3dNum <= 0) return false;
	if(s_im3dNum > s_im3dFloatCap){
		if(s_im3dFloatBuf) rwFree(s_im3dFloatBuf);
		s_im3dFloatBuf = (Im3DVertGles1*)rwMalloc((size_t)s_im3dNum * sizeof(Im3DVertGles1), MEMDUR_EVENT | ID_DRIVER);
		s_im3dFloatCap = s_im3dFloatBuf ? s_im3dNum : 0;
	}
	if(!s_im3dFloatBuf) return false;
	for(int32 i = 0; i < s_im3dNum; i++){
		s_im3dFloatBuf[i].x = s_im3dVerts[i].position.x;
		s_im3dFloatBuf[i].y = s_im3dVerts[i].position.y;
		s_im3dFloatBuf[i].z = s_im3dVerts[i].position.z;
		s_im3dFloatBuf[i].r = s_im3dVerts[i].r / 255.0f;
		s_im3dFloatBuf[i].g = s_im3dVerts[i].g / 255.0f;
		s_im3dFloatBuf[i].b = s_im3dVerts[i].b / 255.0f;
		s_im3dFloatBuf[i].a = s_im3dVerts[i].a / 255.0f;
		s_im3dFloatBuf[i].u = s_im3dVerts[i].u;
		s_im3dFloatBuf[i].v = s_im3dVerts[i].v;
	}
	return true;
}

void im3DRenderPrimitive(PrimitiveType primType)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !PrepareIm3DFloatVerts()) return;

	gl->glDisable(GL_LIGHTING);
	gl->glEnableClientState(GL_VERTEX_ARRAY);
	gl->glEnableClientState(GL_COLOR_ARRAY);
	gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	gl->glVertexPointer(3, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].x);
	gl->glColorPointer(4, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].r);
	gl->glTexCoordPointer(2, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].u);

	gl->glDrawArrays(PrimTypeMap(primType), 0, s_im3dNum);

	gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	gl->glDisableClientState(GL_COLOR_ARRAY);
	gl->glDisableClientState(GL_VERTEX_ARRAY);
}

void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !indices || numIndices <= 0 || !PrepareIm3DFloatVerts()) return;

	gl->glDisable(GL_LIGHTING);
	gl->glEnableClientState(GL_VERTEX_ARRAY);
	gl->glEnableClientState(GL_COLOR_ARRAY);
	gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	gl->glVertexPointer(3, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].x);
	gl->glColorPointer(4, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].r);
	gl->glTexCoordPointer(2, GL_FLOAT, sizeof(Im3DVertGles1), &s_im3dFloatBuf[0].u);

	gl->glDrawElements(PrimTypeMap(primType), numIndices, GL_UNSIGNED_SHORT, indices);

	gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	gl->glDisableClientState(GL_COLOR_ARRAY);
	gl->glDisableClientState(GL_VERTEX_ARRAY);
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
