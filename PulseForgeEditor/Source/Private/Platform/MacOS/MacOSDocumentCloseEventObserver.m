#import <AppKit/AppKit.h>

#include "Editor/MacOSDocumentCloseEventObserver.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct PulseForgeMacOSDocumentCloseEventObserver
{
	id Monitor;
	char* Scenario;
	char* HostID;
	char* AssetID;
	uint32_t EventCount;
	size_t PromptGeneration;
} PulseForgeMacOSDocumentCloseEventObserver;

static char* PulseForgeCopyString(const char* Value)
{
	return strdup(Value ? Value : "");
}

void* PulseForgeCreateMacOSDocumentCloseEventObserver()
{
	return calloc(1, sizeof(PulseForgeMacOSDocumentCloseEventObserver));
}

void PulseForgeStartMacOSDocumentCloseEventObserver(void* OpaqueObserver, const char* Scenario, const char* HostID,
	const char* AssetID)
{
	PulseForgeMacOSDocumentCloseEventObserver* Observer = OpaqueObserver;
	if (!Observer || Observer->Monitor || ![NSThread isMainThread]) return;
	Observer->Scenario = PulseForgeCopyString(Scenario);
	Observer->HostID = PulseForgeCopyString(HostID);
	Observer->AssetID = PulseForgeCopyString(AssetID);
	if (!Observer->Scenario || !Observer->HostID || !Observer->AssetID)
	{
		fprintf(stderr, "PF_DOC_CLOSE_DIAG|COCOA_LOCAL_MONITOR|started=0|reason=label-allocation-failed|pid=%d\n", getpid());
		fflush(stderr);
		return;
	}
	const NSEventMask Mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp;
	Observer->Monitor = [[NSEvent addLocalMonitorForEventsMatchingMask:Mask handler:^NSEvent*(NSEvent* Event)
	{
		if (Observer->EventCount < 32)
		{
			const char* Action = Event.type == NSEventTypeLeftMouseDown ? "down" : "up";
			NSWindow* KeyWindow = NSApp.keyWindow;
			const NSPoint ScreenPoint = Event.window ? [Event.window convertPointToScreen:Event.locationInWindow] : NSZeroPoint;
			fprintf(stderr,
				"PF_DOC_CLOSE_DIAG|COCOA_LOCAL_EVENT|scenario=%s|host=%s|asset=%s|generation=%zu|sequence=%u|pid=%d|action=%s|"
				"event_number=%ld|timestamp=%.6f|window_number=%ld|key_window_number=%ld|key=%d|click_count=%ld|"
				"local=%.2f,%.2f|screen=%.2f,%.2f\n",
				Observer->Scenario, Observer->HostID, Observer->AssetID, Observer->PromptGeneration, ++Observer->EventCount, getpid(), Action,
				(long)Event.eventNumber, Event.timestamp, (long)Event.windowNumber,
				(long)(KeyWindow ? KeyWindow.windowNumber : 0), Event.window.isKeyWindow ? 1 : 0,
				(long)Event.clickCount, Event.locationInWindow.x, Event.locationInWindow.y,
				ScreenPoint.x, ScreenPoint.y);
			fflush(stderr);
		}
		return Event;
	}] retain];
	fprintf(stderr, "PF_DOC_CLOSE_DIAG|COCOA_LOCAL_MONITOR|scenario=%s|host=%s|asset=%s|started=%d|pid=%d\n",
		Observer->Scenario, Observer->HostID, Observer->AssetID, Observer->Monitor ? 1 : 0, getpid());
	fflush(stderr);
}

void PulseForgeUpdateMacOSDocumentCloseEventObserver(void* OpaqueObserver, size_t PromptGeneration)
{
	PulseForgeMacOSDocumentCloseEventObserver* Observer = OpaqueObserver;
	if (Observer) Observer->PromptGeneration = PromptGeneration;
}

void PulseForgeDestroyMacOSDocumentCloseEventObserver(void* OpaqueObserver)
{
	PulseForgeMacOSDocumentCloseEventObserver* Observer = OpaqueObserver;
	if (!Observer) return;
	if (Observer->Monitor)
	{
		[NSEvent removeMonitor:Observer->Monitor];
		[Observer->Monitor release];
	}
	free(Observer->Scenario);
	free(Observer->HostID);
	free(Observer->AssetID);
	free(Observer);
}
