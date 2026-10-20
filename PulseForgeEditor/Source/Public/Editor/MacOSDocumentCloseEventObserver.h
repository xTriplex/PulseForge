#pragma once

#if defined(_DEBUG) || defined(PF_DEBUG)

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

void* PulseForgeCreateMacOSDocumentCloseEventObserver();
void PulseForgeStartMacOSDocumentCloseEventObserver(void* Observer, const char* Scenario, const char* HostID, const char* AssetID);
void PulseForgeUpdateMacOSDocumentCloseEventObserver(void* Observer, size_t PromptGeneration);
void PulseForgeDestroyMacOSDocumentCloseEventObserver(void* Observer);

#ifdef __cplusplus
}
#endif

#endif
