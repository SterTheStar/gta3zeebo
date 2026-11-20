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
extern uint32_t g_zeeboDrawCalls;
extern uint32_t g_zeeboTris;
}

namespace rw {
namespace gles1 {

static void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);

static void instanceNothing(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	(void)geo; (void)header; (void)reinstance;
}

static void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	if(reinstance){
		defaultUninstanceCB(geo, header);
	}

	MeshHeader *meshh = geo->meshHeader;
	header->serialNumber = meshh->serialNum;
	header->numMeshes    = meshh->numMeshes;
	header->primType     = (meshh->flags & MeshHeader::TRISTRIP) ? GL_TRIANGLE_STRIP : GL_TRIANGLES;

	uint32 totalIdx = 0;
	Mesh *meshes = meshh->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++)
		totalIdx += meshes[i].numIndices;

	V3d *pos  = geo->morphTargets[0].vertices;
	V3d *norm = ((geo->flags & Geometry::NORMALS) != 0) ? geo->morphTargets[0].normals : nil;
	RGBA *col = ((geo->flags & Geometry::PRELIT) != 0)  ? geo->colors : nil;
	TexCoords *uv = (geo->numTexCoordSets > 0) ? geo->texCoords[0] : nil;

	// Compact per-geometry 4-byte aligned native vertex layout:
	//   pos:    3 x GLfixed (12 B) [always]
	//   normal: 4 x int8    ( 4 B) [optional, GL_BYTE]
	//   color:  4 x uint8   ( 4 B) [optional, GL_UNSIGNED_BYTE]
	//   uv:     2 x GLfixed ( 8 B) [optional, GL_FIXED]
	// Stride: 12..28 bytes (down from 48 bytes)
	int32 off = 0;
	header->posOffset = off;
	off += 12;

	if(norm){
		header->normalOffset = off;
		off += 4;
	}else{
		header->normalOffset = -1;
	}

	if(col){
		header->colorOffset = off;
		off += 4;
	}else{
		header->colorOffset = -1;
	}

	if(uv){
		header->texCoordOffset = off;
		off += 8;
	}else{
		header->texCoordOffset = -1;
	}

	header->totalNumIndex  = totalIdx;
	header->totalNumVertex = geo->numVertices;
	header->stride         = (uint32)off;

	header->indexBuffer  = (uint16*)rwMalloc(totalIdx * sizeof(uint16), MEMDUR_EVENT | ID_GEOMETRY);
	header->vertexBuffer = (uint8*)rwMalloc((size_t)geo->numVertices * (size_t)header->stride, MEMDUR_EVENT | ID_GEOMETRY);
	header->inst         = (InstanceData*)rwMalloc(header->numMeshes * sizeof(InstanceData), MEMDUR_EVENT | ID_GEOMETRY);
	if(!header->indexBuffer || !header->vertexBuffer || !header->inst){
		defaultUninstanceCB(geo, header);
		return;
	}

	const uint32 stride = header->stride;
	const int32 normOff = header->normalOffset;
	const int32 colOff  = header->colorOffset;
	const int32 uvOff   = header->texCoordOffset;
	uint8 *dst = header->vertexBuffer;

	for(int32 i = 0; i < geo->numVertices; i++, dst += stride){
		GLfixed *pDst = (GLfixed*)dst;
		if(pos){
			pDst[0] = ZeeboToFixed(pos[i].x);
			pDst[1] = ZeeboToFixed(pos[i].y);
			pDst[2] = ZeeboToFixed(pos[i].z);
		}else{
			pDst[0] = pDst[1] = pDst[2] = 0;
		}
		if(normOff >= 0){
			int8_t *nDst = (int8_t*)(dst + normOff);
			nDst[0] = ZeeboNormToByte(norm[i].x);
			nDst[1] = ZeeboNormToByte(norm[i].y);
			nDst[2] = ZeeboNormToByte(norm[i].z);
			nDst[3] = 0;
		}
		if(colOff >= 0){
			uint8 *cDst = dst + colOff;
			cDst[0] = col[i].red;
			cDst[1] = col[i].green;
			cDst[2] = col[i].blue;
			cDst[3] = col[i].alpha;
		}
		if(uvOff >= 0){
			GLfixed *tDst = (GLfixed*)(dst + uvOff);
			tDst[0] = ZeeboToFixed(uv[i].u);
			tDst[1] = ZeeboToFixed(uv[i].v);
		}
	}

	uint32 idxOff = 0;
	for(uint32 i = 0; i < header->numMeshes; i++){
		InstanceData *inst = &header->inst[i];
		inst->numIndex    = meshes[i].numIndices;
		inst->minVert     = 0;
		inst->numVertices = geo->numVertices;
		inst->material    = meshes[i].material;
		inst->vertexAlpha = 0;
		if(col){
			for(uint32 k = 0; k < inst->numIndex; k++){
				if(col[meshes[i].indices[k]].alpha < 255){
					inst->vertexAlpha = 1;
					break;
				}
			}
		}
		inst->offset = idxOff * sizeof(uint16);

		memcpy(&header->indexBuffer[idxOff], meshes[i].indices, meshes[i].numIndices * sizeof(uint16));
		idxOff += meshes[i].numIndices;
	}
}

