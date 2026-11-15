#include "TestSupport.h"

void RunGltfAnimationSamplerTests(TestRunner& Tests);

int main()
{
	TestRunner Tests;
	RunGltfAnimationSamplerTests(Tests);
	return Tests.Finish();
}
