#include "TestSupport.h"

#include "Assets/AssetExportService.h"
#include "Assets/AssetMetadata.h"
#include "Assets/Project.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
			if (!Error)
			{
				m_Root = std::filesystem::canonical(m_Root, Error);
			}
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

	bool ReadTextFile(const std::filesystem::path& Path, std::string& Content)
	{
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) return false;
		Content.assign(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>());
		return !Input.bad();
	}

	bool WriteTemplateBundle(const std::filesystem::path& TemplateRoot, const ExportTemplateDescriptor& Descriptor)
	{
		for (const std::string& Relative : Descriptor.RuntimeFiles)
		{
			const std::filesystem::path Destination = TemplateRoot / std::filesystem::u8path(Relative);
			if (Relative == Descriptor.Entrypoint)
			{
				if (!WriteValidTestExecutable(Destination)) return false;
			}
			else if (!WriteTextFile(Destination, "template file: " + Relative)) return false;
		}
		return true;
	}

	bool WriteTemplateDescriptor(const std::filesystem::path& DescriptorPath, const ExportTemplateDescriptor& Descriptor)
	{
		const auto Serialized = SerializeExportTemplateDescriptor(Descriptor);
		return Serialized && WriteTextFile(DescriptorPath, *Serialized);
	}

	bool WriteTemplateDescriptorWithRawRuntimeFiles(const std::filesystem::path& DescriptorPath,
		const ExportTemplateDescriptor& ValidDescriptor, const std::vector<std::string>& RuntimeFiles)
	{
		const auto Serialized = SerializeExportTemplateDescriptor(ValidDescriptor);
		if (!Serialized) return false;
		nlohmann::json Document = nlohmann::json::parse(*Serialized);
		Document["runtimeFiles"] = RuntimeFiles;
		return WriteTextFile(DescriptorPath, Document.dump(2) + "\n");
	}

	struct RealExportCase final
	{
		ExportTemplateDescriptor Descriptor = MakeHostDescriptor();
		AssetExportRequest Request;
		std::filesystem::path TemplateRoot;
		std::filesystem::path DescriptorPath;
		std::filesystem::path Output;
	};

	bool PrepareRealExportCase(const std::filesystem::path& Root, RealExportCase& ExportCase)
	{
		std::filesystem::path ProjectFile;
		AssetID StartupScene;
		if (!PrepareExportProject(Root, ProjectFile, StartupScene)) return false;

		ExportCase.TemplateRoot = Root / "Template";
		ExportCase.DescriptorPath = Root / "template.json";
		ExportCase.Output = Root / "Package";
		if (!WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor) ||
			!WriteTemplateDescriptor(ExportCase.DescriptorPath, ExportCase.Descriptor)) return false;

		ExportCase.Request.SourceProjectFile = ProjectFile;
		ExportCase.Request.Profile = MakeHostProfile();
		ExportCase.Request.Profile.StartupScene = StartupScene;
		ExportCase.Request.TemplateRoot = ExportCase.TemplateRoot;
		ExportCase.Request.TemplateDescriptor = ExportCase.DescriptorPath;
		ExportCase.Request.OutputOverride = ExportCase.Output;
		return true;
	}

	bool CapturePackageTree(const std::filesystem::path& Root, std::map<std::string, std::string>& Tree)
	{
		std::error_code Error;
		std::filesystem::recursive_directory_iterator Current(Root, Error);
		if (Error) return false;
		for (const std::filesystem::recursive_directory_iterator End; Current != End; Current.increment(Error))
		{
			if (Error) return false;
			const std::filesystem::file_status Status = Current->symlink_status(Error);
			if (Error) return false;
			if (std::filesystem::is_directory(Status)) continue;
			if (!std::filesystem::is_regular_file(Status)) return false;

			std::ifstream Input(Current->path(), std::ios::binary);
			if (!Input) return false;
			const std::string Bytes((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
			if (Input.bad()) return false;
			Tree[PathUtf8(Current->path().lexically_relative(Root))] = DigestText(Bytes);
		}
		return !Error;
	}

	AssetExportSnapshot WaitForTerminal(AssetExportService& Service, uint64_t RequestID,
		std::string_view Context = "asset export request");

	AssetExportSnapshot RunExportRequest(TestRunner& Tests, AssetExportRequest Request)
	{
		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return {};
		const AssetExportSnapshot Snapshot = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, !Snapshot.Running);
		Service.Shutdown();
		return Snapshot;
	}

	std::string RuntimeConfigurationRelativePath()
	{
		return HostPlatform() == ExportTargetPlatform::MacOS
			? "Contents/Resources/PulseForgeRuntime.cfg"
			: "PulseForgeRuntime.cfg";
	}

	bool HasCaseInsensitivePathSemantics(const std::filesystem::path& Root)
	{
		const auto MixedCase = Root / "CaseSemanticsProbe";
		const auto LowerCase = Root / "casesemanticsprobe";
		if (!WriteTextFile(MixedCase, "probe")) return false;
		std::error_code Error;
		const bool IsCaseInsensitive = std::filesystem::exists(LowerCase, Error) && !Error;
		std::filesystem::remove(MixedCase, Error);
		return IsCaseInsensitive;
	}

	class ExportTestGate final
	{
	public:
		~ExportTestGate() { Release(); }

		void Pause()
		{
			std::unique_lock Lock(m_Mutex);
			m_Entered = true;
			m_Condition.notify_all();
			m_Condition.wait(Lock, [this] { return m_Released; });
		}

		[[nodiscard]] bool WaitUntilEntered(std::chrono::milliseconds Timeout)
		{
			std::unique_lock Lock(m_Mutex);
			return m_Condition.wait_for(Lock, Timeout, [this] { return m_Entered; });
		}

		void Release() noexcept
		{
			{
				std::lock_guard Lock(m_Mutex);
				m_Released = true;
			}
			m_Condition.notify_all();
		}

	private:
		std::mutex m_Mutex;
		std::condition_variable m_Condition;
		bool m_Entered = false;
		bool m_Released = false;
	};

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

	AssetExportSnapshot WaitForTerminal(AssetExportService& Service, uint64_t RequestID, std::string_view Context)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		AssetExportSnapshot Snapshot = Service.GetSnapshot(RequestID);
		while (Snapshot.Running && std::chrono::steady_clock::now() < Deadline)
		{
			std::this_thread::yield();
			Snapshot = Service.GetSnapshot(RequestID);
		}
		if (Snapshot.Running)
			std::cerr << "ASSET_EXPORT_WAIT_TIMEOUT context=" << Context << " request=" << RequestID
				<< " phase=" << static_cast<unsigned>(Snapshot.Phase) << " progress=" << Snapshot.Progress
				<< " cancellation_requested=" << Snapshot.CancellationRequested << std::endl;
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
		PF_CHECK(Tests, WriteTemplateBundle(Fixture.Root(), WrongTarget));
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
		if (Snapshot.Error)
			std::cerr << "Wrong-target export returned " << static_cast<int>(Snapshot.Error->Code)
				<< ": " << Snapshot.Error->Message << '\n';
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
		std::error_code DirectoryError;
		std::filesystem::create_directories(Entrypoint.parent_path(), DirectoryError);
		PF_CHECK(Tests, !DirectoryError);
		if (DirectoryError) return;
		{
			std::ofstream Output(Entrypoint, std::ios::binary | std::ios::trunc);
			PF_CHECK(Tests, static_cast<bool>(Output));
			Output.write(InvalidExecutableHeader.data(), static_cast<std::streamsize>(InvalidExecutableHeader.size()));
			Output.flush();
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
#if !defined(_WIN32)
		std::error_code PermissionError;
		std::filesystem::permissions(Entrypoint, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
			std::filesystem::perms::owner_exec, std::filesystem::perm_options::replace, PermissionError);
		PF_CHECK(Tests, !PermissionError);
#endif

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
		if (!Recovered)
		{
			std::cerr << "Recovery test reported: " << Recovered.error().Message << " at ";
			TestSupport::WritePathUtf8(std::cerr, Recovered.error().Path);
			std::cerr << '\n';
		}
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
		std::ostringstream PathDiagnostic;
		TestSupport::WritePathUtf8(PathDiagnostic, Fixture.Root());
		PF_CHECK(Tests, PathDiagnostic.str().find("\xCE\xA9") != std::string::npos);
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
		const AssetExportSnapshot Published = WaitForTerminal(Service, *Started, "package inventory initial export");
		if (Published.Error)
		{
			std::cerr << "Real export fixture failed (code " << static_cast<int>(Published.Error->Code) << "): "
				<< Published.Error->Message << " at ";
			TestSupport::WritePathUtf8(std::cerr, Published.Error->Path);
			std::cerr << '\n';
		}
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
		const AssetExportSnapshot Interrupted = WaitForTerminal(RecoveryService, *RecoveryRequestID, "locked package recovery export");
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
		InventoryInput.close();
		Json Inventory = Json::parse(InventoryText);
		PF_CHECK(Tests, Inventory.is_object());
		PF_CHECK(Tests, Inventory.value("format", std::string{}) == "PulseForgeExportInventory");
		PF_CHECK(Tests, Inventory.value("version", 0u) == 3u);
		PF_CHECK(Tests, Inventory.value("profileId", std::string{}) == Request.Profile.ProfileID.ToString());
		PF_CHECK(Tests, !Inventory.value("cookGeneration", std::string{}).empty());
		PF_CHECK(Tests, Inventory.contains("files") && Inventory["files"].is_object());
		const auto OwnershipPath = Output / ".pf-export-template-ownership.json";
		std::string OwnershipText;
		PF_CHECK(Tests, ReadTextFile(OwnershipPath, OwnershipText));
		const Json Ownership = Json::parse(OwnershipText);
		PF_CHECK(Tests, Ownership.value("format", std::string{}) == "PulseForgeTemplateOwnership");
		PF_CHECK(Tests, Ownership.value("version", 0u) == 1u);
		PF_CHECK(Tests, Ownership.value("entrypoint", std::string{}) == Descriptor.Entrypoint);
		PF_CHECK(Tests, Ownership.value("membershipDigest", std::string{}).size() == 32);
		const Json OriginalInventory = Inventory;
		Inventory["files"]["unrelated sentinel.txt"] = Json{
			{ "role", "template" }, { "digest", DigestText("owned by the user") }
		};
		PF_CHECK(Tests, Inventory["files"]["unrelated sentinel.txt"].value("digest", std::string{}) ==
			DigestText("owned by the user"));
		PF_CHECK(Tests, WriteTextFile(InventoryPath, Inventory.dump(2) + "\n"));
		std::ifstream TamperedInput(InventoryPath, std::ios::binary);
		const std::string TamperedInventory((std::istreambuf_iterator<char>(TamperedInput)), std::istreambuf_iterator<char>());
		TamperedInput.close();

		AssetExportService RetryService;
		Started = RetryService.Start(Request);
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot Rejected = WaitForTerminal(RetryService, *Started);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::PublicationFailed);
		std::ifstream PreservedInventoryInput(InventoryPath, std::ios::binary);
		const std::string PreservedInventory((std::istreambuf_iterator<char>(PreservedInventoryInput)), std::istreambuf_iterator<char>());
		PreservedInventoryInput.close();
		std::ifstream SentinelInput(Sentinel, std::ios::binary);
		const std::string PreservedSentinel((std::istreambuf_iterator<char>(SentinelInput)), std::istreambuf_iterator<char>());
		SentinelInput.close();
		PF_CHECK(Tests, PreservedInventory == TamperedInventory);
		PF_CHECK(Tests, PreservedSentinel == "owned by the user");
		RetryService.Shutdown();

		Json UnsupportedVersion = Inventory;
		UnsupportedVersion["version"] = 1;
		const std::string UnsupportedVersionInventory = UnsupportedVersion.dump(2) + "\n";
		PF_CHECK(Tests, WriteTextFile(InventoryPath, UnsupportedVersionInventory));
		AssetExportService VersionRetryService;
		Started = VersionRetryService.Start(Request);
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot VersionRejected = WaitForTerminal(VersionRetryService, *Started);
		PF_CHECK(Tests, VersionRejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, VersionRejected.Error && VersionRejected.Error->Code == AssetExportErrorCode::PublicationFailed);
		std::string PreservedVersionInventory;
		PF_CHECK(Tests, ReadTextFile(InventoryPath, PreservedVersionInventory));
		PF_CHECK(Tests, PreservedVersionInventory == UnsupportedVersionInventory);
		VersionRetryService.Shutdown();

		Json LegacyVersionTwo = OriginalInventory;
		LegacyVersionTwo["version"] = 2;
		LegacyVersionTwo.erase("resourcePrefix");
		LegacyVersionTwo.erase("templateFiles");
		LegacyVersionTwo.erase("templateEntrypoint");
		LegacyVersionTwo.erase("templateMembershipDigest");
		Json LegacyReceipts = Json::object();
		for (const auto& [Name, Receipt] : OriginalInventory["files"].items())
			if (Name != ".pf-export-template-ownership.json") LegacyReceipts[Name] = Receipt.at("digest");
		LegacyVersionTwo["files"] = std::move(LegacyReceipts);
		const std::string LegacyVersionTwoText = LegacyVersionTwo.dump(2) + "\n";
		PF_CHECK(Tests, WriteTextFile(InventoryPath, LegacyVersionTwoText));
		std::error_code RemoveOwnershipError;
		std::filesystem::remove(OwnershipPath, RemoveOwnershipError);
		PF_CHECK(Tests, !RemoveOwnershipError);
		AssetExportService LegacyUpgradeService;
		Started = LegacyUpgradeService.Start(Request);
		PF_CHECK(Tests, Started.has_value());
		if (!Started) return;
		const AssetExportSnapshot LegacyUpgraded = WaitForTerminal(LegacyUpgradeService, *Started);
		if (LegacyUpgraded.Error)
		{
			std::cerr << "Legacy inventory upgrade returned " << static_cast<int>(LegacyUpgraded.Error->Code)
				<< ": " << LegacyUpgraded.Error->Message << " at ";
			TestSupport::WritePathUtf8(std::cerr, LegacyUpgraded.Error->Path);
			std::cerr << '\n';
		}
		PF_CHECK(Tests, LegacyUpgraded.Phase == AssetExportPhase::Complete);
		std::string UpgradedInventoryText;
		PF_CHECK(Tests, ReadTextFile(InventoryPath, UpgradedInventoryText));
		const Json UpgradedInventory = Json::parse(UpgradedInventoryText);
		PF_CHECK(Tests, UpgradedInventory.value("version", 0u) == 3u);
		std::string PreservedLegacySentinel;
		PF_CHECK(Tests, ReadTextFile(Sentinel, PreservedLegacySentinel));
		PF_CHECK(Tests, PreservedLegacySentinel == "owned by the user");
		LegacyUpgradeService.Shutdown();
	}

	void TestCookFailureCleansOwnedStagingRoot(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;
		std::map<std::string, std::string> LastGoodPackage;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, LastGoodPackage));

		std::filesystem::path Candidate;
		bool StageRootPrepared = false;
		bool SourceChanged = false;
		const std::filesystem::path SceneSource = ExportCase.Request.SourceProjectFile.parent_path() / "Assets" / "Startup.scene";
		ExportCase.Request.BeforeCandidateCreate = [&Candidate](const std::filesystem::path& Path) { Candidate = Path; };
		ExportCase.Request.BeforeCook = [&]
		{
			if (Candidate.empty()) return;
			std::error_code Error;
			const std::filesystem::path CookRoot = Candidate / ".pf-cook";
			std::filesystem::create_directories(CookRoot, Error);
			StageRootPrepared = !Error && std::filesystem::is_directory(CookRoot);
			std::string SceneText;
			SourceChanged = StageRootPrepared && ReadTextFile(SceneSource, SceneText) &&
				WriteTextFile(SceneSource, SceneText + "\n");
		};

		const AssetExportSnapshot Rejected = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, StageRootPrepared);
		PF_CHECK(Tests, SourceChanged);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::CookFailed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Path == SceneSource);
		std::map<std::string, std::string> AfterFailure;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, AfterFailure));
		PF_CHECK(Tests, AfterFailure == LastGoodPackage);
		PF_CHECK(Tests, !Candidate.empty() && !std::filesystem::exists(Candidate));
		PF_CHECK(Tests, !std::filesystem::exists(Fixture.Root() / ".Package.pf-export-recovery.json"));
	}

	void TestExportTemplateRenameRemovalAndRepeatedExport(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;

		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const std::string OldRuntimeFile = "Runtime/Old.dat";
		const std::string RemovedRuntimeFile = "Runtime/Removed.dat";
		const std::string RenamedRuntimeFile = "Runtime/New.dat";
		ExportCase.Descriptor.RuntimeFiles.push_back(OldRuntimeFile);
		ExportCase.Descriptor.RuntimeFiles.push_back(RemovedRuntimeFile);
		PF_CHECK(Tests, WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor));
		PF_CHECK(Tests, WriteTemplateDescriptor(ExportCase.DescriptorPath, ExportCase.Descriptor));

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;

		const auto Sentinel = ExportCase.Output / "User" / "sentinel.txt";
		PF_CHECK(Tests, WriteTextFile(Sentinel, "user-owned package data"));
		const auto OldOutputFile = ExportCase.Output / std::filesystem::u8path(OldRuntimeFile);
		const auto RemovedOutputFile = ExportCase.Output / std::filesystem::u8path(RemovedRuntimeFile);
		const auto RenamedOutputFile = ExportCase.Output / std::filesystem::u8path(RenamedRuntimeFile);
		PF_CHECK(Tests, std::filesystem::exists(OldOutputFile));
		PF_CHECK(Tests, std::filesystem::exists(RemovedOutputFile));

		auto OldRuntime = std::find(ExportCase.Descriptor.RuntimeFiles.begin(), ExportCase.Descriptor.RuntimeFiles.end(), OldRuntimeFile);
		PF_CHECK(Tests, OldRuntime != ExportCase.Descriptor.RuntimeFiles.end());
		if (OldRuntime == ExportCase.Descriptor.RuntimeFiles.end()) return;
		*OldRuntime = RenamedRuntimeFile;
		std::erase(ExportCase.Descriptor.RuntimeFiles, RemovedRuntimeFile);
		PF_CHECK(Tests, WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor));
		PF_CHECK(Tests, WriteTemplateDescriptor(ExportCase.DescriptorPath, ExportCase.Descriptor));

		const AssetExportSnapshot Updated = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Updated.Phase == AssetExportPhase::Complete);
		if (Updated.Phase != AssetExportPhase::Complete) return;
		PF_CHECK(Tests, !std::filesystem::exists(OldOutputFile));
		PF_CHECK(Tests, !std::filesystem::exists(RemovedOutputFile));
		PF_CHECK(Tests, std::filesystem::exists(RenamedOutputFile));
		std::string RuntimeText;
		PF_CHECK(Tests, ReadTextFile(RenamedOutputFile, RuntimeText));
		PF_CHECK(Tests, RuntimeText == "template file: " + RenamedRuntimeFile);
		std::string SentinelText;
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "user-owned package data");

		const AssetExportSnapshot Repeated = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Repeated.Phase == AssetExportPhase::Complete);
		PF_CHECK(Tests, std::filesystem::exists(RenamedOutputFile));
		PF_CHECK(Tests, !std::filesystem::exists(OldOutputFile));
		PF_CHECK(Tests, !std::filesystem::exists(RemovedOutputFile));
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "user-owned package data");

		std::string InventoryText;
		PF_CHECK(Tests, ReadTextFile(ExportCase.Output / ".pf-export-inventory.json", InventoryText));
		const nlohmann::json Inventory = nlohmann::json::parse(InventoryText);
		PF_CHECK(Tests, Inventory.value("version", 0u) == 3u);
		std::string OwnershipText;
		PF_CHECK(Tests, ReadTextFile(ExportCase.Output / ".pf-export-template-ownership.json", OwnershipText));
		const nlohmann::json Ownership = nlohmann::json::parse(OwnershipText);
		PF_CHECK(Tests, Ownership["templateFiles"].is_array());
		PF_CHECK(Tests, std::find(Ownership["templateFiles"].begin(), Ownership["templateFiles"].end(), RenamedRuntimeFile) !=
			Ownership["templateFiles"].end());
		PF_CHECK(Tests, std::find(Ownership["templateFiles"].begin(), Ownership["templateFiles"].end(), OldRuntimeFile) ==
			Ownership["templateFiles"].end());
		PF_CHECK(Tests, std::find(Ownership["templateFiles"].begin(), Ownership["templateFiles"].end(), RemovedRuntimeFile) ==
			Ownership["templateFiles"].end());
		PF_CHECK(Tests, Inventory["files"].contains(RenamedRuntimeFile));
		PF_CHECK(Tests, Inventory["files"][RenamedRuntimeFile].value("role", std::string{}) == "template");
		PF_CHECK(Tests, Inventory["files"][RenamedRuntimeFile].value("digest", std::string{}) ==
			DigestText("template file: " + RenamedRuntimeFile));
		PF_CHECK(Tests, !Inventory["files"].contains(OldRuntimeFile));
		PF_CHECK(Tests, !Inventory["files"].contains(RemovedRuntimeFile));
		PF_CHECK(Tests, !Inventory["files"].contains("User/sentinel.txt"));
	}

	void TestExportPreservesModifiedPreviouslyOwnedFile(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;

		const auto Sentinel = ExportCase.Output / "User" / "sentinel.txt";
		PF_CHECK(Tests, WriteTextFile(Sentinel, "keep this unowned file"));
		const std::string OwnedRuntimePath = ExportCase.Descriptor.RuntimeFiles.back();
		const auto ModifiedOwnedFile = ExportCase.Output / std::filesystem::u8path(OwnedRuntimePath);
		PF_CHECK(Tests, WriteTextFile(ModifiedOwnedFile, "user-modified owned runtime file"));
		std::map<std::string, std::string> BeforeRetry;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, BeforeRetry));

		const AssetExportSnapshot Rejected = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::PublicationFailed);
		std::map<std::string, std::string> AfterRetry;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, AfterRetry));
		PF_CHECK(Tests, AfterRetry == BeforeRetry);
		std::string ModifiedText;
		PF_CHECK(Tests, ReadTextFile(ModifiedOwnedFile, ModifiedText));
		PF_CHECK(Tests, ModifiedText == "user-modified owned runtime file");
		std::string SentinelText;
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "keep this unowned file");
	}

	void TestExportPreservesUnownedTemplateCollisionAndAllowsRetry(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;

		const std::string CollidingPath = "User/PlayerData.dat";
		const auto Sentinel = ExportCase.Output / std::filesystem::u8path(CollidingPath);
		PF_CHECK(Tests, WriteTextFile(Sentinel, "unowned bytes must survive"));
		std::map<std::string, std::string> BeforeCollision;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, BeforeCollision));

		ExportCase.Descriptor.RuntimeFiles.push_back(CollidingPath);
		PF_CHECK(Tests, WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor));
		PF_CHECK(Tests, WriteTemplateDescriptor(ExportCase.DescriptorPath, ExportCase.Descriptor));
		const AssetExportSnapshot Collision = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Collision.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Collision.Error && Collision.Error->Code == AssetExportErrorCode::PublicationFailed);
		PF_CHECK(Tests, Collision.Error && Collision.Error->Message.find("unowned package file collides") != std::string::npos);
		std::map<std::string, std::string> AfterCollision;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, AfterCollision));
		PF_CHECK(Tests, AfterCollision == BeforeCollision);
		std::string SentinelText;
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "unowned bytes must survive");

		std::error_code Error;
		std::filesystem::remove(Sentinel, Error);
		PF_CHECK(Tests, !Error);
		const AssetExportSnapshot Retry = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Retry.Phase == AssetExportPhase::Complete);
		std::string TemplateText;
		PF_CHECK(Tests, ReadTextFile(ExportCase.Output / std::filesystem::u8path(CollidingPath), TemplateText));
		PF_CHECK(Tests, TemplateText == "template file: " + CollidingPath);
	}

	void TestExportDoesNotAdoptAnOccupiedCandidatePath(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;
		std::map<std::string, std::string> PublishedBefore;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, PublishedBefore));

		bool CollisionCreated = false;
		std::filesystem::path OccupiedCandidate;
		AssetExportRequest Request = ExportCase.Request;
		Request.BeforeCandidateCreate = [&CollisionCreated, &OccupiedCandidate](const std::filesystem::path& Candidate)
		{
			OccupiedCandidate = Candidate;
			std::error_code Error;
			CollisionCreated = std::filesystem::create_directory(Candidate, Error) && !Error;
			if (CollisionCreated)
				CollisionCreated = WriteTextFile(Candidate / "foreign.txt", "foreign candidate data");
		};
		const AssetExportSnapshot Rejected = RunExportRequest(Tests, std::move(Request));
		PF_CHECK(Tests, CollisionCreated);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::RecoveryRequired);
		std::string ForeignText;
		PF_CHECK(Tests, ReadTextFile(OccupiedCandidate / "foreign.txt", ForeignText));
		PF_CHECK(Tests, ForeignText == "foreign candidate data");
		std::map<std::string, std::string> PublishedAfter;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, PublishedAfter));
		PF_CHECK(Tests, PublishedAfter == PublishedBefore);
	}

	void TestCandidateCleanupPreservesUnownedChangesAndRecoveryRecord(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;
		std::map<std::string, std::string> PublishedBefore;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, PublishedBefore));

		const std::string ModifiedRelative = ExportCase.Descriptor.RuntimeFiles.back();
		std::filesystem::path CandidatePath;
		bool ModifiedCandidate = false;
		bool AddedCandidateEntry = false;
		ExportTestGate Gate;
		AssetExportRequest Request = ExportCase.Request;
		Request.BeforeDependencyFinalValidation = [&Gate] { Gate.Pause(); };
		Request.BeforeCandidateCleanup = [&](const std::filesystem::path& Candidate)
		{
			CandidatePath = Candidate;
			ModifiedCandidate = WriteTextFile(Candidate / std::filesystem::u8path(ModifiedRelative), "independently changed runtime");
			AddedCandidateEntry = WriteTextFile(Candidate / "foreign-after-stage.txt", "unowned candidate bytes");
		};

		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started)
		{
			Gate.Release();
			Service.Shutdown();
			return;
		}
		const bool ReachedGate = Gate.WaitUntilEntered(std::chrono::seconds(10));
		PF_CHECK(Tests, ReachedGate);
		if (!ReachedGate)
		{
			Gate.Release();
			(void)WaitForTerminal(Service, *Started);
			Service.Shutdown();
			return;
		}
		PF_CHECK(Tests, Service.RequestCancel(*Started));
		Gate.Release();
		const AssetExportSnapshot Rejected = WaitForTerminal(Service, *Started);
		Service.Shutdown();

		PF_CHECK(Tests, ModifiedCandidate);
		PF_CHECK(Tests, AddedCandidateEntry);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::RecoveryRequired);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Message.find("preserving the candidate") != std::string::npos);
		std::string ModifiedText;
		PF_CHECK(Tests, ReadTextFile(CandidatePath / std::filesystem::u8path(ModifiedRelative), ModifiedText));
		PF_CHECK(Tests, ModifiedText == "independently changed runtime");
		std::string ForeignText;
		PF_CHECK(Tests, ReadTextFile(CandidatePath / "foreign-after-stage.txt", ForeignText));
		PF_CHECK(Tests, ForeignText == "unowned candidate bytes");
		std::map<std::string, std::string> PublishedAfter;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, PublishedAfter));
		PF_CHECK(Tests, PublishedAfter == PublishedBefore);

		const auto OutputName = ExportCase.Output.filename().u8string();
		const std::string Utf8OutputName(reinterpret_cast<const char*>(OutputName.data()), OutputName.size());
		const auto Recovery = ExportCase.Output.parent_path() /
			std::filesystem::u8path("." + Utf8OutputName + ".pf-export-recovery.json");
		std::string RecoveryBefore;
		PF_CHECK(Tests, ReadTextFile(Recovery, RecoveryBefore));
		const auto RecoveryAttempt = AssetExportService::RecoverInterruptedPublication(ExportCase.Output);
		PF_CHECK(Tests, !RecoveryAttempt);
		PF_CHECK(Tests, RecoveryAttempt.error().Code == AssetExportErrorCode::RecoveryRequired);
		std::string RecoveryAfter;
		PF_CHECK(Tests, ReadTextFile(Recovery, RecoveryAfter));
		PF_CHECK(Tests, RecoveryAfter == RecoveryBefore);
		PF_CHECK(Tests, ReadTextFile(CandidatePath / "foreign-after-stage.txt", ForeignText));
		PF_CHECK(Tests, ForeignText == "unowned candidate bytes");
	}

	void TestExportRejectsTemplateRuntimeConfigurationCollision(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const std::string RuntimeConfiguration = RuntimeConfigurationRelativePath();
		ExportCase.Descriptor.RuntimeFiles.push_back(RuntimeConfiguration);
		PF_CHECK(Tests, WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor));
		PF_CHECK(Tests, WriteTemplateDescriptorWithRawRuntimeFiles(
			ExportCase.DescriptorPath, MakeHostDescriptor(), ExportCase.Descriptor.RuntimeFiles));
		const AssetExportSnapshot Rejected = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::TemplateIncompatible);
		PF_CHECK(Tests, !std::filesystem::exists(ExportCase.Output));
	}

	void TestExportPreservesUnownedRuntimeConfigurationCollision(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;

		const std::string RuntimeConfiguration = RuntimeConfigurationRelativePath();
		const auto Sentinel = ExportCase.Output / std::filesystem::u8path(RuntimeConfiguration);
		std::string InventoryText;
		PF_CHECK(Tests, ReadTextFile(ExportCase.Output / ".pf-export-inventory.json", InventoryText));
		nlohmann::json Inventory = nlohmann::json::parse(InventoryText);
		PF_CHECK(Tests, Inventory.value("version", 0u) == 3u);
		PF_CHECK(Tests, Inventory["files"].contains(RuntimeConfiguration));
		Inventory["files"].erase(RuntimeConfiguration);
		PF_CHECK(Tests, WriteTextFile(ExportCase.Output / ".pf-export-inventory.json", Inventory.dump(2) + "\n"));
		PF_CHECK(Tests, WriteTextFile(Sentinel, "unowned runtime configuration"));
		std::map<std::string, std::string> BeforeExport;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, BeforeExport));
		const AssetExportSnapshot Rejected = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::PublicationFailed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Message.find("unowned package file collides") != std::string::npos);
		std::map<std::string, std::string> AfterExport;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, AfterExport));
		PF_CHECK(Tests, AfterExport == BeforeExport);
		std::string SentinelText;
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "unowned runtime configuration");
	}

	void TestExportRejectsCaseAliasesOnCaseInsensitiveFilesystem(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		if (!HasCaseInsensitivePathSemantics(Fixture.Root())) return;

		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;
		ExportCase.Descriptor.RuntimeFiles.push_back("CaseAlias.bin");
		ExportCase.Descriptor.RuntimeFiles.push_back("casealias.bin");
		PF_CHECK(Tests, WriteTemplateBundle(ExportCase.TemplateRoot, ExportCase.Descriptor));
		PF_CHECK(Tests, WriteTemplateDescriptorWithRawRuntimeFiles(
			ExportCase.DescriptorPath, MakeHostDescriptor(), ExportCase.Descriptor.RuntimeFiles));
		const AssetExportSnapshot Rejected = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Rejected.Phase == AssetExportPhase::Failed);
		PF_CHECK(Tests, Rejected.Error && Rejected.Error->Code == AssetExportErrorCode::TemplateIncompatible);
		PF_CHECK(Tests, !std::filesystem::exists(ExportCase.Output));
	}

	void TestHeldDependencyValidationCancellationRetainsOutputAndRetries(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;

		const auto Sentinel = ExportCase.Output / "User" / "sentinel.txt";
		PF_CHECK(Tests, WriteTextFile(Sentinel, "old package remains usable"));
		std::map<std::string, std::string> BeforeCancellation;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, BeforeCancellation));

		ExportTestGate Gate;
		size_t CookStartCount = 0;
		AssetExportRequest CancellationRequest = ExportCase.Request;
		CancellationRequest.BeforeDependencyFinalValidation = [&Gate] { Gate.Pause(); };
		CancellationRequest.BeforeCook = [&CookStartCount] { ++CookStartCount; };
		AssetExportService Service;
		const auto Started = Service.Start(std::move(CancellationRequest));
		PF_CHECK(Tests, Started.has_value());
		if (!Started)
		{
			Gate.Release();
			Service.Shutdown();
			return;
		}

		const bool ReachedValidationBoundary = Gate.WaitUntilEntered(std::chrono::seconds(10));
		PF_CHECK(Tests, ReachedValidationBoundary);
		if (!ReachedValidationBoundary)
		{
			Gate.Release();
			(void)WaitForTerminal(Service, *Started);
			Service.Shutdown();
			return;
		}

		const AssetExportSnapshot Held = Service.GetSnapshot(*Started);
		PF_CHECK(Tests, Held.Running);
		PF_CHECK(Tests, Held.Phase == AssetExportPhase::Cook);
		const bool CancellationRequested = Service.RequestCancel(*Started);
		PF_CHECK(Tests, CancellationRequested);
		PF_CHECK(Tests, Service.GetSnapshot(*Started).CancellationRequested);
		Gate.Release();

		const AssetExportSnapshot Cancelled = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, Cancelled.Phase == AssetExportPhase::Cancelled);
		PF_CHECK(Tests, Cancelled.Error && Cancelled.Error->Code == AssetExportErrorCode::Cancelled);
		PF_CHECK(Tests, CookStartCount == 0);
		std::map<std::string, std::string> AfterCancellation;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, AfterCancellation));
		PF_CHECK(Tests, AfterCancellation == BeforeCancellation);
		PF_CHECK(Tests, !Service.RequestCancel(*Started));
		Service.Shutdown();

		const AssetExportSnapshot Retry = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, Retry.Phase == AssetExportPhase::Complete);
		std::string SentinelText;
		PF_CHECK(Tests, ReadTextFile(Sentinel, SentinelText));
		PF_CHECK(Tests, SentinelText == "old package remains usable");
	}

	void TestCancellationClosesAtPublicationBoundary(TestRunner& Tests)
	{
		TemporaryExportFixture Fixture;
		PF_CHECK(Tests, Fixture.IsReady());
		if (!Fixture.IsReady()) return;
		RealExportCase ExportCase;
		PF_CHECK(Tests, PrepareRealExportCase(Fixture.Root(), ExportCase));
		if (ExportCase.Request.SourceProjectFile.empty()) return;

		const AssetExportSnapshot First = RunExportRequest(Tests, ExportCase.Request);
		PF_CHECK(Tests, First.Phase == AssetExportPhase::Complete);
		if (First.Phase != AssetExportPhase::Complete) return;
		std::map<std::string, std::string> BeforePublication;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, BeforePublication));

		ExportTestGate Gate;
		AssetExportRequest Request = ExportCase.Request;
		Request.BeforePublicationRename = [&Gate] { Gate.Pause(); };
		AssetExportService Service;
		const auto Started = Service.Start(std::move(Request));
		PF_CHECK(Tests, Started.has_value());
		if (!Started)
		{
			Gate.Release();
			Service.Shutdown();
			return;
		}

		const bool ReachedBoundary = Gate.WaitUntilEntered(std::chrono::seconds(10));
		PF_CHECK(Tests, ReachedBoundary);
		if (!ReachedBoundary)
		{
			Gate.Release();
			(void)WaitForTerminal(Service, *Started);
			Service.Shutdown();
			return;
		}
		const AssetExportSnapshot Held = Service.GetSnapshot(*Started);
		PF_CHECK(Tests, Held.Running && Held.Phase == AssetExportPhase::Publish);
		PF_CHECK(Tests, !Held.CancellationRequested);
		PF_CHECK(Tests, !Service.RequestCancel(*Started));
		std::map<std::string, std::string> StillPublished;
		PF_CHECK(Tests, CapturePackageTree(ExportCase.Output, StillPublished));
		PF_CHECK(Tests, StillPublished == BeforePublication);

		Gate.Release();
		const AssetExportSnapshot Completed = WaitForTerminal(Service, *Started);
		PF_CHECK(Tests, Completed.Phase == AssetExportPhase::Complete);
		PF_CHECK(Tests, !Completed.CancellationRequested);
		PF_CHECK(Tests, std::filesystem::exists(ExportCase.Output / ".pf-export-inventory.json"));
		Service.Shutdown();
	}
} // namespace

