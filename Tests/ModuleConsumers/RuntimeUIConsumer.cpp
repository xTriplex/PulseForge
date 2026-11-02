#include "UI/RuntimeUI.h"

int main()
{
	PulseForge::RuntimeUIDocument Document;
	return PulseForge::ValidateRuntimeUIDocument(Document) ? 0 : 1;
}
