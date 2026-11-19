// custompipes_zeebo.cpp - GLES1 pipeline & WorldRender hooks for Zeebo.
#ifdef __ZEEBO__

#include "common.h"
#include "rwcore.h"
#include "custompipes.h"

namespace WorldRender {

int numBlendInsts[3] = { 0, 0, 0 };

void AtomicFullyTransparent(RpAtomic *atomic, int pass, int fadeAlpha)
{
	(void)atomic; (void)pass; (void)fadeAlpha;
}

void AtomicFirstPass(RpAtomic *atomic, int pass)
{
	(void)pass;
	if(atomic) RpAtomicRender(atomic);
}

void RenderBlendPass(int pass)
{
	(void)pass;
}

} // namespace WorldRender

namespace CustomPipes {

void CreateWorldPipe(void) {}
void DestroyWorldPipe(void) {}

void CreateEnvMapPipe(void) {}
void DestroyEnvMapPipe(void) {}

void CreateVehiclePipe(void) {}
void DestroyVehiclePipe(void) {}

void CreateGlossPipe(void) {}
void DestroyGlossPipe(void) {}

void CreateRimLightPipes(void) {}
void DestroyRimLightPipes(void) {}

} // namespace CustomPipes

#endif // __ZEEBO__