void RunAssetExportServiceTests(TestRunner& Tests)
{
	const auto Run = [&Tests](std::string_view Name, auto&& Test)
	{
		std::cerr << "ASSET_EXPORT_TEST_BEGIN " << Name << std::endl;
		Test(Tests);
		std::cerr << "ASSET_EXPORT_TEST_END " << Name << std::endl;
	};
	Run("TemplateDescriptorRoundTripAndStrictness", TestTemplateDescriptorRoundTripAndStrictness);
	Run("MalformedAndMissingTemplateDescriptor", TestMalformedAndMissingTemplateDescriptor);
	Run("TemplateMismatchDoesNotTouchOutput", TestTemplateMismatchDoesNotTouchOutput);
	Run("MissingTemplateDoesNotTouchOutput", TestMissingTemplateDoesNotTouchOutput);
	Run("NativeEntrypointValidationDoesNotTouchOutput", TestNativeEntrypointValidationDoesNotTouchOutput);
	Run("PosixEntrypointRequiresExecutablePermission", TestPosixEntrypointRequiresExecutablePermission);
	Run("InterruptedPublicationRestoresLastGoodPackage", TestInterruptedPublicationRestoresLastGoodPackage);
	Run("InterruptedStagingRecoveryIsExplicitAndSafe", TestInterruptedStagingRecoveryIsExplicitAndSafe);
	Run("InterruptedPublicationPreservesChangedRecoveryData", TestInterruptedPublicationPreservesChangedRecoveryData);
	Run("PackageInventoryCannotClaimUnrelatedFiles", TestPackageInventoryCannotClaimUnrelatedFiles);
	Run("CookFailureCleansOwnedStagingRoot", TestCookFailureCleansOwnedStagingRoot);
	Run("ExportTemplateRenameRemovalAndRepeatedExport", TestExportTemplateRenameRemovalAndRepeatedExport);
	Run("ExportPreservesModifiedPreviouslyOwnedFile", TestExportPreservesModifiedPreviouslyOwnedFile);
	Run("ExportPreservesUnownedTemplateCollisionAndAllowsRetry", TestExportPreservesUnownedTemplateCollisionAndAllowsRetry);
	Run("ExportDoesNotAdoptAnOccupiedCandidatePath", TestExportDoesNotAdoptAnOccupiedCandidatePath);
	Run("CandidateCleanupPreservesUnownedChangesAndRecoveryRecord", TestCandidateCleanupPreservesUnownedChangesAndRecoveryRecord);
	Run("ExportRejectsTemplateRuntimeConfigurationCollision", TestExportRejectsTemplateRuntimeConfigurationCollision);
	Run("ExportPreservesUnownedRuntimeConfigurationCollision", TestExportPreservesUnownedRuntimeConfigurationCollision);
	Run("ExportRejectsCaseAliasesOnCaseInsensitiveFilesystem", TestExportRejectsCaseAliasesOnCaseInsensitiveFilesystem);
	Run("HeldDependencyValidationCancellationRetainsOutputAndRetries", TestHeldDependencyValidationCancellationRetainsOutputAndRetries);
	Run("CancellationClosesAtPublicationBoundary", TestCancellationClosesAtPublicationBoundary);
}
