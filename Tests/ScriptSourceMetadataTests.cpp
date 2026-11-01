#include "TestAssetSupport.h"

#include "Assets/ScriptSourceMetadata.h"

#include <string>

namespace
{
	std::string PathText(const std::filesystem::path& Path)
	{
		const std::u8string Text = Path.generic_u8string();
		return { reinterpret_cast<const char*>(Text.data()), Text.size() };
	}
}

void RunScriptSourceMetadataTests(TestRunner& Tests)
{
	using namespace PulseForge;
	const auto Empty = ParseScriptSourceMetadata("function OnCreate() end\n");
	PF_CHECK(Tests, Empty && !Empty->HasDeclarations && Empty->FloatProperties.empty() && Empty->Modules.empty());

	const std::string Valid = "--[[PulseForgeScript:1\n"
		"{\"version\":1,\"properties\":[{\"id\":\"move.speed\",\"label\":\"Vitesse Ω\",\"type\":\"float\","
		"\"default\":4.5,\"min\":0,\"max\":20}],\"modules\":[\"shared/math\",\"./local\"]}\n]]\n"
		"function OnCreate() end\n";
	const auto Parsed = ParseScriptSourceMetadata(Valid);
	PF_CHECK(Tests, Parsed && Parsed->HasDeclarations && Parsed->FloatProperties.size() == 1 && Parsed->Modules.size() == 2);
	const auto ParsedWindowsLineEndings = ParseScriptSourceMetadata(
		"--[[PulseForgeScript:1\r\n{\"version\":1,\"modules\":[\"shared/math\"]}\r\n]]\r\nreturn true\r\n");
	PF_CHECK(Tests, ParsedWindowsLineEndings && ParsedWindowsLineEndings->Modules.size() == 1 &&
		ParsedWindowsLineEndings->Modules.front() == "shared/math");
	if (Parsed && !Parsed->FloatProperties.empty())
	{
		const ScriptFloatPropertyDeclaration& Property = Parsed->FloatProperties.front();
		PF_CHECK(Tests, Property.ID == "move.speed" && Property.Label == "Vitesse Ω");
		PF_CHECK(Tests, Property.DefaultValue == 4.5f && Property.Minimum == 0.0f && Property.Maximum == 20.0f);
	}

	const auto Nested = ResolveScriptModuleAssetPath("Assets/Scripts/actors/player.lua", "../shared/math");
	PF_CHECK(Tests, Nested && PathText(*Nested) == "Assets/Scripts/shared/math.lua");
	const auto SameDirectory = ResolveScriptModuleAssetPath("Assets/Scripts/actors/player.lua", "./movement");
	PF_CHECK(Tests, SameDirectory && PathText(*SameDirectory) == "Assets/Scripts/actors/movement.lua");
	PF_CHECK(Tests, !ResolveScriptModuleAssetPath("Assets/Scripts/player.lua", "../../outside"));
	PF_CHECK(Tests, !ResolveScriptModuleAssetPath("Assets/Scripts/player.lua", "C:/Scripts/module"));
	PF_CHECK(Tests, !ResolveScriptModuleAssetPath("Assets/Scripts/player.lua", "module.lua"));
	PF_CHECK(Tests, !ResolveScriptModuleAssetPath("Assets/Scripts/player.lua", "folder\\module"));
	PF_CHECK(Tests, !ResolveScriptModuleAssetPath("Assets/Other/player.lua", "module"));

	const auto InvalidJSON = ParseScriptSourceMetadata("--[[PulseForgeScript:1\n{\n\"version\":1,\n]\n]]\n");
	PF_CHECK(Tests, !InvalidJSON && InvalidJSON.error().Line == 4 && !InvalidJSON.error().Message.empty());
	const auto InvalidJSONImmediatelyAfterMarker = ParseScriptSourceMetadata(
		"--[[PulseForgeScript:1\n]\n]]\n");
	PF_CHECK(Tests, !InvalidJSONImmediatelyAfterMarker && InvalidJSONImmediatelyAfterMarker.error().Line == 2);
	const auto InvalidJSONSeveralLinesIntoMetadata = ParseScriptSourceMetadata(
		"--[[PulseForgeScript:1\n{\n\"version\":1,\n\"properties\":[\n]\n]]\n");
	PF_CHECK(Tests, !InvalidJSONSeveralLinesIntoMetadata && InvalidJSONSeveralLinesIntoMetadata.error().Line == 5);
	const auto InvalidJSONCRLF = ParseScriptSourceMetadata(
		"--[[PulseForgeScript:1\r\n{\r\n\"version\":1,\r\n]\r\n]]\r\n");
	PF_CHECK(Tests, !InvalidJSONCRLF && InvalidJSONCRLF.error().Line == 4);
	const auto TruncatedJSONAtBlockEnd = ParseScriptSourceMetadata(
		"--[[PulseForgeScript:1\n{\n\"version\":1,\n\"properties\":[\n{\n]]\n");
	PF_CHECK(Tests, !TruncatedJSONAtBlockEnd && TruncatedJSONAtBlockEnd.error().Line == 5);
	PF_CHECK(Tests, !ParseScriptSourceMetadata("--[[PulseForgeScript:1\n{\"version\":2}\n]]\n"));
	PF_CHECK(Tests, !ParseScriptSourceMetadata("--[[PulseForgeScript:1\n{\"version\":1,\"properties\":["
		"{\"id\":\"same\",\"label\":\"One\",\"type\":\"float\",\"default\":0,\"min\":0,\"max\":1},"
		"{\"id\":\"same\",\"label\":\"Two\",\"type\":\"float\",\"default\":0,\"min\":0,\"max\":1}]}\n]]"));
	PF_CHECK(Tests, !ParseScriptSourceMetadata("--[[PulseForgeScript:1\n{\"version\":1,\"properties\":["
		"{\"id\":\"speed\",\"label\":\"Speed\",\"type\":\"integer\",\"default\":1,\"min\":0,\"max\":2}]}\n]]"));
	PF_CHECK(Tests, !ParseScriptSourceMetadata("--[[PulseForgeScript:1\n{\"version\":1,\"modules\":[\"../../outside\"]}\n]]"));

	std::string Oversized = "--[[PulseForgeScript:1\n{";
	Oversized.append(64u * 1024u, ' ');
	Oversized += "}\n]]";
	PF_CHECK(Tests, !ParseScriptSourceMetadata(Oversized));
}
