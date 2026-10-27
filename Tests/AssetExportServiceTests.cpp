#include "TestSupport.h"

#include "Assets/AssetExportService.h"
#include "Assets/AssetMetadata.h"
#include "Assets/Project.h"
#include "Scene/SceneSerializer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace
{
	using namespace PulseForge;

	ExportTargetPlatform HostPlatform();
	ExportTargetArchitecture HostArchitecture();

	class TemporaryExportFixture final
	{
	public:
		TemporaryExportFixture()
		{
			const auto Identifier = UUID::Generate();
			if (!Identifier) return;
		std::u8string DirectoryName = u8"PFExport Ω ";
			const std::string IdentifierText = Identifier->ToString();
			DirectoryName.append(IdentifierText.begin(), IdentifierText.end());
			m_Root = std::filesystem::temp_directory_path() / std::filesystem::u8path(DirectoryName);
			std::error_code Error;
			std::filesystem::create_directories(m_Root, Error);
			m_Ready = !Error;
		}

		~TemporaryExportFixture()
		{
			if (m_Root.empty()) return;
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

		[[nodiscard]] bool IsReady() const noexcept { return m_Ready; }
		[[nodiscard]] const std::filesystem::path& Root() const noexcept { return m_Root; }

	private:
		std::filesystem::path m_Root;
		bool m_Ready = false;
	};

	ExportBuildProfile MakeHostProfile()
	{
		ExportBuildProfile Profile;
		Profile.ProfileID = UUID{ 0x7100000000000000ull, 71 };
		Profile.DisplayName = "Export test";
		Profile.TargetPlatform = HostPlatform();
		Profile.Architecture = HostArchitecture();
		Profile.Configuration = CookedBuildConfiguration::Release;
		Profile.Renderer = ExportRendererBackend::Vulkan;
		Profile.StartupScene = UUID{ 0x7200000000000000ull, 72 };
		Profile.DefaultOutputPath = std::filesystem::path("Build") / "Exports" / "Test";
		return Profile;
	}

	ExportTemplateDescriptor MakeHostDescriptor()
	{
		ExportTemplateDescriptor Descriptor;
		Descriptor.TargetPlatform = HostPlatform();
		Descriptor.TargetArchitecture = HostArchitecture();
		Descriptor.Configuration = CookedBuildConfiguration::Release;
		Descriptor.RendererBackend = "Vulkan";
		Descriptor.RuntimeABI = std::string(CookedRuntimeABIIdentifier);
		Descriptor.CookManifestSchema = CookedAssetCatalog::ManifestVersion;
#if defined(_WIN32)
		Descriptor.Entrypoint = "PulseForgeCookedGame.exe";
		Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Shaders/Tonemapping.ps.spv" };
#elif defined(__APPLE__)
		Descriptor.Entrypoint = "Contents/MacOS/PulseForgeCookedGame";
		Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Contents/Resources/Shaders/Tonemapping.ps.spv" };
#else
		Descriptor.Entrypoint = "PulseForgeCookedGame";
		Descriptor.RuntimeFiles = { Descriptor.Entrypoint, "Shaders/Tonemapping.ps.spv" };
#endif
		return Descriptor;
	}

	ExportTargetPlatform HostPlatform()
	{
#if defined(_WIN32)
		return ExportTargetPlatform::Windows;
#elif defined(__APPLE__)
		return ExportTargetPlatform::MacOS;
#else
		return ExportTargetPlatform::Linux;
#endif
	}

	ExportTargetArchitecture HostArchitecture()
	{
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
		return ExportTargetArchitecture::Arm64;
#else
		return ExportTargetArchitecture::X64;
#endif
	}

	ExportTargetPlatform OtherPlatform(ExportTargetPlatform Platform)
	{
		return Platform == ExportTargetPlatform::Windows ? ExportTargetPlatform::Linux : ExportTargetPlatform::Windows;
	}

	bool WriteTextFile(const std::filesystem::path& Path, std::string_view Content)
	{
		std::error_code Error;
		std::filesystem::create_directories(Path.parent_path(), Error);
		if (Error) return false;
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output.write(Content.data(), static_cast<std::streamsize>(Content.size()));
		Output.flush();
		return static_cast<bool>(Output);
	}

	std::string DigestText(std::string_view Text)
	{
		const auto Bytes = std::as_bytes(std::span(Text.data(), Text.size()));
		return ComputeAssetContentFingerprint(Bytes).DigestHex;
	}

	std::string TreeJson(const std::map<std::string, std::string>& Tree)
	{
		std::string Result = "{";
		bool First = true;
		for (const auto& [Name, Digest] : Tree)
		{
			if (!First) Result += ",";
			First = false;
			Result += "\"" + Name + "\":\"" + Digest + "\"";
		}
		return Result + "}";
	}

	std::string PathUtf8(const std::filesystem::path& Path)
	{
		const auto Value = Path.generic_u8string();
		return { reinterpret_cast<const char*>(Value.data()), Value.size() };
	}

	bool PrepareExportProject(
		const std::filesystem::path& Root, std::filesystem::path& ProjectFile, AssetID& StartupScene)
	{
		const auto Source = Root / "Source";
		ProjectFile = Source / "Export.pfproj";
		auto Created = Project::Create(ProjectFile, "Export fixture");
		if (!Created) return false;
		std::error_code Error;
		std::filesystem::create_directories(Source / "Assets", Error);
		if (Error) return false;
		Scene SceneData;
		auto Serialized = SceneSerializer::Serialize(SceneData);
		if (!Serialized || !WriteTextFile(Source / "Assets" / "Startup.scene", *Serialized)) return false;
		auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Source / "Assets" / "Startup.scene");
		if (!Metadata) return false;
		StartupScene = Metadata->ID;
		auto Opened = Project::Open(ProjectFile);
		return Opened && Opened->SetStartScene(StartupScene);
	}

	bool WriteValidTestExecutable(const std::filesystem::path& Path)
	{
		std::array<unsigned char, 70> Header{};
#if defined(_WIN32)
		Header[0] = 'M';
		Header[1] = 'Z';
		Header[0x3c] = 64;
		Header[64] = 'P';
		Header[65] = 'E';
		Header[68] = 0x64;
		Header[69] = 0x86;
#elif defined(__APPLE__)
		Header[0] = 0xcf;
		Header[1] = 0xfa;
		Header[2] = 0xed;
		Header[3] = 0xfe;
		Header[4] = 0x0c;
		Header[7] = 0x01;
#else
		Header[0] = 0x7f;
		Header[1] = 'E';
		Header[2] = 'L';
		Header[3] = 'F';
		Header[4] = 2;
		Header[5] = 1;
		Header[18] = 62;
#endif
		std::error_code Error;
		std::filesystem::create_directories(Path.parent_path(), Error);
		if (Error) return false;
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output.write(reinterpret_cast<const char*>(Header.data()), static_cast<std::streamsize>(Header.size()));
		Output.flush();
		if (!Output) return false;
#if !defined(_WIN32)
		std::filesystem::permissions(Path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::owner_exec | std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
			std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
			std::filesystem::perm_options::replace, Error);
		if (Error) return false;
#endif
		return true;
	}

	bool WriteRecoveryFixture(
		const std::filesystem::path& Output,
		const std::filesystem::path& Candidate,
		const std::filesystem::path& Backup,
		const std::filesystem::path& Root,
		bool HadPreviousOutput,
		std::string_view State,
		const std::map<std::string, std::string>& PreviousTree,
		const std::map<std::string, std::string>& CandidateTree)
	{
		std::string Document = "{\"format\":\"PulseForgeExportRecovery\",\"version\":1,\"operationId\":\"71000000-0000-4000-8000-000000000071\","
			"\"projectId\":\"72000000-0000-4000-8000-000000000072\",\"projectRoot\":\"" + PathUtf8(Root) +
			"\",\"output\":\"" + PathUtf8(Output) + "\",\"candidate\":\"" + PathUtf8(Candidate) +
			"\",\"backup\":\"" + PathUtf8(Backup) + "\",\"hadPreviousOutput\":" +
			(HadPreviousOutput ? "true" : "false") + ",\"state\":\"" + std::string(State) +
			"\",\"previousTree\":" + TreeJson(PreviousTree) + ",\"candidateTree\":" + TreeJson(CandidateTree) + "}";
		const auto OutputName = Output.filename().u8string();
		const std::string Utf8OutputName(reinterpret_cast<const char*>(OutputName.data()), OutputName.size());
		const auto Journal = Output.parent_path() / std::filesystem::u8path("." + Utf8OutputName + ".pf-export-recovery.json");
		return WriteTextFile(Journal, Document);
	}

	AssetExportSnapshot WaitForTerminal(AssetExportService& Service, uint64_t RequestID)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		AssetExportSnapshot Snapshot = Service.GetSnapshot(RequestID);
		while (Snapshot.Running && std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::yield();
			Snapshot = Service.GetSnapshot(RequestID);
		}
		return Snapshot;
	}

	void TestTemplateDescriptorRoundTripAndStrictness(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		const ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
		const auto Serialized = SerializeExportTemplateDescriptor(Descriptor);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;

		const auto DescriptorPath = Fixture.Root() / "player-template.json";
		PF_CHECK(Tests, WriteTextFile(DescriptorPath, *Serialized));
		auto Loaded = LoadExportTemplateDescriptor(DescriptorPath);
		PF_CHECK(Tests, Loaded.has_value());
		if (Loaded)
		{
			PF_CHECK(Tests, Loaded->TargetPlatform == Descriptor.TargetPlatform);
			PF_CHECK(Tests, Loaded->TargetArchitecture == Descriptor.TargetArchitecture);
			PF_CHECK(Tests, Loaded->Configuration == Descriptor.Configuration);
			PF_CHECK(Tests, Loaded->RendererBackend == Descriptor.RendererBackend);
			PF_CHECK(Tests, Loaded->RuntimeABI == Descriptor.RuntimeABI);
			PF_CHECK(Tests, Loaded->CookManifestSchema == Descriptor.CookManifestSchema);
			PF_CHECK(Tests, Loaded->Entrypoint == Descriptor.Entrypoint);
			PF_CHECK(Tests, Loaded->RuntimeFiles == Descriptor.RuntimeFiles);
		}

		ExportTemplateDescriptor WrongAbi = Descriptor;
		WrongAbi.RuntimeABI = "unrecognized-runtime-abi";
		const auto AbiMismatch = SerializeExportTemplateDescriptor(WrongAbi);
		PF_CHECK(Tests, !AbiMismatch && AbiMismatch.error().Code == AssetExportErrorCode::TemplateIncompatible);

		ExportTemplateDescriptor WrongSchema = Descriptor;
		++WrongSchema.CookManifestSchema;
		const auto SchemaMismatch = SerializeExportTemplateDescriptor(WrongSchema);
		PF_CHECK(Tests, !SchemaMismatch && SchemaMismatch.error().Code == AssetExportErrorCode::TemplateIncompatible);

		ExportTemplateDescriptor UnsafeEntry = Descriptor;
		UnsafeEntry.Entrypoint = "../PulseForgeCookedGame.exe";
		const auto UnsafeDescriptor = SerializeExportTemplateDescriptor(UnsafeEntry);
		PF_CHECK(Tests, !UnsafeDescriptor && UnsafeDescriptor.error().Code == AssetExportErrorCode::TemplateIncompatible);
	}

	void TestMalformedAndMissingTemplateDescriptor(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		const auto Missing = LoadExportTemplateDescriptor(Fixture.Root() / "missing-template.json");
		PF_CHECK(Tests, !Missing && Missing.error().Code == AssetExportErrorCode::TemplateMissing);

		const auto MalformedPath = Fixture.Root() / "malformed-template.json";
		PF_CHECK(Tests, WriteTextFile(MalformedPath, "{\"format\":\"PulseForgePlayerTemplate\",\"version\":1"));
		const auto Malformed = LoadExportTemplateDescriptor(MalformedPath);
		PF_CHECK(Tests, !Malformed && Malformed.error().Code == AssetExportErrorCode::TemplateIncompatible);

		const auto MissingFieldsPath = Fixture.Root() / "missing-fields-template.json";
		PF_CHECK(Tests, WriteTextFile(MissingFieldsPath, "{\"format\":\"PulseForgePlayerTemplate\",\"version\":1}"));
		const auto MissingFields = LoadExportTemplateDescriptor(MissingFieldsPath);
		PF_CHECK(Tests, !MissingFields && MissingFields.error().Code == AssetExportErrorCode::TemplateIncompatible);
	}

	void TestTemplateMismatchDoesNotTouchOutput(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		ExportTemplateDescriptor WrongTarget = MakeHostDescriptor();
		WrongTarget.TargetPlatform = OtherPlatform(WrongTarget.TargetPlatform);
		WrongTarget.TargetArchitecture = ExportTargetArchitecture::X64;
		WrongTarget.RuntimeFiles = { WrongTarget.Entrypoint };
		const auto WrongTargetJson = SerializeExportTemplateDescriptor(WrongTarget);
		PF_CHECK(Tests, WrongTargetJson.has_value());
		if (!WrongTargetJson) return;
		const auto DescriptorPath = Fixture.Root() / "wrong-target-template.json";
		PF_CHECK(Tests, WriteTextFile(DescriptorPath, *WrongTargetJson));

		const auto Output = Fixture.Root() / "Package Output Ω";
		PF_CHECK(Tests, !std::filesystem::exists(Output));
		AssetExportRequest Request;
		Request.SourceProjectFile = Fixture.Root() / "source-does-not-need-to-open.pfproj";
		Request.Profile = MakeHostProfile();
		Request.TemplateRoot = Fixture.Root();
		Request.TemplateDescriptor = DescriptorPath;
		Request.OutputOverride = Output;

		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Snapshot = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, !Snapshot.Running);
		PF_CHECK(Tests, Snapshot.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Snapshot.Error && Snapshot.Error->Code == AssetExportErrorCode::TemplateIncompatible);
		PF_CHECK(Tests, !std::filesystem::exists(Output));
		Service.Shutdown();
	}

	void TestMissingTemplateDoesNotTouchOutput(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		const auto Output = Fixture.Root() / "Package Output Ω";
		AssetExportRequest Request;
		Request.SourceProjectFile = Fixture.Root() / "source-does-not-need-to-open.pfproj";
		Request.Profile = MakeHostProfile();
		Request.TemplateRoot = Fixture.Root();
		Request.TemplateDescriptor = Fixture.Root() / "missing-template.json";
		Request.OutputOverride = Output;

		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Snapshot = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, !Snapshot.Running);
		PF_CHECK(Tests, Snapshot.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Snapshot.Error && Snapshot.Error->Code == AssetExportErrorCode::TemplateMissing);
		PF_CHECK(Tests, !std::filesystem::exists(Output));
		Service.Shutdown();
	}

	void TestNativeEntrypointValidationDoesNotTouchOutput(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		const ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
		const auto DescriptorText = SerializeExportTemplateDescriptor(Descriptor);
		PF_CHECK(Tests, DescriptorText.has_value());
		if (!DescriptorText) return;
		const auto DescriptorPath = Fixture.Root() / "template.json";
		PF_CHECK(Tests, WriteTextFile(DescriptorPath, *DescriptorText));
		const auto Entrypoint = Fixture.Root() / std::filesystem::u8path(Descriptor.Entrypoint);
		std::array<char, 64> InvalidExecutableHeader{};
		InvalidExecutableHeader[0] = 'N';
		InvalidExecutableHeader[1] = 'O';
		{
			std::ofstream Output(Entrypoint, std::ios::binary | std::ios::trunc);
			PF_CHECK(Tests, static_cast<bool>(Output));
			Output.write(InvalidExecutableHeader.data(), static_cast<std::streamsize>(InvalidExecutableHeader.size()));
			Output.flush();
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		const auto OutputPath = Fixture.Root() / "Invalid Executable Package";
		AssetExportRequest Request;
		Request.SourceProjectFile = Fixture.Root() / "source-does-not-need-to-open.pfproj";
		Request.Profile = MakeHostProfile();
		Request.TemplateRoot = Fixture.Root();
		Request.TemplateDescriptor = DescriptorPath;
		Request.OutputOverride = OutputPath;

		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Snapshot = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, !Snapshot.Running);
		PF_CHECK(Tests, Snapshot.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Snapshot.Error && Snapshot.Error->Code == AssetExportErrorCode::TemplateIncompatible);
		PF_CHECK(Tests, !std::filesystem::exists(OutputPath));
		Service.Shutdown();
	}

	void TestPosixEntrypointRequiresExecutablePermission(TestRunner& Tests)
	{
#if !defined(_WIN32)
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		const ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
		const auto DescriptorText = SerializeExportTemplateDescriptor(Descriptor);
		PF_CHECK(Tests, DescriptorText.has_value());
		if (!DescriptorText) return;
		const auto Executable = Fixture.Root() / std::filesystem::u8path(Descriptor.Entrypoint);
		PF_CHECK(Tests, WriteValidTestExecutable(Executable));
		std::error_code Error;
		std::filesystem::permissions(Executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
			std::filesystem::perm_options::replace, Error);
		PF_CHECK(Tests, !Error);
		const auto DescriptorPath = Fixture.Root() / "template.json";
		PF_CHECK(Tests, WriteTextFile(DescriptorPath, *DescriptorText));
		AssetExportRequest Request;
		Request.SourceProjectFile = Fixture.Root() / "source-not-opened.pfproj";
		Request.Profile = MakeHostProfile();
		Request.TemplateRoot = Fixture.Root();
		Request.TemplateDescriptor = DescriptorPath;
		Request.OutputOverride = Fixture.Root() / "Non Executable Package";
		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Snapshot = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, Snapshot.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Snapshot.Error && Snapshot.Error->Code == AssetExportErrorCode::TemplateIncomplete);
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root() / "Non Executable Package"));
		Service.Shutdown();
