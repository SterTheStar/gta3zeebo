#pragma once

namespace rw {

#ifdef RW_GLES1
struct EngineOpenParams
{
	int width, height;
	const char *windowtitle;
};
#endif

namespace gles1 {

void registerPlatformPlugins(void);
void *destroyGles1NativeData(void *object, int32 offset, int32 size);

extern Device renderdevice;

struct Gles1Raster
{
	uint32 texid;
	int32  internalFormat;
	uint32 format;
	uint32 type;
	bool32 hasAlpha;
	int32  numLevels;
	uint8 *pixels;
	uint32 minFilter;
	uint32 magFilter;
	uint32 addressU;
	uint32 addressV;
};

extern int32 nativeRasterOffset;
#define GETGLES1RASTEREXT(raster) PLUGINOFFSET(rw::gles1::Gles1Raster, raster, rw::gles1::nativeRasterOffset)

struct InstanceData
{
	uint32    numIndex;
	uint32    minVert;
	int32     numVertices;
	Material *material;
	bool32    vertexAlpha;
	uint32    offset;
};

struct InstanceDataHeader : rw::InstanceDataHeader
{
	uint32        serialNumber;
	uint32        numMeshes;
	uint16       *indexBuffer;
	uint32        primType;
	uint8        *vertexBuffer;
	uint32        totalNumIndex;
	uint32        totalNumVertex;
	uint32        stride;
	int32         posOffset;
	int32         normalOffset;
	int32         colorOffset;
	int32         texCoordOffset;
	InstanceData *inst;
};

struct Im3DVertex
{
	V3d     position;
	uint8   r, g, b, a;
	float32 u, v;

	void setX(float32 x) { this->position.x = x; }
	void setY(float32 y) { this->position.y = y; }
	void setZ(float32 z) { this->position.z = z; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u) { this->u = u; }
	void setV(float32 v) { this->v = v; }

	float getX(void) { return this->position.x; }
	float getY(void) { return this->position.y; }
	float getZ(void) { return this->position.z; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

struct Im2DVertex
{
	float32 x, y, z, w;
	uint8   r, g, b, a;
	float32 u, v;

	void setScreenX(float32 x) { this->x = x; }
	void setScreenY(float32 y) { this->y = y; }
	void setScreenZ(float32 z) { this->z = z; }
	void setCameraZ(float32 z) { this->w = z; }
	void setRecipCameraZ(float32 recipz) { this->w = 1.0f/recipz; }
	void setColor(uint8 r, uint8 g, uint8 b, uint8 a) {
		this->r = r; this->g = g; this->b = b; this->a = a; }
	void setU(float32 u, float recipz) { (void)recipz; this->u = u; }
	void setV(float32 v, float recipz) { (void)recipz; this->v = v; }

	float getScreenX(void) { return this->x; }
	float getScreenY(void) { return this->y; }
	float getScreenZ(void) { return this->z; }
	float getCameraZ(void) { return this->w; }
	float getRecipCameraZ(void) { return 1.0f/this->w; }
	RGBA getColor(void) { return makeRGBA(this->r, this->g, this->b, this->a); }
	float getU(void) { return this->u; }
	float getV(void) { return this->v; }
};

class ObjPipeline : public rw::ObjPipeline
{
public:
	void init(void);
	static ObjPipeline *create(void);

	void (*instanceCB)(Geometry *geo, InstanceDataHeader *header, bool32 reinstance);
	void (*uninstanceCB)(Geometry *geo, InstanceDataHeader *header);
	void (*renderCB)(Atomic *atomic, InstanceDataHeader *header);
};

ObjPipeline *makeDefaultPipeline(void);
ObjPipeline *makeMatFXPipeline(void);
ObjPipeline *makeSkinPipeline(void);

Raster *rasterCreate(Raster *raster);
uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode);
void rasterUnlock(Raster *raster, int32 level);
int32 rasterNumLevels(Raster *raster);
bool32 imageFindRasterFormat(Image *img, int32 type, int32 *width, int32 *height, int32 *depth, int32 *format);
bool32 rasterFromImage(Raster *raster, Image *image);
Image *rasterToImage(Raster *raster);
Texture *readNativeTextureD3D8(Stream *stream);
Texture *readNativeTextureD3D9(Stream *stream);

void im2DRenderLine(void *vertices, int32 numVertices, int32 vert1, int32 vert2);
void im2DRenderTriangle(void *vertices, int32 numVertices, int32 vert1, int32 vert2, int32 vert3);
void im2DRenderPrimitive(PrimitiveType primType, void *vertices, int32 numVertices);
void im2DRenderIndexedPrimitive(PrimitiveType primType, void *vertices, int32 numVertices, void *indices, int32 numIndices);

void im3DTransform(void *vertices, int32 numVertices, Matrix *world, uint32 flags);
void im3DRenderPrimitive(PrimitiveType primType);
void im3DRenderIndexedPrimitive(PrimitiveType primType, void *indices, int32 numIndices);
void im3DEnd(void);

void setMaterial(Material *mat, bool32 hasColor, bool32 hasTex);
void setLights(World *world, Matrix *worldMat);
void bindTexture(Texture *tex);
void flushCache(void);

// Internal GLES1 state cache helpers (shared across gles1.cpp / pipe / immed / raster)
void notifyTextureBound(uint32 texid);
void setClientStates(bool vert, bool norm, bool color, bool tex);
void setLightingState(bool lighting, bool rescaleNormal, bool colorMaterial);
void setConstantColor(uint8 r, uint8 g, uint8 b, uint8 a);
void invalidateLightCache(void);
bool isTextureEnabled(void);

} // namespace gles1
} // namespace rw
