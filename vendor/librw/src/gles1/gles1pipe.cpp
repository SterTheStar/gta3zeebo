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

namespace rw {
namespace gles1 {

struct Gles1Vertex {
	float32 x, y, z;
	float32 nx, ny, nz;
	float32 r, g, b, a;
	float32 u, v;
};

static void defaultUninstanceCB(Geometry *geo, InstanceDataHeader *header);

static void instanceNothing(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	(void)geo; (void)header; (void)reinstance;
}

static void defaultInstanceCB(Geometry *geo, InstanceDataHeader *header, bool32 reinstance)
{
	if(reinstance) return;

	MeshHeader *meshh = geo->meshHeader;
	header->serialNumber = meshh->serialNum;
	header->numMeshes    = meshh->numMeshes;
	header->primType     = (meshh->flags & MeshHeader::TRISTRIP) ? GL_TRIANGLE_STRIP : GL_TRIANGLES;

	uint32 totalIdx = 0;
	Mesh *meshes = meshh->getMeshes();
	for(uint32 i = 0; i < header->numMeshes; i++)
		totalIdx += meshes[i].numIndices;

	header->totalNumIndex  = totalIdx;
	header->totalNumVertex = geo->numVertices;
	header->stride         = sizeof(Gles1Vertex);
	header->posOffset      = 0;
	header->normalOffset   = 12;
	header->colorOffset    = 24;
	header->texCoordOffset = 28;

	header->indexBuffer  = (uint16*)rwMalloc(totalIdx * sizeof(uint16), MEMDUR_EVENT | ID_GEOMETRY);
	header->vertexBuffer = (uint8*)rwMalloc(geo->numVertices * sizeof(Gles1Vertex), MEMDUR_EVENT | ID_GEOMETRY);
	header->inst         = (InstanceData*)rwMalloc(header->numMeshes * sizeof(InstanceData), MEMDUR_EVENT | ID_GEOMETRY);
	if(!header->indexBuffer || !header->vertexBuffer || !header->inst){
		defaultUninstanceCB(geo, header);
		return;
	}

	// Pack vertices
	Gles1Vertex *verts = (Gles1Vertex*)header->vertexBuffer;
	V3d *pos  = geo->morphTargets[0].vertices;
	V3d *norm = geo->morphTargets[0].normals;
	RGBA *col = geo->colors;
	TexCoords *uv = geo->texCoords[0];

	for(int32 i = 0; i < geo->numVertices; i++){
		verts[i].x  = pos ? pos[i].x : 0.0f;
		verts[i].y  = pos ? pos[i].y : 0.0f;
		verts[i].z  = pos ? pos[i].z : 0.0f;
		verts[i].nx = norm ? norm[i].x : 0.0f;
		verts[i].ny = norm ? norm[i].y : 0.0f;
		verts[i].nz = norm ? norm[i].z : 1.0f;
		if(col){
			verts[i].r = col[i].red   / 255.0f;
			verts[i].g = col[i].green / 255.0f;
			verts[i].b = col[i].blue  / 255.0f;
			verts[i].a = col[i].alpha / 255.0f;
		}else{
			verts[i].r = verts[i].g = verts[i].b = verts[i].a = 1.0f;
		}
		verts[i].u = uv ? uv[i].u : 0.0f;
		verts[i].v = uv ? uv[i].v : 0.0f;
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
		inst->offset      = idxOff * sizeof(uint16);

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
	gl->glLightModelxv(0x0B53 /* GL_LIGHT_MODEL_AMBIENT */, amb);

	int lightIdx = 0;
	static const GLfixed kZero[4] = { 0, 0, 0, ZeeboToFixed(1.0f) };
	for(int i = 0; i < lightData.numDirectionals && lightIdx < 8; i++){
		Light *l = lightData.directionals[i];
		if(!l || !l->getFrame()) continue;
		uint32 lid = 0x4000u + (uint32)lightIdx; // GL_LIGHT0 + i
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
			ZeeboToFixed(1.0f)
		};
		gl->glEnable(lid);
		gl->glLightxv(lid, 0x1203 /* GL_POSITION */, pos);
		gl->glLightxv(lid, 0x1201 /* GL_DIFFUSE */,  diff);
		gl->glLightxv(lid, 0x1200 /* GL_AMBIENT */,  kZero);
		gl->glLightxv(lid, 0x1202 /* GL_SPECULAR */, kZero);
		lightIdx++;
	}
	for(int i = 0; i < lightData.numLocals && lightIdx < 8; i++){
		Light *l = lightData.locals[i];
		if(!l || !l->getFrame()) continue;
		uint32 lid = 0x4000u + (uint32)lightIdx;
		V3d p = l->getFrame()->getLTM()->pos;
		GLfixed pos[4] = {
			ZeeboToFixed(p.x),
			ZeeboToFixed(p.y),
			ZeeboToFixed(p.z),
			ZeeboToFixed(1.0f)
		};
		GLfixed diff[4] = {
			ZeeboToFixed(l->color.red),
			ZeeboToFixed(l->color.green),
			ZeeboToFixed(l->color.blue),
			ZeeboToFixed(1.0f)
		};
		gl->glEnable(lid);
		gl->glLightxv(lid, 0x1203 /* GL_POSITION */, pos);
		gl->glLightxv(lid, 0x1201 /* GL_DIFFUSE */,  diff);
		gl->glLightxv(lid, 0x1200 /* GL_AMBIENT */,  kZero);
		gl->glLightxv(lid, 0x1202 /* GL_SPECULAR */, kZero);
		lightIdx++;
	}
	for(; lightIdx < 8; lightIdx++){
		gl->glDisable(0x4000u + (uint32)lightIdx);
	}
}

static void RenderMeshesWithVerts(const IGLVtbl *gl, Atomic *atomic, InstanceDataHeader *header, Gles1Vertex *v)
{
	Geometry *geo = atomic->geometry;
	Frame *frame = atomic->getFrame();
	Matrix *ltm = frame ? frame->getLTM() : nil;

	bool hasNorm  = (geo->flags & Geometry::NORMALS) != 0;
	bool hasColor = (geo->flags & Geometry::PRELIT) != 0;
	bool hasTex   = (geo->numTexCoordSets > 0);
	bool useLight = (geo->flags & Geometry::LIGHT) != 0 && hasNorm;

	gl->glMatrixMode(GL_MODELVIEW);
	if(useLight){
		// Set world-space lights while GL_MODELVIEW still holds only the view matrix
		ApplyGles1Lights(gl, atomic);
		gl->glEnable(GL_LIGHTING);
		gl->glEnable(GL_COLOR_MATERIAL);
		gl->glEnable(GL_NORMALIZE);
	}else{
		gl->glDisable(GL_LIGHTING);
		gl->glDisable(GL_NORMALIZE);
	}

	if(ltm){
		gl->glPushMatrix();
		RawMatrix rm;
		convMatrix(&rm, ltm);
		GLfixed fx[16];
		const float32 *m = (const float32*)&rm;
		for(int i = 0; i < 16; i++) fx[i] = ZeeboToFixed(m[i]);
		gl->glMultMatrixx(fx);
	}

	gl->glEnableClientState(GL_VERTEX_ARRAY);
	gl->glVertexPointer(3, GL_FLOAT, sizeof(Gles1Vertex), &v[0].x);

	if(hasNorm){
		gl->glEnableClientState(GL_NORMAL_ARRAY);
		gl->glNormalPointer(GL_FLOAT, sizeof(Gles1Vertex), &v[0].nx);
	}else{
		gl->glDisableClientState(GL_NORMAL_ARRAY);
	}

	if(hasColor){
		gl->glEnableClientState(GL_COLOR_ARRAY);
		gl->glColorPointer(4, GL_FLOAT, sizeof(Gles1Vertex), &v[0].r);
	}else{
		gl->glDisableClientState(GL_COLOR_ARRAY);
	}

	if(hasTex){
		gl->glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		gl->glTexCoordPointer(2, GL_FLOAT, sizeof(Gles1Vertex), &v[0].u);
	}else{
		gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	for(uint32 i = 0; i < header->numMeshes; i++){
		InstanceData *inst = &header->inst[i];
		Material *mat = inst->material;
		if(mat){
			SetRenderState(VERTEXALPHA, (inst->vertexAlpha || mat->color.alpha != 0xFF) ? 1 : 0);
			if(!hasColor){
				gl->glColor4x(
					ZeeboToFixed(mat->color.red   / 255.0f),
					ZeeboToFixed(mat->color.green / 255.0f),
					ZeeboToFixed(mat->color.blue  / 255.0f),
					ZeeboToFixed(mat->color.alpha / 255.0f)
				);
			}
			if(mat->texture && mat->texture->raster && hasTex){
				SetRenderState(TEXTURERASTER, mat->texture->raster);
				SetRenderState(TEXTUREADDRESSU, mat->texture->getAddressU());
				SetRenderState(TEXTUREADDRESSV, mat->texture->getAddressV());
				SetRenderState(TEXTUREFILTER,   mat->texture->getFilter());
			}else{
				SetRenderState(TEXTURERASTER, nil);
			}
		}

		uint16 *idx = (uint16*)((uint8*)header->indexBuffer + inst->offset);
		gl->glDrawElements(header->primType, inst->numIndex, GL_UNSIGNED_SHORT, idx);
	}

	gl->glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	gl->glDisableClientState(GL_COLOR_ARRAY);
	gl->glDisableClientState(GL_NORMAL_ARRAY);
	gl->glDisableClientState(GL_VERTEX_ARRAY);
	gl->glDisable(GL_LIGHTING);
	gl->glDisable(GL_NORMALIZE);

	if(ltm){
		gl->glPopMatrix();
	}
}

static void defaultRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !header || !header->vertexBuffer || !header->indexBuffer) return;
	RenderMeshesWithVerts(gl, atomic, header, (Gles1Vertex*)header->vertexBuffer);
}

static Matrix s_skinMatrices[64];
static Gles1Vertex *s_skinScratch = nil;
static int32 s_skinScratchCap = 0;

static void skinRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	const IGLVtbl *gl = ZeeboGL();
	if(!gl || !header || !header->vertexBuffer || !header->indexBuffer) return;