static void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header)
{
	(void)geo;
	if(header->indexBuffer)  rwFree(header->indexBuffer);
	if(header->vertexBuffer) rwFree(header->vertexBuffer);
	if(header->inst)         rwFree(header->inst);
	header->indexBuffer  = nil;
	header->vertexBuffer = nil;
	header->inst         = nil;
}

// Per-camera-pass light cache to avoid redundant glLightxv / glEnable / glDisable spam
struct LightCache {
	bool    valid;
	int     enabledCount;
	GLfixed amb[3];
	int     numDir;
	Light  *dir[8];
	int     numLoc;
	Light  *loc[8];
};

static LightCache s_lightCache = { false, 8, {0,0,0}, 0, {nil}, 0, {nil} };

void invalidateLightCache(void)
{
	s_lightCache.valid = false;
}

static void ApplyGles1Lights(const IGLVtbl *gl, Atomic *atomic)
{
	WorldLights lightData;
	Light *directionals[8];
	Light *locals[8];
	lightData.directionals = directionals;
	lightData.numDirectionals = 8;
	lightData.locals = locals;
	lightData.numLocals = 8;
	memset(&lightData.ambient, 0, sizeof(lightData.ambient));

	if(engine->currentWorld){
		((World*)engine->currentWorld)->enumerateLights(atomic, &lightData);
	}else{
		lightData.numDirectionals = 0;
		lightData.numLocals = 0;
		lightData.ambient.red = lightData.ambient.green = lightData.ambient.blue = lightData.ambient.alpha = 1.0f;
	}

	GLfixed amb[4] = {
		ZeeboToFixed(lightData.ambient.red),
		ZeeboToFixed(lightData.ambient.green),
		ZeeboToFixed(lightData.ambient.blue),
		ZeeboToFixed(1.0f)
	};

	// Fast path: if the light set and ambient color match the previous atomic, skip all GL calls
	if(s_lightCache.valid &&
	   s_lightCache.amb[0] == amb[0] &&
	   s_lightCache.amb[1] == amb[1] &&
	   s_lightCache.amb[2] == amb[2] &&
	   s_lightCache.numDir == lightData.numDirectionals &&
	   s_lightCache.numLoc == lightData.numLocals){
		bool same = true;
		for(int i = 0; i < lightData.numDirectionals; i++){
			if(s_lightCache.dir[i] != lightData.directionals[i]){ same = false; break; }
		}
		if(same){
			for(int i = 0; i < lightData.numLocals; i++){
				if(s_lightCache.loc[i] != lightData.locals[i]){ same = false; break; }
			}
		}
		if(same) return;
	}

	gl->glLightModelxv(GL_LIGHT_MODEL_AMBIENT, amb);

	int lightIdx = 0;
	static const GLfixed kZero[4] = { 0, 0, 0, 65536 };
	for(int i = 0; i < lightData.numDirectionals && lightIdx < 8; i++){
		Light *l = lightData.directionals[i];
		if(!l || !l->getFrame()) continue;
		uint32 lid = GL_LIGHT0 + (uint32)lightIdx;
		V3d at = l->getFrame()->getLTM()->at;
		GLfixed pos[4] = {
			ZeeboToFixed(-at.x),
			ZeeboToFixed(-at.y),
			ZeeboToFixed(-at.z),
			0
		};
		GLfixed diff[4] = {
			ZeeboToFixed(l->color.red),
			ZeeboToFixed(l->color.green),
			ZeeboToFixed(l->color.blue),
			65536
		};
		if(lightIdx >= s_lightCache.enabledCount || !s_lightCache.valid)
			gl->glEnable(lid);
		gl->glLightxv(lid, GL_POSITION, pos);
		gl->glLightxv(lid, GL_DIFFUSE,  diff);
		gl->glLightxv(lid, GL_AMBIENT,  kZero);
		gl->glLightxv(lid, GL_SPECULAR, kZero);
		lightIdx++;
	}
	for(int i = 0; i < lightData.numLocals && lightIdx < 8; i++){
		Light *l = lightData.locals[i];
		if(!l || !l->getFrame()) continue;
		uint32 lid = GL_LIGHT0 + (uint32)lightIdx;
		V3d p = l->getFrame()->getLTM()->pos;
		GLfixed pos[4] = {
			ZeeboToFixed(p.x),
			ZeeboToFixed(p.y),
			ZeeboToFixed(p.z),
			65536
		};
		GLfixed diff[4] = {
			ZeeboToFixed(l->color.red),
			ZeeboToFixed(l->color.green),
			ZeeboToFixed(l->color.blue),
			65536
		};
		if(lightIdx >= s_lightCache.enabledCount || !s_lightCache.valid)
			gl->glEnable(lid);
		gl->glLightxv(lid, GL_POSITION, pos);
		gl->glLightxv(lid, GL_DIFFUSE,  diff);
		gl->glLightxv(lid, GL_AMBIENT,  kZero);
		gl->glLightxv(lid, GL_SPECULAR, kZero);
		lightIdx++;
	}
	int disableTo = s_lightCache.valid ? s_lightCache.enabledCount : 8;
	for(int i = lightIdx; i < disableTo; i++){
		gl->glDisable(GL_LIGHT0 + (uint32)i);
	}

	s_lightCache.valid = true;
	s_lightCache.enabledCount = lightIdx;
	s_lightCache.amb[0] = amb[0];
	s_lightCache.amb[1] = amb[1];
	s_lightCache.amb[2] = amb[2];
	s_lightCache.numDir = lightData.numDirectionals;
	for(int i = 0; i < lightData.numDirectionals; i++) s_lightCache.dir[i] = lightData.directionals[i];
	s_lightCache.numLoc = lightData.numLocals;
	for(int i = 0; i < lightData.numLocals; i++) s_lightCache.loc[i] = lightData.locals[i];
}

