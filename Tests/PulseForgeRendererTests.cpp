#include "TestSupport.h"

void RunRendererPolicyTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunRendererPolicyTests(Tests);
	return Tests.Finish();
}
