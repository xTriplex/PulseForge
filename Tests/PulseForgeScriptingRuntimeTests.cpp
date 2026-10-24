#include "TestEngineSupport.h"

void RunScriptRuntimeTests(TestRunner& Tests);
void RunPlayerSessionTests(TestRunner& Tests);
void RunGameplayInputTests(TestRunner& Tests);
void RunGameplayInputLifecycleTests(TestRunner& Tests);
void RunGameplayInputStoreTests(TestRunner& Tests);

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	RunScriptRuntimeTests(Tests);
	RunPlayerSessionTests(Tests);
	RunGameplayInputTests(Tests);
	RunGameplayInputLifecycleTests(Tests);
	RunGameplayInputStoreTests(Tests);
	return Tests.Finish();
}
