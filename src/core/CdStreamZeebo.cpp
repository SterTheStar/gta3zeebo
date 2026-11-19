// CdStreamZeebo.cpp - Synchronous BREW IFileMgr / stdio CD streaming for Zeebo.
#ifdef __ZEEBO__

#include "common.h"
#include "CdStream.h"
#include "rwcore.h"
#include "MemoryMgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CDIMAGES 8

#ifdef FLUSHABLE_STREAMING
bool flushStream[MAX_CDCHANNELS];
#endif

struct CdImageEntry {
	FILE  *fp;
	char   name[260];
	uint32 size;
};

static CdImageEntry s_images[MAX_CDIMAGES];
static int32        s_numImages = 0;
static int32        s_numChannels = 0;
static int32        s_lastPosn = 0;
static uint8       *s_channelStatus = nil;

void CdStreamInitThread(void)
{
}

void CdStreamInit(int32 numChannels)
{
	s_numChannels = numChannels;
	if(s_channelStatus) free(s_channelStatus);
	s_channelStatus = (uint8*)calloc((size_t)(numChannels > 0 ? numChannels : 1), sizeof(uint8));
	for(int32 i = 0; i < numChannels; i++)
		s_channelStatus[i] = STREAM_NONE;
}

uint32 GetGTA3ImgSize(void)
{
	return (s_numImages > 0) ? s_images[0].size : 0;
}

void CdStreamShutdown(void)
{
	CdStreamRemoveImages();
	if(s_channelStatus){
		free(s_channelStatus);
		s_channelStatus = nil;
	}
	s_numChannels = 0;
}

int32 CdStreamRead(int32 channel, void *buffer, uint32 offset, uint32 size)
{
	if(channel < 0 || channel >= s_numChannels || !buffer)
		return STREAM_ERROR;

	int32 imgIdx = (int32)_GET_INDEX(offset);
	uint32 sectorOff = _GET_OFFSET(offset);
	if(imgIdx < 0 || imgIdx >= s_numImages || !s_images[imgIdx].fp){
		s_channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}

	uint32 byteOff  = sectorOff * CDSTREAM_SECTOR_SIZE;
	uint32 byteSize = size * CDSTREAM_SECTOR_SIZE;

	FILE *fp = s_images[imgIdx].fp;
	if(fseek(fp, (long)byteOff, SEEK_SET) != 0){
		s_channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}
	size_t nread = fread(buffer, 1, byteSize, fp);
	if(nread == 0 && byteSize > 0){
		s_channelStatus[channel] = STREAM_ERROR;
		return STREAM_ERROR;
	}
	s_lastPosn = (int32)(offset + size);
	s_channelStatus[channel] = STREAM_NONE;
	return STREAM_SUCCESS;
}

int32 CdStreamGetStatus(int32 channel)
{
	if(channel < 0 || channel >= s_numChannels || !s_channelStatus)
		return STREAM_NONE;
	return s_channelStatus[channel];
}

int32 CdStreamGetLastPosn(void)
{
	return s_lastPosn;
}

int32 CdStreamSync(int32 channel)
{
	if(channel < 0 || channel >= s_numChannels || !s_channelStatus)
		return STREAM_NONE;
	s_channelStatus[channel] = STREAM_NONE;
	return STREAM_NONE;
}

void AddToQueue(Queue *queue, int32 item)
{
	if(!queue || !queue->items || queue->size <= 0) return;
	queue->items[queue->tail] = item;
	queue->tail = (queue->tail + 1) % queue->size;
}

int32 GetFirstInQueue(Queue *queue)
{
	if(!queue || !queue->items || queue->head == queue->tail) return -1;
	return queue->items[queue->head];
}

void RemoveFirstInQueue(Queue *queue)
{
	if(!queue || !queue->items || queue->head == queue->tail) return;
	queue->head = (queue->head + 1) % queue->size;
}

bool CdStreamAddImage(char const *path)
{
	if(!path || s_numImages >= MAX_CDIMAGES) return false;
	FILE *fp = fopen(path, "rb");
	if(!fp) return false;

	fseek(fp, 0, SEEK_END);
	long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	CdImageEntry &e = s_images[s_numImages];
	e.fp = fp;
	strncpy(e.name, path, sizeof(e.name) - 1);
	e.name[sizeof(e.name) - 1] = '\0';
	e.size = (sz > 0) ? (uint32)sz : 0;
	s_numImages++;
	return true;
}

char *CdStreamGetImageName(int32 cd)
{
	if(cd < 0 || cd >= s_numImages) return nil;
	return s_images[cd].name;
}

void CdStreamRemoveImages(void)
{
	for(int32 i = 0; i < s_numImages; i++){
		if(s_images[i].fp){
			fclose(s_images[i].fp);
			s_images[i].fp = nil;
		}
		s_images[i].name[0] = '\0';
		s_images[i].size = 0;
	}
	s_numImages = 0;
}

int32 CdStreamGetNumImages(void)
{
	return s_numImages;
}

#endif // __ZEEBO__
