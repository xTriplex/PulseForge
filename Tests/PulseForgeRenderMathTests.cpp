#include "TestSupport.h"

int main()
{
	TestRunner Tests;
	RunRenderMathTests(Tests);
	return Tests.Finish();
}
