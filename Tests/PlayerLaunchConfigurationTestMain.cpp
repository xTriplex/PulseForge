#include "TestSupport.h"

void RunPlayerLaunchConfigurationTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunPlayerLaunchConfigurationTests(Tests);
	return Tests.Finish();
}