static void RenderMeshesWithVerts(const IGLVtbl *gl, Atomic *atomic, InstanceDataHeader *header, uint8 *vertBuf)
{
	Geometry *geo = atomic->geometry;
	Frame *frame = atomic->getFrame();
	Matrix *ltm = frame ? frame->getLTM() : nil;

	bool hasNorm  = (header->normalOffset >= 0);
	bool hasColor = (header->colorOffset >= 0);
	bool hasTex   = (header->texCoordOffset >= 0);
	bool useLight = (geo->flags & Geometry::LIGHT) != 0 && hasNorm;

	gl->glMatrixMode(GL_MODELVIEW);
	if(useLight){
		// Set world-space lights while GL_MODELVIEW still holds only the view matrix
		ApplyGles1Lights(gl, atomic);
	}
	setLightingState(useLight, useLight, useLight && hasColor);

	if(ltm){
		gl->glPushMatrix();
		RawMatrix rm;
		convMatrix(&rm, ltm);
		GLfixed fx[16];
		const float32 *m = (const float32*)&rm;
		for(int i = 0; i < 16; i++) fx[i] = ZeeboToFixed(m[i]);
		gl->glMultMatrixx(fx);
	}

	const int stride = (int)header->stride;
	setClientStates(true, useLight, hasColor, hasTex);

	gl->glVertexPointer(3, GL_FIXED, stride, vertBuf + header->posOffset);
	if(useLight){
		gl->glNormalPointer(GL_BYTE, stride, vertBuf + header->normalOffset);
	}
	if(hasColor){
		gl->glColorPointer(4, GL_UNSIGNED_BYTE, stride, vertBuf + header->colorOffset);
	}
	if(hasTex){
		gl->glTexCoordPointer(2, GL_FIXED, stride, vertBuf + header->texCoordOffset);
	}

	for(uint32 i = 0; i < header->numMeshes; i++){
		InstanceData *inst = &header->inst[i];
		Material *mat = inst->material;
		if(mat){
			bool texHasAlpha = (mat->texture && mat->texture->raster &&
			                    GETGLES1RASTEREXT(mat->texture->raster)->hasAlpha);
			SetRenderState(VERTEXALPHA, (inst->vertexAlpha || mat->color.alpha != 0xFF || texHasAlpha) ? 1 : 0);
			if(!hasColor){
				setConstantColor(mat->color.red, mat->color.green, mat->color.blue, mat->color.alpha);
			}
			if(mat->texture && mat->texture->raster && hasTex){
				SetRenderState(TEXTUREADDRESSU, mat->texture->getAddressU());
				SetRenderState(TEXTUREADDRESSV, mat->texture->getAddressV());
				SetRenderState(TEXTUREFILTER,   mat->texture->getFilter());
				SetRenderState(TEXTURERASTER,   mat->texture->raster);
			}else{
				SetRenderState(TEXTURERASTER, nil);
			}
		}

		uint16 *idx = (uint16*)((uint8*)header->indexBuffer + inst->offset);
		gl->glDrawElements(header->primType, inst->numIndex, GL_UNSIGNED_SHORT, idx);
		g_zeeboDrawCalls++;
		g_zeeboTris += (header->primType == GL_TRIANGLE_STRIP)
			? (inst->numIndex > 2 ? inst->numIndex - 2 : 0)
			: (inst->numIndex / 3);
	}

	if(ltm){
		gl->glPopMatrix();
	}
}

