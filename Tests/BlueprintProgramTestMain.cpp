#include "TestSupport.h"

void RunBlueprintProgramTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunBlueprintProgramTests(Tests);
	return Tests.Finish();
}
