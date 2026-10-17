#include "TestSupport.h"

void RunTransparencyImageAnalysisTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunPointShadowImageAnalysisTests(Tests);
	RunTransparencyImageAnalysisTests(Tests);
	return Tests.Finish();
}