	Geometry *geo = atomic->geometry;
	Skin *skin = Skin::get(geo);
	if(!skin || !skin->indices || !skin->weights || !skin->inverseMatrices){
		RenderMeshesWithVerts(gl, atomic, header, (Gles1Vertex*)header->vertexBuffer);
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

	int32 nVerts = (int32)header->totalNumVertex;
	if(nVerts > s_skinScratchCap){
		if(s_skinScratch) rwFree(s_skinScratch);
		s_skinScratch = (Gles1Vertex*)rwMalloc((size_t)nVerts * sizeof(Gles1Vertex), MEMDUR_EVENT | ID_SKIN);
		s_skinScratchCap = s_skinScratch ? nVerts : 0;
	}
	if(!s_skinScratch){
		RenderMeshesWithVerts(gl, atomic, header, (Gles1Vertex*)header->vertexBuffer);
		return;
	}

	const Gles1Vertex *src = (const Gles1Vertex*)header->vertexBuffer;
	for(int32 i = 0; i < nVerts; i++){
		s_skinScratch[i] = src[i];
		float32 px = 0.0f, py = 0.0f, pz = 0.0f;
		float32 nx = 0.0f, ny = 0.0f, nz = 0.0f;
		const uint8   *idx = &skin->indices[i * 4];
		const float32 *wt  = &skin->weights[i * 4];
		for(int b = 0; b < 4; b++){
			float32 w = wt[b];
			if(w <= 0.0f) continue;
			uint8 bi = idx[b];
			if(bi >= numBones) continue;
			const Matrix &m = s_skinMatrices[bi];
			px += w * (src[i].x * m.right.x + src[i].y * m.up.x + src[i].z * m.at.x + m.pos.x);
			py += w * (src[i].x * m.right.y + src[i].y * m.up.y + src[i].z * m.at.y + m.pos.y);
			pz += w * (src[i].x * m.right.z + src[i].y * m.up.z + src[i].z * m.at.z + m.pos.z);
			nx += w * (src[i].nx * m.right.x + src[i].ny * m.up.x + src[i].nz * m.at.x);
			ny += w * (src[i].nx * m.right.y + src[i].ny * m.up.y + src[i].nz * m.at.y);
			nz += w * (src[i].nx * m.right.z + src[i].ny * m.up.z + src[i].nz * m.at.z);
		}
		s_skinScratch[i].x  = px; s_skinScratch[i].y  = py; s_skinScratch[i].z  = pz;
		s_skinScratch[i].nx = nx; s_skinScratch[i].ny = ny; s_skinScratch[i].nz = nz;
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
