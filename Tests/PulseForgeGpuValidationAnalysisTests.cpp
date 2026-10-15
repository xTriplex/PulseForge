#include "TestSupport.h"

int main()
{
	TestRunner Tests;
	RunPointShadowImageAnalysisTests(Tests);
	return Tests.Finish();
}
