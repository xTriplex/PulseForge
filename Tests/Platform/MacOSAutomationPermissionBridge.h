#pragma once

#include <stdint.h>
#import <Foundation/Foundation.h>

@interface PFAutomationPermissionBridge : NSObject

- (int32_t)probeSystemEventsDescriptor;
- (int32_t)readSystemEventsNativeAddress;
- (int32_t)typeWildCardEventClass;
- (int32_t)typeWildCardEventID;
- (int32_t)determineSystemEventsPermission;
- (int32_t)preflightQuartzEventAccess;

@end