#else
		(void)Tests;
#endif
	}

	void TestInterruptedPublicationRestoresLastGoodPackage(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		const auto Output = Fixture.Root() / "Package";
		const auto Candidate = Fixture.Root() / ".pf-export-candidate-71000000-0000-4000-8000-000000000071";
		const auto Backup = Fixture.Root() / ".pf-export-backup-71000000-0000-4000-8000-000000000071";
		std::error_code Error;
		std::filesystem::create_directories(Backup, Error);
		PF_CHECK(Tests, !Error);
		std::filesystem::create_directories(Candidate, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, WriteTextFile(Backup / "old.txt", "last-good"));
		PF_CHECK(Tests, WriteTextFile(Candidate / "new.txt", "candidate"));
		const std::map<std::string, std::string> OldTree = { { "F:old.txt", DigestText("last-good") } };
		const std::map<std::string, std::string> NewTree = { { "F:new.txt", DigestText("candidate") } };
		PF_CHECK(Tests, WriteRecoveryFixture(Output, Candidate, Backup, Fixture.Root(), true, "OldMoved", OldTree, NewTree));
		PF_CHECK(Tests, std::filesystem::exists(Fixture.Root() / ".Package.pf-export-recovery.json"));

		auto Recovered = AssetExportService::RecoverInterruptedPublication(Output);
		if (!Recovered) std::cerr << "Recovery test reported: " << Recovered.error().Message << " at " << Recovered.error().Path << '\n';
		PF_CHECK(Tests, Recovered.has_value());
		PF_CHECK(Tests, std::filesystem::exists(Output / "old.txt"));
		PF_CHECK(Tests, !std::filesystem::exists(Output / "new.txt"));
		PF_CHECK(Tests, !std::filesystem::exists(Candidate));
		PF_CHECK(Tests, !std::filesystem::exists(Backup));
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root() / ".Package.pf-export-recovery.json"));
		PF_CHECK(Tests, AssetExportService::RecoverInterruptedPublication(Output).has_value());
	}

	void TestInterruptedStagingRecoveryIsExplicitAndSafe(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		const auto Output = Fixture.Root() / "Package";
		const auto Candidate = Fixture.Root() / ".pf-export-candidate-71000000-0000-4000-8000-000000000071";
		const auto Backup = Fixture.Root() / ".pf-export-backup-71000000-0000-4000-8000-000000000071";
		const auto Journal = Fixture.Root() / ".Package.pf-export-recovery.json";
		const std::map<std::string, std::string> OldTree = { { "F:old.txt", DigestText("last-good") } };
		std::error_code Error;
		std::filesystem::create_directories(Output, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, WriteTextFile(Output / "old.txt", "last-good"));
		PF_CHECK(Tests, WriteRecoveryFixture(Output, Candidate, Backup, Fixture.Root(), true, "Staging", OldTree, {}));
		auto Recovered = AssetExportService::RecoverInterruptedPublication(Output);
		PF_CHECK(Tests, Recovered.has_value());
		PF_CHECK(Tests, std::filesystem::exists(Output / "old.txt"));
		PF_CHECK(Tests, !std::filesystem::exists(Journal));

		PF_CHECK(Tests, std::filesystem::create_directory(Candidate, Error));
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, WriteTextFile(Candidate / "partial-template-copy", "incomplete candidate"));
		PF_CHECK(Tests, WriteRecoveryFixture(Output, Candidate, Backup, Fixture.Root(), true, "Staging", OldTree, {}));
		std::ifstream BeforeInput(Journal, std::ios::binary);
		const std::string JournalBefore((std::istreambuf_iterator<char>(BeforeInput)), std::istreambuf_iterator<char>());
		Recovered = AssetExportService::RecoverInterruptedPublication(Output);
		PF_CHECK(Tests, !Recovered && Recovered.error().Code == AssetExportErrorCode::RecoveryRequired);
		std::ifstream AfterInput(Journal, std::ios::binary);
		const std::string JournalAfter((std::istreambuf_iterator<char>(AfterInput)), std::istreambuf_iterator<char>());
		std::ifstream CandidateInput(Candidate / "partial-template-copy", std::ios::binary);
		const std::string CandidateBytes((std::istreambuf_iterator<char>(CandidateInput)), std::istreambuf_iterator<char>());
		PF_CHECK(Tests, JournalAfter == JournalBefore);
		PF_CHECK(Tests, CandidateBytes == "incomplete candidate");
		PF_CHECK(Tests, std::filesystem::exists(Output / "old.txt"));
	}

	void TestInterruptedPublicationPreservesChangedRecoveryData(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		const auto Output = Fixture.Root() / "Package";
		const auto Candidate = Fixture.Root() / ".pf-export-candidate-71000000-0000-4000-8000-000000000071";
		const auto Backup = Fixture.Root() / ".pf-export-backup-71000000-0000-4000-8000-000000000071";
		std::error_code Error;
		std::filesystem::create_directories(Backup, Error);
		std::filesystem::create_directories(Candidate, Error);
		PF_CHECK(Tests, !Error);
		PF_CHECK(Tests, WriteTextFile(Backup / "old.txt", "edited externally"));
		PF_CHECK(Tests, WriteTextFile(Candidate / "new.txt", "candidate"));
		const auto ExpectedPrevious = std::map<std::string, std::string>{ { "F:old.txt", DigestText("last-good") } };
		const auto NewTree = std::map<std::string, std::string>{ { "F:new.txt", DigestText("candidate") } };
		PF_CHECK(Tests, WriteRecoveryFixture(Output, Candidate, Backup, Fixture.Root(), true, "OldMoved", ExpectedPrevious, NewTree));
		const auto RecoveryPath = Fixture.Root() / ".Package.pf-export-recovery.json";
		const auto BeforeChangedFile = std::filesystem::file_size(Backup / "old.txt");

		auto Recovered = AssetExportService::RecoverInterruptedPublication(Output);
		PF_CHECK(Tests, !Recovered && Recovered.error().Code == AssetExportErrorCode::RecoveryRequired);
		PF_CHECK(Tests, std::filesystem::exists(Backup / "old.txt"));
		PF_CHECK(Tests, std::filesystem::file_size(Backup / "old.txt") == BeforeChangedFile);
		PF_CHECK(Tests, std::filesystem::exists(Candidate / "new.txt"));
		PF_CHECK(Tests, std::filesystem::exists(RecoveryPath));
		auto Retried = AssetExportService::RecoverInterruptedPublication(Output);
		PF_CHECK(Tests, !Retried && Retried.error().Code == AssetExportErrorCode::RecoveryRequired);
		PF_CHECK(Tests, std::filesystem::exists(Backup / "old.txt"));
		PF_CHECK(Tests, std::filesystem::exists(RecoveryPath));
	}

	void TestPackageInventoryCannotClaimUnrelatedFiles(TestRunner& Tests)
	{
		using Json = nlohmann::json;
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		std::filesystem::path ProjectFile;
		AssetID StartupScene;
		PF_CHECK(Tests, PrepareExportProject(Fixture.Root(), ProjectFile, StartupScene));
		if (ProjectFile.empty()) return;

		const auto TemplateRoot = Fixture.Root() / "Template";
		std::error_code Error;
		std::filesystem::create_directories(TemplateRoot / "Shaders", Error);
		PF_CHECK(Tests, !Error);
		const ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
		const auto DescriptorJson = SerializeExportTemplateDescriptor(Descriptor);
		PF_CHECK(Tests, DescriptorJson.has_value());
		if (!DescriptorJson) return;
		PF_CHECK(Tests, WriteValidTestExecutable(TemplateRoot / std::filesystem::u8path(Descriptor.Entrypoint)));
		PF_CHECK(Tests, WriteTextFile(TemplateRoot / "Shaders" / "Tonemapping.ps.spv", "shader"));
		PF_CHECK(Tests, WriteTextFile(TemplateRoot / "Contents" / "Resources" / "Shaders" /
			"Tonemapping.ps.spv", "shader"));
		const auto DescriptorPath = Fixture.Root() / "template.json";
		PF_CHECK(Tests, WriteTextFile(DescriptorPath, *DescriptorJson));
		const auto Output = Fixture.Root() / "Package";

		AssetExportRequest Request;
		Request.SourceProjectFile = ProjectFile;
		Request.Profile = MakeHostProfile();
		Request.Profile.StartupScene = StartupScene;
		Request.TemplateRoot = TemplateRoot;
		Request.TemplateDescriptor = DescriptorPath;
		Request.OutputOverride = Output;
		AssetExportService Service;
		auto Started = Service.Start(Request);
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Published = WaitForTerminal(Service, *Started);
		if (Published.Error)
			std::cerr << "Real export fixture failed: " << Published.Error->Message << " at " << Published.Error->Path << '\n';
		PF_CHECK(Tests, Published.Phase == AssetExportPhase::Complete);
		PF_CHECK(Tests, std::filesystem::exists(Output / ".pf-export-inventory.json"));
		Service.Shutdown();
		if (Published.Phase != AssetExportPhase::Complete) return;

#if defined(_WIN32)
		const HANDLE HeldPackageFile = CreateFileW((Output / ".pf-export-inventory.json").c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		PF_CHECK(Tests, HeldPackageFile != INVALID_HANDLE_VALUE);
		if (HeldPackageFile == INVALID_HANDLE_VALUE) return;
		AssetExportService RecoveryService;
		auto RecoveryRequestID = RecoveryService.Start(Request);
		PF_CHECK(Tests, RecoveryRequestID.has_value());
		if (!RecoveryRequestID) { CloseHandle(HeldPackageFile); return; }
		const AssetExportSnapshot Interrupted = WaitForTerminal(RecoveryService, *RecoveryRequestID);
		PF_CHECK(Tests, Interrupted.Phase == AssetExportPhase::Failed);
		if (Interrupted.Error)
			std::cerr << "Locked-package export returned " << static_cast<int>(Interrupted.Error->Code)
				<< ": " << Interrupted.Error->Message << '\n';
		const bool IsRecoveryRequired = Interrupted.Error.has_value() &&
			static_cast<uint8_t>(Interrupted.Error->Code) == static_cast<uint8_t>(AssetExportErrorCode::RecoveryRequired);
		PF_CHECK(Tests, IsRecoveryRequired);
		CloseHandle(HeldPackageFile);
		RecoveryService.Shutdown();
		const auto Recovered = AssetExportService::RecoverInterruptedPublication(Output);
		PF_CHECK(Tests, Recovered.has_value());
		PF_CHECK(Tests, std::filesystem::exists(Output / ".pf-export-inventory.json"));
#endif

		const auto Sentinel = Output / "unrelated sentinel.txt";
		PF_CHECK(Tests, WriteTextFile(Sentinel, "owned by the user"));
		const auto InventoryPath = Output / ".pf-export-inventory.json";
		std::ifstream InventoryInput(InventoryPath, std::ios::binary);
		const std::string InventoryText((std::istreambuf_iterator<char>(InventoryInput)), std::istreambuf_iterator<char>());
		Json Inventory = Json::parse(InventoryText);
		Inventory["files"]["unrelated sentinel.txt"] = DigestText("owned by the user");
		PF_CHECK(Tests, WriteTextFile(InventoryPath, Inventory.dump(2) + "\n"));
		std::ifstream TamperedInput(InventoryPath, std::ios::binary);
		const std::string TamperedInventory((std::istreambuf_iterator<char>(TamperedInput)), std::istreambuf_iterator<char>());

		AssetExportService RetryService;
		Started = RetryService.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Rejected = WaitForTerminal(RetryService, *Started);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::PublicationFailed);
		std::ifstream PreservedInventoryInput(InventoryPath, std::ios::binary);
		const std::string PreservedInventory((std::istreambuf_iterator<char>(PreservedInventoryInput)), std::istreambuf_iterator<char>());
		std::ifstream SentinelInput(Sentinel, std::ios::binary);
		const std::string PreservedSentinel((std::istreambuf_iterator<char>(SentinelInput)), std::istreambuf_iterator<char>());
		PF_CHECK(Tests, PreservedInventory == TamperedInventory);
		PF_CHECK(Tests, PreservedSentinel == "owned by the user");
		RetryService.Shutdown();
	}
} // namespace

void RunAssetExportServiceTests(TestRunner& Tests)
{
	TestTemplateDescriptorRoundTripAndStrictness(Tests);
	TestMalformedAndMissingTemplateDescriptor(Tests);
	TestTemplateMismatchDoesNotTouchOutput(Tests);
	TestMissingTemplateDoesNotTouchOutput(Tests);
	TestNativeEntrypointValidationDoesNotTouchOutput(Tests);
	TestPosixEntrypointRequiresExecutablePermission(Tests);
	TestInterruptedPublicationRestoresLastGoodPackage(Tests);
	TestInterruptedStagingRecoveryIsExplicitAndSafe(Tests);
	TestInterruptedPublicationPreservesChangedRecoveryData(Tests);
	TestPackageInventoryCannotClaimUnrelatedFiles(Tests);
}