static void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !header || !header->vertexBuffer || !header->indexBuffer) return;
	RenderMeshesWithVerts(gl, atomic, header, header->vertexBuffer);
}

static Matrix s_skinMatrices[64];
static uint8 *s_skinScratch = nil;
static size_t s_skinScratchCap = 0;

static void skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !header || !header->vertexBuffer || !header->indexBuffer) return;

	Geometry *geo = atomic->geometry;
	Skin *skin = Skin::get(geo);
	if(!skin || !skin->indices || !skin->weights || !skin->inverseMatrices){
		RenderMeshesWithVerts(gl, atomic, header, header->vertexBuffer);
		return;
	}

	HAnimHierarchy *hier = Skin::getHierarchy(atomic);
	int32 numBones = skin->numBones;
	if(numBones > 64) numBones = 64;
	if(hier && hier->matrices && atomic->getFrame()){
		Matrix *invMats = (Matrix*)skin->inverseMatrices;
		Matrix tmp;
		if(hier->flags & HAnimHierarchy::LOCALSPACEMATRICES){
			for(int32 i = 0; i < numBones && i < hier->numNodes; i++){
				invMats[i].flags = 0;
				Matrix::mult(&s_skinMatrices[i], &invMats[i], &hier->matrices[i]);
			}
		}else{
			Matrix invAtmMat;
			Matrix::invert(&invAtmMat, atomic->getFrame()->getLTM());
			for(int32 i = 0; i < numBones && i < hier->numNodes; i++){
				invMats[i].flags = 0;
				Matrix::mult(&tmp, &hier->matrices[i], &invAtmMat);
				Matrix::mult(&s_skinMatrices[i], &invMats[i], &tmp);
			}
		}
	}else{
		for(int32 i = 0; i < numBones; i++)
			s_skinMatrices[i].setIdentity();
	}

	const int32  nVerts = (int32)header->totalNumVertex;
	const uint32 stride = header->stride;
	const size_t needed = (size_t)nVerts * (size_t)stride;
	if(needed > s_skinScratchCap){
		if(s_skinScratch) rwFree(s_skinScratch);
		s_skinScratch = (uint8*)rwMalloc(needed, MEMDUR_EVENT | ID_SKIN);
		s_skinScratchCap = s_skinScratch ? needed : 0;
	}
	if(!s_skinScratch){
		RenderMeshesWithVerts(gl, atomic, header, header->vertexBuffer);
		return;
	}

	// Bulk-copy static attributes (colors, UVs) once
	memcpy(s_skinScratch, header->vertexBuffer, needed);

	const int32 normOff = header->normalOffset;
	const bool  doNorm  = ((geo->flags & Geometry::LIGHT) != 0) && (normOff >= 0);
	const V3d  *srcPos  = geo->morphTargets[0].vertices;
	const V3d  *srcNorm = doNorm ? geo->morphTargets[0].normals : nil;
	const uint8 *srcRaw = header->vertexBuffer;
	uint8 *dst = s_skinScratch;

	for(int32 i = 0; i < nVerts; i++, dst += stride, srcRaw += stride){
		float32 sx, sy, sz;
		if(srcPos){
			sx = srcPos[i].x; sy = srcPos[i].y; sz = srcPos[i].z;
		}else{
			const GLfixed *fp = (const GLfixed*)srcRaw;
			sx = fp[0] * (1.0f / 65536.0f);
			sy = fp[1] * (1.0f / 65536.0f);
			sz = fp[2] * (1.0f / 65536.0f);
		}

		const uint8   *idx = &skin->indices[i * 4];
		const float32 *wt  = &skin->weights[i * 4];
		float32 px, py, pz;

		// Fast path for single-bone vertices (~75% of GTA III ped vertices)
		if(wt[0] >= 0.999f && idx[0] < numBones){
			const Matrix &m = s_skinMatrices[idx[0]];
			px = sx * m.right.x + sy * m.up.x + sz * m.at.x + m.pos.x;
			py = sx * m.right.y + sy * m.up.y + sz * m.at.y + m.pos.y;
			pz = sx * m.right.z + sy * m.up.z + sz * m.at.z + m.pos.z;
			if(doNorm){
				float32 snx, sny, snz;
				if(srcNorm){
					snx = srcNorm[i].x; sny = srcNorm[i].y; snz = srcNorm[i].z;
				}else{
					const int8_t *np = (const int8_t*)(srcRaw + normOff);
					snx = np[0] * (1.0f / 127.0f);
					sny = np[1] * (1.0f / 127.0f);
					snz = np[2] * (1.0f / 127.0f);
				}
				int8_t *nDst = (int8_t*)(dst + normOff);
				nDst[0] = ZeeboNormToByte(snx * m.right.x + sny * m.up.x + snz * m.at.x);
				nDst[1] = ZeeboNormToByte(snx * m.right.y + sny * m.up.y + snz * m.at.y);
				nDst[2] = ZeeboNormToByte(snx * m.right.z + sny * m.up.z + snz * m.at.z);
			}
		}else{
			px = py = pz = 0.0f;
			float32 nx = 0.0f, ny = 0.0f, nz = 0.0f;
			float32 snx = 0.0f, sny = 0.0f, snz = 0.0f;
			if(doNorm){
				if(srcNorm){
					snx = srcNorm[i].x; sny = srcNorm[i].y; snz = srcNorm[i].z;
				}else{
					const int8_t *np = (const int8_t*)(srcRaw + normOff);
					snx = np[0] * (1.0f / 127.0f);
					sny = np[1] * (1.0f / 127.0f);
					snz = np[2] * (1.0f / 127.0f);
				}
			}
			for(int b = 0; b < 4; b++){
				float32 w = wt[b];
				if(w <= 0.0f) continue;
				uint8 bi = idx[b];
				if(bi >= numBones) continue;
				const Matrix &m = s_skinMatrices[bi];
				px += w * (sx * m.right.x + sy * m.up.x + sz * m.at.x + m.pos.x);
				py += w * (sx * m.right.y + sy * m.up.y + sz * m.at.y + m.pos.y);
				pz += w * (sx * m.right.z + sy * m.up.z + sz * m.at.z + m.pos.z);
				if(doNorm){
					nx += w * (snx * m.right.x + sny * m.up.x + snz * m.at.x);
					ny += w * (snx * m.right.y + sny * m.up.y + snz * m.at.y);
					nz += w * (snx * m.right.z + sny * m.up.z + snz * m.at.z);
				}
			}
			if(doNorm){
				int8_t *nDst = (int8_t*)(dst + normOff);
				nDst[0] = ZeeboNormToByte(nx);
				nDst[1] = ZeeboNormToByte(ny);
				nDst[2] = ZeeboNormToByte(nz);
			}
		}

		GLfixed *pDst = (GLfixed*)dst;
		pDst[0] = ZeeboToFixed(px);
		pDst[1] = ZeeboToFixed(py);
		pDst[2] = ZeeboToFixed(pz);
	}

	RenderMeshesWithVerts(gl, atomic, header, s_skinScratch);
}

