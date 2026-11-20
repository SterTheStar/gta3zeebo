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

static inline bool IsPowerOfTwo(int32 v)
{
	return v > 0 && (v & (v - 1)) == 0;
}

static void ConvertToRGBA(uint32 rwFmt, const uint8 *src, uint8 *dst, int32 count)
{
	uint32 fmt = rwFmt & 0x0F00;
	if(fmt == Raster::C8888){
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

static void Choose16BitFormat(uint32 rwFmt, const uint8 *rgba, int32 count, uint32 *outFmt, uint32 *outType, bool32 *outHasAlpha)
{
	(void)rwFmt;
	// Always inspect decoded RGBA alpha channel (TXD headers for PAL8/DXT often report C565/C888 even with alpha)
	bool hasZero = false;
	bool hasPartial = false;
	for(int32 i = 0; i < count; i++){
		uint8 a = rgba[i*4 + 3];
		if(a == 0){
			hasZero = true;
		}else if(a < 255){
			hasPartial = true;
			break;
		}
	}
	if(!hasZero && !hasPartial){
		*outFmt = GL_RGB;
		*outType = GL_UNSIGNED_SHORT_5_6_5;
		*outHasAlpha = 0;
	}else if(!hasPartial){
		*outFmt = GL_RGBA;
		*outType = GL_UNSIGNED_SHORT_5_5_5_1;
		*outHasAlpha = 1;
	}else{
		*outFmt = GL_RGBA;
		*outType = GL_UNSIGNED_SHORT_4_4_4_4;
		*outHasAlpha = 1;
	}
}

static void PackRGBAto16(const uint8 *rgba, uint16 *dst16, int32 count, uint32 glType)
{
	if(glType == GL_UNSIGNED_SHORT_5_6_5){
		for(int32 i = 0; i < count; i++){
			uint32 r = rgba[i*4+0] >> 3;
			uint32 g = rgba[i*4+1] >> 2;
			uint32 b = rgba[i*4+2] >> 3;
			dst16[i] = (uint16)((r << 11) | (g << 5) | b);
		}
	}else if(glType == GL_UNSIGNED_SHORT_5_5_5_1){
		for(int32 i = 0; i < count; i++){
			uint32 r = rgba[i*4+0] >> 3;
			uint32 g = rgba[i*4+1] >> 3;
			uint32 b = rgba[i*4+2] >> 3;
			uint32 a = (rgba[i*4+3] >= 128) ? 1u : 0u;
			dst16[i] = (uint16)((r << 11) | (g << 6) | (b << 1) | a);
		}
	}else{
		for(int32 i = 0; i < count; i++){
			uint32 r = rgba[i*4+0] >> 4;
			uint32 g = rgba[i*4+1] >> 4;
			uint32 b = rgba[i*4+2] >> 4;
			uint32 a = rgba[i*4+3] >> 4;
			dst16[i] = (uint16)((r << 12) | (g << 8) | (b << 4) | a);
		}
	}
}

static void DownsampleRGBA2x2(const uint8 *src, int32 sw, int32 sh, uint8 *dst, int32 dw, int32 dh)
{
	for(int32 y = 0; y < dh; y++){
		int32 sy0 = (y * 2 < sh) ? (y * 2) : (sh - 1);
		int32 sy1 = (sy0 + 1 < sh) ? (sy0 + 1) : sy0;
		for(int32 x = 0; x < dw; x++){
			int32 sx0 = (x * 2 < sw) ? (x * 2) : (sw - 1);
			int32 sx1 = (sx0 + 1 < sw) ? (sx0 + 1) : sx0;
			const uint8 *p00 = src + (sy0 * sw + sx0) * 4;
			const uint8 *p10 = src + (sy0 * sw + sx1) * 4;
			const uint8 *p01 = src + (sy1 * sw + sx0) * 4;
			const uint8 *p11 = src + (sy1 * sw + sx1) * 4;
			uint8 *d = dst + (y * dw + x) * 4;
			d[0] = (uint8)((p00[0] + p10[0] + p01[0] + p11[0] + 2) >> 2);
			d[1] = (uint8)((p00[1] + p10[1] + p01[1] + p11[1] + 2) >> 2);
			d[2] = (uint8)((p00[2] + p10[2] + p01[2] + p11[2] + 2) >> 2);
			d[3] = (uint8)((p00[3] + p10[3] + p01[3] + p11[3] + 2) >> 2);
		}
	}
}

static uint8 *AllocScratch(uint8 **buf, size_t *cap, size_t needed, size_t maxKeep, bool *tempAlloc)
{
	if(needed <= maxKeep){
		if(*cap < needed){
			if(*buf) rwFree(*buf);
			size_t allocSz = needed < (64u * 1024u) ? (64u * 1024u) : needed;
			*buf = (uint8*)rwMalloc(allocSz, MEMDUR_EVENT | ID_RASTERGL3);
			*cap = *buf ? allocSz : 0;
		}
		*tempAlloc = false;
		return *buf;
	}
	*tempAlloc = true;
	return (uint8*)rwMalloc(needed, MEMDUR_FUNCTION | ID_RASTERGL3);
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
		nat->internalFormat = GL_RGB;
		nat->format         = GL_RGB;
		nat->type           = GL_UNSIGNED_SHORT_5_6_5;
		nat->hasAlpha       = 0;
		nat->numLevels      = 1;
		nat->minFilter      = GL_NEAREST;
		nat->magFilter      = GL_NEAREST;
		nat->addressU       = GL_REPEAT;
		nat->addressV       = GL_REPEAT;

		if(raster->depth == 0) raster->depth = 32;
		raster->stride = raster->width * (raster->depth / 8);

		if(const IGLVtbl *gl = ZeeboGL()){
			gl->glGenTextures(1, &nat->texid);
			gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
			notifyTextureBound(nat->texid);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
			static uint8 *s_tex16Scratch = nil;
			static size_t s_tex16ScratchCap = 0;

			int32 count = raster->width * raster->height;
			size_t rgbaSz = (size_t)count * 4u;
			size_t tex16Sz = (size_t)count * 2u;
			bool tempRgba = false, temp16 = false;
			uint8 *rgba = AllocScratch(&s_rgbaScratch, &s_rgbaScratchCap, rgbaSz, 256u * 1024u, &tempRgba);
			uint16 *tex16 = (uint16*)AllocScratch(&s_tex16Scratch, &s_tex16ScratchCap, tex16Sz, 128u * 1024u, &temp16);

			if(rgba && tex16){
				ConvertToRGBA(raster->format, nat->pixels, rgba, count);
				uint32 glFmt = GL_RGB, glType = GL_UNSIGNED_SHORT_5_6_5;
				bool32 hasAlpha = 0;
				Choose16BitFormat(raster->format, rgba, count, &glFmt, &glType, &hasAlpha);
				PackRGBAto16(rgba, tex16, count, glType);

				nat->internalFormat = (int32)glFmt;
				nat->format         = glFmt;
				nat->type           = glType;
				nat->hasAlpha       = hasAlpha;
				nat->numLevels      = 1;

				gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
				notifyTextureBound(nat->texid);
				gl->glTexImage2D(GL_TEXTURE_2D, 0, (int)glFmt, raster->width, raster->height, 0, glFmt, glType, tex16);

				// Texture is now resident in GL; release CPU pixel copy to save Zeebo's 64MB heap
				rwFree(nat->pixels);
				nat->pixels = nil;
			}
			if(tempRgba && rgba) rwFree(rgba);
			if(temp16 && tex16) rwFree(tex16);
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
			for(int32 x = 0; x < w; x++){
				dst[x*4+0] = src[x*4+2];
				dst[x*4+1] = src[x*4+1];
				dst[x*4+2] = src[x*4+0];
				dst[x*4+3] = src[x*4+3];
			}
		}else if(image->depth == 24){
			for(int32 x = 0; x < w; x++){
				dst[x*4+0] = src[x*3+2];
				dst[x*4+1] = src[x*3+1];
				dst[x*4+2] = src[x*3+0];
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
		ConvertToRGBA(raster->format, nat->pixels, img->pixels, raster->width * raster->height);
	}
	return img;
}

// Decode a single D3D/TXD mip level directly into an RGBA8888 buffer (R,G,B,A byte order)
static bool DecodeD3DLevelToRGBA(const uint8 *buf, uint32 size, const uint8 *pal, int32 pallength,
                                 uint32 format, int32 lw, int32 lh, int32 compression, uint8 *outRGBA)
{
	int32 count = lw * lh;
	if(pallength != 0 && pal){
		if(format & Raster::PAL4){
			int32 maxPal = pallength;
			for(int32 i = 0; i < count; i++){
				uint32 byteIdx = (uint32)(i >> 1);
				uint8 b = (byteIdx < size) ? buf[byteIdx] : 0;
				uint8 idx = (i & 1) ? (b >> 4) : (b & 0xF);
				if(idx >= maxPal) idx = 0;
				outRGBA[i*4+0] = pal[idx*4+0];
				outRGBA[i*4+1] = pal[idx*4+1];
				outRGBA[i*4+2] = pal[idx*4+2];
				outRGBA[i*4+3] = pal[idx*4+3];
			}
		}else{
			int32 maxPal = pallength;
			for(int32 i = 0; i < count; i++){
				uint8 idx = ((uint32)i < size) ? buf[i] : 0;
				if(idx >= maxPal) idx = 0;
				outRGBA[i*4+0] = pal[idx*4+0];
				outRGBA[i*4+1] = pal[idx*4+1];
				outRGBA[i*4+2] = pal[idx*4+2];
				outRGBA[i*4+3] = pal[idx*4+3];
			}
		}
		return true;
	}
	if(compression){
		Image *img = Image::create(lw, lh, 32);
		if(!img) return false;
		img->allocate();
		img->setPixelsDXT(compression, (uint8*)buf);
		// Keep exact channel convention as rasterFromImage + ConvertToRGBA (swap R/B)
		ConvertToRGBA(Raster::C8888, img->pixels, outRGBA, count);
		img->destroy();
		return true;
	}
	ConvertToRGBA(format, buf, outRGBA, count);
	return true;
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

	// On Zeebo, skip top mip levels above 256x256 when mipmaps exist (4x faster upload & 4x less VRAM)
	while(numLevels > 1 && IsPowerOfTwo(width) && IsPowerOfTwo(height) && (width > 256 || height > 256)){
		uint32 skipSize = stream->readU32();
		stream->seek(skipSize);
		width  = (width  > 1) ? (width  >> 1) : 1;
		height = (height > 1) ? (height >> 1) : 1;
		numLevels--;
	}

	const bool useMipmaps = (numLevels > 1) && IsPowerOfTwo(width) && IsPowerOfTwo(height);
	int32 rfmt = (format & 0x0F00) | (type & 0x07);
	if((rfmt & 0x0F00) == 0) rfmt |= (depth == 16) ? Raster::C565 : Raster::C8888;
	if(useMipmaps) rfmt |= Raster::MIPMAP;

	Raster *raster = nil;
	if(width > 0 && height > 0){
		raster = Raster::create(width, height, depth > 0 ? depth : 32, rfmt);
	}

	const IGLVtbl *gl = ZeeboGL();
	Gles1Raster *nat = raster ? GETGLES1RASTEREXT(raster) : nil;

	static uint8 *s_mipReadBuf = nil;
	static size_t s_mipReadBufCap = 0;
	static uint8 *s_mipRgbaA = nil;
	static size_t s_mipRgbaACap = 0;
	static uint8 *s_mipRgbaB = nil;
	static size_t s_mipRgbaBCap = 0;
	static uint8 *s_mipTex16 = nil;
	static size_t s_mipTex16Cap = 0;

	uint32 chosenFmt = GL_RGB;
	uint32 chosenType = GL_UNSIGNED_SHORT_5_6_5;
	bool32 chosenAlpha = 0;
	int32  uploadedLevels = 0;
	int32  lastW = width, lastH = height;
	uint8 *lastRgba = nil;
	bool   tempLastRgba = false;

	int32 lw = width;
	int32 lh = height;
	for(int32 i = 0; i < numLevels; i++){
		uint32 size = stream->readU32();
		if(!raster || !nat || !nat->texid || !gl || (!useMipmaps && i > 0)){
			stream->seek(size);
			continue;
		}

		bool tempRead = false;
		uint8 *buf = AllocScratch(&s_mipReadBuf, &s_mipReadBufCap, size > 0 ? size : 1u, 256u * 1024u, &tempRead);
		if(!buf){
			stream->seek(size);
			continue;
		}
		stream->read8(buf, size);

		int32 count = lw * lh;
		bool tempRgba = false, temp16 = false;
		uint8 *rgba = AllocScratch(&s_mipRgbaA, &s_mipRgbaACap, (size_t)count * 4u, 256u * 1024u, &tempRgba);
		uint16 *tex16 = (uint16*)AllocScratch(&s_mipTex16, &s_mipTex16Cap, (size_t)count * 2u, 128u * 1024u, &temp16);

		if(rgba && tex16 && DecodeD3DLevelToRGBA(buf, size, pal, pallength, (uint32)rfmt, lw, lh, compression, rgba)){
			if(i == 0){
				Choose16BitFormat((uint32)rfmt, rgba, count, &chosenFmt, &chosenType, &chosenAlpha);
				nat->internalFormat = (int32)chosenFmt;
				nat->format         = chosenFmt;
				nat->type           = chosenType;
				nat->hasAlpha       = chosenAlpha;
				gl->glBindTexture(GL_TEXTURE_2D, nat->texid);
				notifyTextureBound(nat->texid);
			}
			PackRGBAto16(rgba, tex16, count, chosenType);
			gl->glTexImage2D(GL_TEXTURE_2D, i, (int)chosenFmt, lw, lh, 0, chosenFmt, chosenType, tex16);
			uploadedLevels = i + 1;
			lastW = lw;
			lastH = lh;

			// Keep last level's RGBA only if we need to finish the mipchain down to 1x1
			if(useMipmaps && i == numLevels - 1 && (lw > 1 || lh > 1)){
				lastRgba = rgba;
				tempLastRgba = tempRgba;
				tempRgba = false; // ownership transferred until after tail generation
			}
		}

		if(tempRead) rwFree(buf);
		if(tempRgba && rgba) rwFree(rgba);
		if(temp16 && tex16) rwFree(tex16);

		lw = (lw > 1) ? (lw >> 1) : 1;
		lh = (lh > 1) ? (lh >> 1) : 1;
	}

	// OpenGL ES requires mipmap chains to be complete down to 1x1
	if(raster && nat && nat->texid && gl && useMipmaps && uploadedLevels > 0 && lastRgba){
		uint8 *curSrc = lastRgba;
		bool   curTemp = tempLastRgba;
		int32  cw = lastW;
		int32  ch = lastH;
		int32  lvl = uploadedLevels;
		while(cw > 1 || ch > 1){
			int32 nw = (cw > 1) ? (cw >> 1) : 1;
			int32 nh = (ch > 1) ? (ch >> 1) : 1;
			int32 ncount = nw * nh;
			bool tempDst = false, temp16 = false;
			uint8 *dst = (curSrc == s_mipRgbaA)
				? AllocScratch(&s_mipRgbaB, &s_mipRgbaBCap, (size_t)ncount * 4u, 256u * 1024u, &tempDst)
				: AllocScratch(&s_mipRgbaA, &s_mipRgbaACap, (size_t)ncount * 4u, 256u * 1024u, &tempDst);
			uint16 *tex16 = (uint16*)AllocScratch(&s_mipTex16, &s_mipTex16Cap, (size_t)ncount * 2u, 128u * 1024u, &temp16);
			if(!dst || !tex16){
				if(tempDst && dst) rwFree(dst);
				if(temp16 && tex16) rwFree(tex16);
				break;
			}
			DownsampleRGBA2x2(curSrc, cw, ch, dst, nw, nh);
			PackRGBAto16(dst, tex16, ncount, chosenType);
			gl->glTexImage2D(GL_TEXTURE_2D, lvl, (int)chosenFmt, nw, nh, 0, chosenFmt, chosenType, tex16);
			if(temp16) rwFree(tex16);
			if(curTemp && curSrc) rwFree(curSrc);
			curSrc = dst;
			curTemp = tempDst;
			cw = nw;
			ch = nh;
			lvl++;
			uploadedLevels = lvl;
		}
		if(curTemp && curSrc) rwFree(curSrc);
	}else if(tempLastRgba && lastRgba){
		rwFree(lastRgba);
	}

	if(raster && nat && nat->texid && gl && uploadedLevels > 0){
		nat->numLevels = uploadedLevels;
		if(uploadedLevels > 1){
			nat->minFilter = GL_NEAREST_MIPMAP_NEAREST;
			nat->magFilter = GL_NEAREST;
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
			gl->glTexParameterx(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
