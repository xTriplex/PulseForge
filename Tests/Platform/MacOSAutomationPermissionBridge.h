#pragma once

#include <stdint.h>

int32_t PFProbeSystemEventsDescriptor(void);
int32_t PFReadSystemEventsNativeAddress(void);
int32_t PFTypeWildCardEventClass(void);
int32_t PFTypeWildCardEventID(void);
int32_t PFDetermineSystemEventsPermission(void);
