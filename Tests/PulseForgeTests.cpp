#include "TestCoreSupport.h"

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunCoreTests(Tests);
	return Tests.Finish();
}
