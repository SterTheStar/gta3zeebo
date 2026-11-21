#include "common.h"
#include "Pools.h"
#include "Lists.h"

void*
CPtrNode::operator new(size_t){
	if(!CPools::GetPtrNodePool())
		return nil;
	CPtrNode *node = CPools::GetPtrNodePool()->New();
	assert(node);
	return node;
}

void
CPtrNode::operator delete(void *p, size_t){
	if(CPools::GetPtrNodePool())
		CPools::GetPtrNodePool()->Delete((CPtrNode*)p);
}

void*
CEntryInfoNode::operator new(size_t){
	if(!CPools::GetEntryInfoNodePool())
		return nil;
	CEntryInfoNode *node = CPools::GetEntryInfoNodePool()->New();
	assert(node);
	return node;
}
void
CEntryInfoNode::operator delete(void *p, size_t){
	if(CPools::GetEntryInfoNodePool())
		CPools::GetEntryInfoNodePool()->Delete((CEntryInfoNode*)p);
}