void *destroyGles1NativeData(void *object, int32 offset, int32 size)
{
	(void)offset; (void)size;
	Geometry *geo = (Geometry*)object;
	if(geo->instData && geo->instData->platform == PLATFORM_GL3){
		InstanceDataHeader *header = (InstanceDataHeader*)geo->instData;
		defaultUninstanceCB(geo, header);
		rwFree(header);
		geo->instData = nil;
	}
	return object;
}

void ObjPipeline::init(void)
{
	this->rw::ObjPipeline::init(PLATFORM_GL3);
	this->impl.instance   = (void (*)(ObjPipeline*, Atomic*))nil;
	this->impl.uninstance = (void (*)(ObjPipeline*, Atomic*))nil;
	this->impl.render     = [](rw::ObjPipeline *pipe, Atomic *atomic) {
		ObjPipeline *p = (ObjPipeline*)pipe;
		Geometry *geo = atomic->geometry;
		if(!geo || !geo->meshHeader) return;

		if(geo->instData && geo->lockedSinceInst != 0){
			destroyGles1NativeData(geo, 0, 0);
			geo->lockedSinceInst = 0;
		}
		if(!geo->instData){
			InstanceDataHeader *hdr = (InstanceDataHeader*)rwMalloc(sizeof(InstanceDataHeader), MEMDUR_EVENT | ID_GEOMETRY);
			memset(hdr, 0, sizeof(*hdr));
			hdr->platform = PLATFORM_GL3;
			geo->instData = hdr;
			if(p->instanceCB) p->instanceCB(geo, hdr, 0);
			geo->lockedSinceInst = 0;
		}
		if(p->renderCB && geo->instData){
			p->renderCB(atomic, (InstanceDataHeader*)geo->instData);
		}
	};
	this->instanceCB   = defaultInstanceCB;
	this->uninstanceCB = defaultUninstanceCB;
	this->renderCB     = defaultRenderCB;
	(void)instanceNothing;
}

ObjPipeline *ObjPipeline::create(void)
{
	ObjPipeline *pipe = (ObjPipeline*)rwMalloc(sizeof(ObjPipeline), MEMDUR_GLOBAL);
	pipe->init();
	return pipe;
}

ObjPipeline *makeDefaultPipeline(void)
{
	return ObjPipeline::create();
}

ObjPipeline *makeMatFXPipeline(void)
{
	return ObjPipeline::create();
}

ObjPipeline *makeSkinPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->renderCB = skinRenderCB;
	return pipe;
}

} // namespace gles1
} // namespace rw

#endif // RW_GLES1
