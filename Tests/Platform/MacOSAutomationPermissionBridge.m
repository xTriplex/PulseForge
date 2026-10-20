#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

enum
{
	PFPermissionDescriptorUnavailable = -32001,
	PFPermissionAddressUnavailable = -32002,
	PFPermissionApiUnsupported = -32003,
	PFPermissionBridgeFailure = -32004
};

static NSAppleEventDescriptor* s_SystemEventsDescriptor;

static NSAppleEventDescriptor* MakeSystemEventsDescriptor(void)
{
	return [NSAppleEventDescriptor descriptorWithBundleIdentifier:@"com.apple.systemevents"];
}

int32_t PFProbeSystemEventsDescriptor(void)
{
	@autoreleasepool
	{
		@try
		{
			s_SystemEventsDescriptor = MakeSystemEventsDescriptor();
			if (s_SystemEventsDescriptor == nil)
				return PFPermissionDescriptorUnavailable;
			return 0;
		}
		@catch (NSException* exception)
		{
			return PFPermissionBridgeFailure;
		}
	}
}

int32_t PFReadSystemEventsNativeAddress(void)
{
	@autoreleasepool
	{
		@try
		{
			if (s_SystemEventsDescriptor == nil)
				return PFPermissionDescriptorUnavailable;
			AEAddressDesc address = s_SystemEventsDescriptor.aeDesc;
			if (address.descriptorType == typeNull || address.dataHandle == NULL)
				return PFPermissionAddressUnavailable;
			return (int32_t)address.descriptorType;
		}
		@catch (NSException* exception)
		{
			return PFPermissionBridgeFailure;
		}
	}
}

int32_t PFTypeWildCardEventClass(void)
{
	return (int32_t)typeWildCard;
}

int32_t PFTypeWildCardEventID(void)
{
	return (int32_t)typeWildCard;
}

int32_t PFDetermineSystemEventsPermission(void)
{
	@autoreleasepool
	{
		@try
		{
			if (s_SystemEventsDescriptor == nil)
				return PFPermissionDescriptorUnavailable;

			AEAddressDesc address = s_SystemEventsDescriptor.aeDesc;
			if (address.descriptorType == typeNull || address.dataHandle == NULL)
				return PFPermissionAddressUnavailable;

			if (@available(macOS 10.14, *))
				return (int32_t)AEDeterminePermissionToAutomateTarget(&address, typeWildCard, typeWildCard, false);
			return PFPermissionApiUnsupported;
		}
		@catch (NSException* exception)
		{
			return PFPermissionBridgeFailure;
		}
	}
}
