#include "TestSupport.h"

#include "Assets/ExportBuildProfile.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace
{
	using namespace PulseForge;

	ExportBuildProfile MakeProfile()
	{
		ExportBuildProfile Profile;
		Profile.ProfileID = UUID{ 0x1100000000000000ull, 1 };
		Profile.DisplayName = "Portable Alpha Ω";
		Profile.TargetPlatform = ExportTargetPlatform::Windows;
		Profile.Architecture = ExportTargetArchitecture::X64;
		Profile.Configuration = CookedBuildConfiguration::Release;
		Profile.Renderer = ExportRendererBackend::Vulkan;
		Profile.StartupScene = UUID{ 0x2200000000000000ull, 2 };
		Profile.DynamicRoots = {
			UUID{ 0x4400000000000000ull, 4 },
			UUID{ 0x3300000000000000ull, 3 }
		};
		Profile.DefaultOutputPath = std::filesystem::path("Build") / "Exports" / "Alpha";
		Profile.Window = ExportWindowSettings{ 1600, 900 };
		return Profile;
	}

	bool ReplaceOnce(std::string& Value, std::string_view Find, std::string_view Replacement)
	{
		const size_t Position = Value.find(Find);
		if (Position == std::string::npos) return false;
		Value.replace(Position, Find.size(), Replacement);
		return true;
	}

	void TestDeterministicRoundTrip(TestRunner& Tests)
	{
		ExportBuildProfile Profile = MakeProfile();
		auto Serialized = ExportBuildProfileSerializer::Serialize(Profile);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		std::reverse(Profile.DynamicRoots.begin(), Profile.DynamicRoots.end());
		auto Reordered = ExportBuildProfileSerializer::Serialize(Profile);
		PF_CHECK(Tests, Reordered.has_value());
		PF_CHECK(Tests, Reordered && *Reordered == *Serialized);

		auto Parsed = ExportBuildProfileSerializer::Parse(*Serialized);
		PF_CHECK(Tests, Parsed.has_value());
		if (!Parsed) return;
		PF_CHECK(Tests, Parsed->ProfileID == Profile.ProfileID);
		PF_CHECK(Tests, Parsed->DisplayName == Profile.DisplayName);
		PF_CHECK(Tests, Parsed->TargetPlatform == ExportTargetPlatform::Windows);
		PF_CHECK(Tests, Parsed->Architecture == ExportTargetArchitecture::X64);
		PF_CHECK(Tests, Parsed->Configuration == CookedBuildConfiguration::Release);
		PF_CHECK(Tests, Parsed->StartupScene == Profile.StartupScene);
		PF_CHECK(Tests, Parsed->DynamicRoots.size() == 2);
		PF_CHECK(Tests, std::is_sorted(Parsed->DynamicRoots.begin(), Parsed->DynamicRoots.end()));
		PF_CHECK(Tests, Parsed->DefaultOutputPath == Profile.DefaultOutputPath);
		PF_CHECK(Tests, Parsed->Window == Profile.Window);

		auto ReSerialized = ExportBuildProfileSerializer::Serialize(*Parsed);
		PF_CHECK(Tests, ReSerialized && *ReSerialized == *Serialized);
	}

	void TestVersionAndMalformedDocuments(TestRunner& Tests)
	{
		const auto Serialized = ExportBuildProfileSerializer::Serialize(MakeProfile());
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		std::string MissingVersion = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(MissingVersion, "  \"version\": 1,\n", ""));
		auto Missing = ExportBuildProfileSerializer::Parse(MissingVersion);
		PF_CHECK(Tests, !Missing && Missing.error().Code == ExportBuildProfileErrorCode::InvalidDocument);

		std::string FutureVersion = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(FutureVersion, "\"version\": 1", "\"version\": 99"));
		auto Future = ExportBuildProfileSerializer::Parse(FutureVersion);
		PF_CHECK(Tests, !Future && Future.error().Code == ExportBuildProfileErrorCode::UnsupportedVersion);

		std::string DuplicateField = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(DuplicateField, "\"version\": 1,", "\"version\": 1,\n  \"version\": 1,"));
		auto Duplicate = ExportBuildProfileSerializer::Parse(DuplicateField);
		PF_CHECK(Tests, !Duplicate && Duplicate.error().Code == ExportBuildProfileErrorCode::InvalidDocument);

		const auto Malformed = ExportBuildProfileSerializer::Parse("{\"format\":");
		PF_CHECK(Tests, !Malformed && Malformed.error().Code == ExportBuildProfileErrorCode::InvalidDocument);

		std::string MissingRequired = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(MissingRequired, "  \"startupScene\": \"22000000-0000-0000-0000-000000000002\",\n", ""));
		auto MissingField = ExportBuildProfileSerializer::Parse(MissingRequired);
		PF_CHECK(Tests, !MissingField && MissingField.error().Code == ExportBuildProfileErrorCode::InvalidDocument);
	}

	void TestTargetArchitectureAndOutputValidation(TestRunner& Tests)
	{
		const auto Serialized = ExportBuildProfileSerializer::Serialize(MakeProfile());
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		std::string UnsupportedTarget = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(UnsupportedTarget, "\"targetPlatform\": \"Windows\"", "\"targetPlatform\": \"Solaris\""));
		PF_CHECK(Tests, !ExportBuildProfileSerializer::Parse(UnsupportedTarget));

		std::string UnsupportedArchitecture = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(UnsupportedArchitecture, "\"architecture\": \"x64\"", "\"architecture\": \"riscv64\""));
		PF_CHECK(Tests, !ExportBuildProfileSerializer::Parse(UnsupportedArchitecture));

		std::string UnsupportedCombination = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(UnsupportedCombination, "\"architecture\": \"x64\"", "\"architecture\": \"arm64\""));
		const auto WrongWindowsArchitecture = ExportBuildProfileSerializer::Parse(UnsupportedCombination);
		PF_CHECK(Tests, !WrongWindowsArchitecture && WrongWindowsArchitecture.error().Code == ExportBuildProfileErrorCode::InvalidDocument);

		ExportBuildProfile UnsafePath = MakeProfile();
		UnsafePath.DefaultOutputPath = std::filesystem::path("Build") / ".." / "Outside";
		const auto Unsafe = ExportBuildProfileSerializer::Serialize(UnsafePath);
		PF_CHECK(Tests, !Unsafe && Unsafe.error().Code == ExportBuildProfileErrorCode::InvalidProfile);

		std::string UnsafeParsedPath = *Serialized;
		PF_CHECK(Tests, ReplaceOnce(UnsafeParsedPath, "Build/Exports/Alpha", "../Outside"));
		const auto ParsedUnsafe = ExportBuildProfileSerializer::Parse(UnsafeParsedPath);
		PF_CHECK(Tests, !ParsedUnsafe && ParsedUnsafe.error().Code == ExportBuildProfileErrorCode::InvalidDocument);
	}

	void TestWindowBounds(TestRunner& Tests)
	{
		for (const ExportWindowSettings InvalidWindow : {
				ExportWindowSettings{ 0, 720 },
				ExportWindowSettings{ 1280, 0 },
				ExportWindowSettings{ ExportBuildProfileSerializer::MaximumWindowDimension + 1, 720 },
				ExportWindowSettings{ 1280, ExportBuildProfileSerializer::MaximumWindowDimension + 1 } })
		{
			ExportBuildProfile Invalid = MakeProfile();
			Invalid.Window = InvalidWindow;
			const auto Serialized = ExportBuildProfileSerializer::Serialize(Invalid);
			PF_CHECK(Tests, !Serialized && Serialized.error().Code == ExportBuildProfileErrorCode::InvalidProfile);
		}

		std::string InvalidParsedWindow;
		auto Valid = ExportBuildProfileSerializer::Serialize(MakeProfile());
		PF_CHECK(Tests, Valid.has_value());
		if (!Valid) return;
		InvalidParsedWindow = *Valid;
		PF_CHECK(Tests, ReplaceOnce(InvalidParsedWindow, "\"width\": 1600", "\"width\": 16385"));
		const auto Parsed = ExportBuildProfileSerializer::Parse(InvalidParsedWindow);
		PF_CHECK(Tests, !Parsed && Parsed.error().Code == ExportBuildProfileErrorCode::InvalidDocument);
	}
} // namespace

void RunExportBuildProfileTests(TestRunner& Tests)
{
	TestDeterministicRoundTrip(Tests);
	TestVersionAndMalformedDocuments(Tests);
	TestTargetArchitectureAndOutputValidation(Tests);
	TestWindowBounds(Tests);
}
