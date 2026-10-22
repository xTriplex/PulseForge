#include "TestCoreSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunCoreTests(Tests);
	RunDiagnosticsRecorderTests(Tests);
	return Tests.Finish();
}
