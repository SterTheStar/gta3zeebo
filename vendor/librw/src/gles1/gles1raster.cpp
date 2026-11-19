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
#define PLUGIN_ID ID_DRIVER

namespace rw {
namespace gles1 {

int32 nativeRasterOffset = 0;

static void ConvertToRGBA(Raster *raster, const uint8 *src, uint8 *dst, int32 w, int32 h)
{
	uint32 fmt = raster->format & 0x0F00;
	int32 count = w * h;
	if(fmt == Raster::C8888){
		// BGRA -> RGBA or RGBA -> RGBA depending on platform; RW PC is BGRA
		for(int32 i = 0; i < count; i++){
			dst[i*4+0] = src[i*4+2];
			dst[i*4+1] = src[i*4+1];
			dst[i*4+2] = src[i*4+0];
			dst[i*4+3] = src[i*4+3];
		}
	}else if(fmt == Raster::C888){
		for(int32 i = 0; i < count; i++){
			dst[i*4+0] = src[i*4+2];
			dst[i*4+1] = src[i*4+1];
			dst[i*4+2] = src[i*4+0];
			dst[i*4+3] = 255;
		}
	}else if(fmt == Raster::C1555){
		const uint16 *s16 = (const uint16*)src;
		for(int32 i = 0; i < count; i++){
			uint16 p = s16[i];
			dst[i*4+0] = ((p >> 10) & 0x1F) << 3;
			dst[i*4+1] = ((p >>  5) & 0x1F) << 3;
			dst[i*4+2] = ((p >>  0) & 0x1F) << 3;
			dst[i*4+3] = (p & 0x8000) ? 255 : 0;
		}
	}else if(fmt == Raster::C565){
		const uint16 *s16 = (const uint16*)src;
		for(int32 i = 0; i < count; i++){
			uint16 p = s16[i];
			dst[i*4+0] = ((p >> 11) & 0x1F) << 3;
			dst[i*4+1] = ((p >>  5) & 0x3F) << 2;
			dst[i*4+2] = ((p >>  0) & 0x1F) << 3;
			dst[i*4+3] = 255;
		}
	}else if(fmt == Raster::C4444){
		const uint16 *s16 = (const uint16*)src;
		for(int32 i = 0; i < count; i++){
			uint16 p = s16[i];
			dst[i*4+0] = ((p >>  8) & 0x0F) << 4;
			dst[i*4+1] = ((p >>  4) & 0x0F) << 4;
			dst[i*4+2] = ((p >>  0) & 0x0F) << 4;
			dst[i*4+3] = ((p >> 12) & 0x0F) << 4;
		}
	}else{
		memset(dst, 255, (size_t)count * 4);
	}
}

Raster *rasterCreate(Raster *raster)
{
	Gles1Raster *nat = GETGLES1RASTEREXT(raster);
	memset(nat, 0, sizeof(*nat));

	if(raster->width <= 0 || raster->height <= 0){
		raster->flags |= Raster::DONTALLOCATE;
		return raster;
	}

	int32 type = raster->type;
	if(type == Raster::NORMAL || type == Raster::TEXTURE){
		nat->internalFormat = GL_RGBA;
		nat->format         = GL_RGBA;
		nat->type           = GL_UNSIGNED_BYTE;
		nat->hasAlpha       = 1;
		nat->numLevels      = 1;

		if(raster->depth == 0) raster->depth = 32;
		raster->stride = raster->width * (raster->depth / 8);

		if(const IGLVtbl *gl = ZeeboGL()){
			gl->glGenTextures(1, &nat->texid);
			gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		}
	}
	return raster;
}

uint8 *rasterLock(Raster *raster, int32 level, int32 lockMode)
{
	(void)level;
	(void)lockMode;
	Gles1Raster *nat = GETGLES1RASTEREXT(raster);
	if(!nat->pixels){
		size_t sz = (size_t)raster->stride * (size_t)raster->height;
		if(sz == 0) sz = (size_t)raster->width * (size_t)raster->height * 4;
		nat->pixels = (uint8*)rwMalloc(sz, MEMDUR_EVENT | ID_RASTERGL3);
		if(nat->pixels) memset(nat->pixels, 0, sz);
	}
	raster->pixels = nat->pixels;
	return raster->pixels;
}

void rasterUnlock(Raster *raster, int32 level)
{
	Gles1Raster *nat = GETGLES1RASTEREXT(raster);
	if(level == 0 && nat->pixels && nat->texid){
		const IGLVtbl *gl = ZeeboGL();
		if(gl){
			static uint8 *s_rgbaScratch = nil;
			static size_t s_rgbaScratchCap = 0;
			size_t rgbaSz = (size_t)raster->width * (size_t)raster->height * 4;
			uint8 *rgba = nil;
			bool tempAlloc = false;
			if(rgbaSz <= 256u * 1024u){
				if(s_rgbaScratchCap < rgbaSz){
					if(s_rgbaScratch) rwFree(s_rgbaScratch);
					size_t allocSz = rgbaSz < (64u * 1024u) ? (64u * 1024u) : rgbaSz;
					s_rgbaScratch = (uint8*)rwMalloc(allocSz, MEMDUR_EVENT | ID_RASTERGL3);
					s_rgbaScratchCap = s_rgbaScratch ? allocSz : 0;
				}
				rgba = s_rgbaScratch;
			}else{
				rgba = (uint8*)rwMalloc(rgbaSz, MEMDUR_FUNCTION | ID_RASTERGL3);
				tempAlloc = true;
			}
			if(rgba){
				ConvertToRGBA(raster, nat->pixels, rgba, raster->width, raster->height);
				gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
				gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, raster->width, raster->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
				if(tempAlloc) rwFree(rgba);
				// Texture is now resident in GL; release CPU pixel copy to save Zeebo's 64MB heap
				rwFree(nat->pixels);
				nat->pixels = nil;
			}
		}
	}
	raster->pixels = nil;
}

int32 rasterNumLevels(Raster *raster)
{
	Gles1Raster *nat = GETGLES1RASTEREXT(raster);
	return nat->numLevels > 0 ? nat->numLevels : 1;
}

bool32 imageFindRasterFormat(Image *img, int32 type, int32 *width, int32 *height, int32 *depth, int32 *format)
{
	*width  = img->width;
	*height = img->height;
	*depth  = 32;
	*format = Raster::C8888 | (type & 0x7) | (type & Raster::MIPMAP);
	return 1;
}

bool32 rasterFromImage(Raster *raster, Image *image)
{
	if(!image || !raster) return 0;

	uint8 *pixels = rasterLock(raster, 0, Raster::LOCKWRITE);
	if(!pixels) return 0;

	int32 w = image->width;
	int32 h = image->height;
	for(int32 y = 0; y < h; y++){
		const uint8 *src = image->pixels + y * image->stride;
		uint8 *dst = pixels + y * raster->stride;
		if(image->depth == 32){
			memcpy(dst, src, (size_t)w * 4);
		}else if(image->depth == 24){
			for(int32 x = 0; x < w; x++){
				dst[x*4+0] = src[x*3+0];
				dst[x*4+1] = src[x*3+1];
				dst[x*4+2] = src[x*3+2];
				dst[x*4+3] = 255;
			}
		}else if(image->depth == 8 && image->palette){
			for(int32 x = 0; x < w; x++){
				uint8 idx = src[x];
				dst[x*4+0] = image->palette[idx*4+2];
				dst[x*4+1] = image->palette[idx*4+1];
				dst[x*4+2] = image->palette[idx*4+0];
				dst[x*4+3] = image->palette[idx*4+3];
			}
		}else if(image->depth == 4 && image->palette){
			for(int32 x = 0; x < w; x++){
				uint8 b = src[x >> 1];
				uint8 idx = (x & 1) ? (b >> 4) : (b & 0xF);
				dst[x*4+0] = image->palette[idx*4+2];
				dst[x*4+1] = image->palette[idx*4+1];
				dst[x*4+2] = image->palette[idx*4+0];
				dst[x*4+3] = image->palette[idx*4+3];
			}
		}
	}

	rasterUnlock(raster, 0);
	return 1;
}

Image *rasterToImage(Raster *raster)
{
	if(!raster) return nil;
	Image *img = Image::create(raster->width, raster->height, 32);
	if(!img) return nil;
	img->allocate();
	Gles1Raster *nat = GETGLES1RASTEREXT(raster);
	if(nat->pixels){
		ConvertToRGBA(raster, nat->pixels, img->pixels, raster->width, raster->height);
	}
	return img;
}

static Raster *readD3DRasterLevels(Stream *stream, uint32 format, int32 width, int32 height, int32 depth, int32 numLevels, int32 type, int32 compression)
{
	int32 pallength = 0;
	if(format & Raster::PAL4) pallength = 32;
	else if(format & Raster::PAL8) pallength = 256;

	uint8 *pal = nil;
	if(pallength != 0){
		pal = rwNewT(uint8, 4 * pallength, MEMDUR_FUNCTION | ID_IMAGE);
		stream->read8(pal, 4 * pallength);
	}

	Raster *raster = nil;
	for(int32 i = 0; i < numLevels; i++){
		uint32 size = stream->readU32();
		if(i > 0 || width <= 0 || height <= 0){
			stream->seek(size);
			continue;
		}
		uint8 *buf = rwNewT(uint8, size, MEMDUR_FUNCTION | ID_IMAGE);
		stream->read8(buf, size);

		if(pallength != 0){
			int32 imgDepth = (format & Raster::PAL4) ? 4 : 8;
			Image *img = Image::create(width, height, imgDepth);
			img->allocate();
			uint32 copySz = size < (uint32)(img->stride * height) ? size : (uint32)(img->stride * height);
			memcpy(img->pixels, buf, copySz);
			int32 maxPal = (1 << imgDepth) * 4;
			int32 srcPal = 4 * pallength;
			memcpy(img->palette, pal, srcPal < maxPal ? srcPal : maxPal);
			rwFree(buf);
			raster = Raster::createFromImage(img);
			img->destroy();
		}else if(compression){
			Image *img = Image::create(width, height, 32);
			img->allocate();
			img->setPixelsDXT(compression, buf);
			rwFree(buf);
			raster = Raster::createFromImage(img);
			img->destroy();
		}else{
			int32 rfmt = (format & 0x0F00) | (type & 0x07);
			if((rfmt & 0x0F00) == 0) rfmt |= (depth == 16) ? Raster::C565 : Raster::C8888;
			raster = Raster::create(width, height, depth, rfmt);
			if(raster){
				uint8 *dst = raster->lock(0, Raster::LOCKWRITE | Raster::LOCKNOFETCH);
				if(dst){
					uint32 copySz = size < (uint32)(raster->stride * height) ? size : (uint32)(raster->stride * height);
					memcpy(dst, buf, copySz);
					raster->unlock(0);
				}
			}
			rwFree(buf);
		}
	}

	if(pal) rwFree(pal);
	return raster;
}

Texture *readNativeTextureD3D8(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_D3D8){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	uint32 format      = stream->readU32();
	bool32 hasAlpha    = stream->readI32();
	(void)hasAlpha;
	int32  width       = stream->readU16();
	int32  height      = stream->readU16();
	int32  depth       = stream->readU8();
	int32  numLevels   = stream->readU8();
	int32  type        = stream->readU8();
	int32  compression = stream->readU8();

	tex->raster = readD3DRasterLevels(stream, format, width, height, depth, numLevels, type, compression);
	return tex;
}

Texture *readNativeTextureD3D9(Stream *stream)
{
	uint32 platform;
	if(!findChunk(stream, ID_STRUCT, nil, nil)){
		RWERROR((ERR_CHUNK, "STRUCT"));
		return nil;
	}
	platform = stream->readU32();
	if(platform != PLATFORM_D3D9){
		RWERROR((ERR_PLATFORM, platform));
		return nil;
	}
	Texture *tex = Texture::create(nil);
	if(tex == nil)
		return nil;

	tex->filterAddressing = stream->readU32();
	stream->read8(tex->name, 32);
	stream->read8(tex->mask, 32);

	uint32 format    = (uint32)stream->readI32();
	int32  d3dformat = stream->readI32();
	int32  width     = stream->readU16();
	int32  height    = stream->readU16();
	int32  depth     = stream->readU8();
	int32  numLevels = stream->readU8();
	int32  type      = stream->readU8();
	int32  flags     = stream->readU8();

	int32 compression = 0;
	if(flags & 8){
		if(d3dformat == 0x31545844) compression = 1;
		else if(d3dformat == 0x33545844) compression = 3;
		else if(d3dformat == 0x35545844) compression = 5;
	}

	tex->raster = readD3DRasterLevels(stream, format, width, height, depth, numLevels, type, compression);
	return tex;
}

} // namespace gles1
} // namespace rw

#endif // RW_GLES1
