#include "Assets/ScriptSourceMetadata.h"

int main()
{
	const auto Metadata = PulseForge::ParseScriptSourceMetadata("function OnCreate() end\n");
	return Metadata && !Metadata->HasDeclarations ? 0 : 1;
}
