#include "Assets/AssetCookService.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Project.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/AssetCookService.h"
#include "Assets/AssetDependencyCatalog.h"
#include "Assets/Detail/AssetCpuProducts.h"
#include "Assets/AssetTypes.h"
#include "Assets/MaterialAsset.h"
#include "Runtime/CookedContentProvider.h"
#include "Scene/SceneSerializer.h"
#include "TestSupport.h"

#include <filesystem>
#include <array>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <iostream>
#include <string>

namespace
{
	using namespace PulseForge;

	template<typename Operation>
	auto RunCookWithBoundary(std::string_view Name, Operation&& CookOperation)
	{
		std::cerr << "ASSET_COOK_OPERATION_BEGIN " << Name << std::endl;
		auto Result = std::forward<Operation>(CookOperation)();
		if (Result)
			std::cerr << "ASSET_COOK_OPERATION_END " << Name << " success" << std::endl;
		else
			std::cerr << "ASSET_COOK_OPERATION_END " << Name << " failure=" << Result.error().Message << std::endl;
		return Result;
	}

	struct CookFixture final
	{
		std::filesystem::path Root;
		std::filesystem::path Source;
		std::filesystem::path Output;
		std::filesystem::path ProjectFile;
		AssetID SceneAsset;
		AssetID AlternateSceneAsset;
		AssetID ScriptAsset;
		AssetID MaterialAsset;
		CookedAssetCompatibility Compatibility;

		CookFixture()
		{
			const auto ID = UUID::Generate();
			if (!ID) return;
			std::error_code Error;
			const auto SystemTemporaryRoot = std::filesystem::temp_directory_path(Error);
			if (Error) { Root.clear(); return; }
			const auto TemporaryRoot = std::filesystem::weakly_canonical(SystemTemporaryRoot, Error);
			if (Error) { Root.clear(); return; }
			Root = TemporaryRoot /
				std::filesystem::u8path("PF \xCE\xA9 " + ID->ToString());
			Source = Root / "S";
			Output = Root / "O";
			ProjectFile = Source / "P.pfproj";
			auto Project = Project::Create(ProjectFile, "Cook fixture");
			if (!Project) { Root.clear(); return; }
			std::filesystem::create_directories(Source / "Assets");
			Scene SceneData;
			auto SceneText = SceneSerializer::Serialize(SceneData);
			if (!SceneText) { Root.clear(); return; }
			const auto ScenePath = Source / "Assets" / "Startup.scene";
			WriteText(ScenePath, *SceneText);
			auto SceneMetadata = AssetMetadataSerializer::CreateForNewAsset(ScenePath);
			if (!SceneMetadata) { Root.clear(); return; }
			SceneAsset = SceneMetadata->ID;
			const auto AlternateScenePath = Source / "Assets" / "Alternate.scene";
			WriteText(AlternateScenePath, *SceneText);
			auto AlternateSceneMetadata = AssetMetadataSerializer::CreateForNewAsset(AlternateScenePath);
			if (!AlternateSceneMetadata) { Root.clear(); return; }
			AlternateSceneAsset = AlternateSceneMetadata->ID;
			const auto ScriptPath = Source / "Assets" / "Dynamic.lua";
			WriteText(ScriptPath, "function OnCreate() end\n");
			auto ScriptMetadata = AssetMetadataSerializer::CreateForNewAsset(ScriptPath);
			if (!ScriptMetadata) { Root.clear(); return; }
			ScriptAsset = ScriptMetadata->ID;
			const auto MaterialPath = Source / "Assets" / "Cooked.material";
			MaterialAssetDesc MaterialDescription;
			MaterialDescription.BaseColorFactor = { 0.25f, 0.5f, 0.75f, 1.0f };
			auto MaterialText = MaterialAssetSerializer::Serialize(MaterialDescription);
			if (!MaterialText) { Root.clear(); return; }
			WriteText(MaterialPath, *MaterialText);
			auto MaterialMetadata = AssetMetadataSerializer::CreateForNewAsset(MaterialPath);
			if (!MaterialMetadata) { Root.clear(); return; }
			MaterialAsset = MaterialMetadata->ID;
			auto OpenedProject = Project::Open(ProjectFile);
			if (!OpenedProject || !OpenedProject->SetStartScene(SceneAsset)) { Root.clear(); return; }
			GameplayActionMap ActionMap;
			GameplayActionDefinition Jump;
			Jump.Id = "Jump";
			Jump.Type = ActionValueType::Digital;
			ActionBinding JumpBinding;
			JumpBinding.Id = "Space";
			JumpBinding.Kind = ActionBindingKind::Button;
			JumpBinding.Primary = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) };
			Jump.Bindings.push_back(std::move(JumpBinding));
			ActionMap.Actions.push_back(std::move(Jump));
			auto SerializedActionMap = SerializeGameplayActionMap(ActionMap);
			if (!SerializedActionMap) { Root.clear(); return; }
			WriteText(Source / "InputActions.json", *SerializedActionMap);
#if defined(_WIN32)
			Compatibility = { "Windows", "Vulkan", "pulseforge-runtime-v1" };
#elif defined(__APPLE__)
			Compatibility = { "macOS", "Vulkan", "pulseforge-runtime-v1" };
#else
			Compatibility = { "Linux", "Vulkan", "pulseforge-runtime-v1" };
#endif
		}

		~CookFixture()
		{
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		static void WriteText(const std::filesystem::path& Path, std::string_view Text)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		}

		[[nodiscard]] std::expected<std::shared_ptr<const AssetDependencyCatalogSnapshot>, AssetDependencyCatalogError> Build() const
		{
			auto Project = Project::Open(ProjectFile);
			if (!Project) return std::unexpected(AssetDependencyCatalogError{ ProjectFile, Project.error().Message });
			return AssetDependencyCatalogBuilder::Build(Source, Project->GetDescription().ID, 1);
		}

		[[nodiscard]] AssetCookRequest Request(std::shared_ptr<const AssetDependencyCatalogSnapshot> Snapshot) const
		{
			return { std::move(Snapshot), ProjectFile, SceneAsset, {}, Compatibility, Output };
		}
	};

	void TestLinkedPrefabSampleCookAndRuntimeScene(TestRunner& Tests)
	{
#if defined(PF_PREFAB_LINKED_SAMPLE_PROJECT)
		constexpr AssetID StartupScene{ 0xd001000000004000ull, 0x8000000000000001ull };
		constexpr AssetID PrefabAsset{ 0xd002000000004000ull, 0x8000000000000001ull };
		constexpr AssetID MeshAsset{ 0xd003000000004000ull, 0x8000000000000001ull };
		constexpr AssetID SourceMaterial{ 0xd004000000004000ull, 0x8000000000000001ull };
		constexpr AssetID OverrideMaterial{ 0xd005000000004000ull, 0x8000000000000001ull };
		constexpr AssetID ScriptAsset{ 0xd006000000004000ull, 0x8000000000000001ull };
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		std::error_code Error;
		const auto TemporaryDirectory = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const std::filesystem::path TemporaryRoot = TemporaryDirectory /
			std::filesystem::u8path("PulseForge-G15-Cook-" + ID->ToString());
		struct OwnedTemporaryRoot final
		{
			std::filesystem::path Path;
			~OwnedTemporaryRoot() { std::error_code CleanupError; std::filesystem::remove_all(Path, CleanupError); }
		} Cleanup{ TemporaryRoot };
		const std::filesystem::path SourceRoot = TemporaryRoot / "Source";
		const std::filesystem::path OutputRoot = TemporaryRoot / "Cooked";
		std::filesystem::create_directories(TemporaryRoot, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		std::filesystem::copy(std::filesystem::path(PF_PREFAB_LINKED_SAMPLE_PROJECT).parent_path(), SourceRoot,
			std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		auto OpenedProject = Project::Open(SourceRoot / std::filesystem::path(PF_PREFAB_LINKED_SAMPLE_PROJECT).filename());
		PF_CHECK(Tests, OpenedProject.has_value());
		if (!OpenedProject) return;
		PF_CHECK(Tests, OpenedProject->GetDescription().StartScene == StartupScene);
		auto SourceSnapshot = AssetDependencyCatalogBuilder::Build(
			OpenedProject->GetRootPath(), OpenedProject->GetDescription().ID, 1);
		PF_CHECK(Tests, SourceSnapshot.has_value());
		if (!SourceSnapshot) return;
		AssetCookRequest Request{
			*SourceSnapshot,
			OpenedProject->GetFilePath(),
			StartupScene,
			{},
#if defined(_WIN32)
			{ "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#elif defined(__APPLE__)
			{ "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#else
			{ "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#endif
			OutputRoot
		};
		auto Plan = AssetCookService::Plan(Request);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto HasProduct = [&](AssetID Asset, CookedAssetProductKind Product)
		{
			return std::any_of(Plan->Products.begin(), Plan->Products.end(), [&](const AssetCookPlannedProduct& Candidate)
			{
				return Candidate.Asset == Asset && Candidate.Product == Product;
			});
		};
		PF_CHECK(Tests, HasProduct(PrefabAsset, CookedAssetProductKind::Prefab));
		PF_CHECK(Tests, HasProduct(MeshAsset, CookedAssetProductKind::MeshPrimitive));
		PF_CHECK(Tests, HasProduct(SourceMaterial, CookedAssetProductKind::Material));
		PF_CHECK(Tests, HasProduct(OverrideMaterial, CookedAssetProductKind::Material));
		PF_CHECK(Tests, HasProduct(ScriptAsset, CookedAssetProductKind::Script));
		auto Cooked = AssetCookService::Cook(*Plan);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		auto Provider = CookedContentProvider::Open(OutputRoot, Request.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		PF_CHECK(Tests, (*Provider)->GetStartupScene() == StartupScene);
		PF_CHECK(Tests, (*Provider)->FindPrefab(PrefabAsset) != nullptr);
		PF_CHECK(Tests, (*Provider)->FindMaterial(SourceMaterial) != nullptr);
		PF_CHECK(Tests, (*Provider)->FindMaterial(OverrideMaterial) != nullptr);
		PF_CHECK(Tests, (*Provider)->FindScript(ScriptAsset) != nullptr);
		auto RuntimeScene = (*Provider)->LoadScene(StartupScene);
		PF_CHECK(Tests, RuntimeScene.has_value());
		if (!RuntimeScene) return;
		const auto LeftRoot = (*RuntimeScene)->FindEntity(UUID{ 0x2100000000004000ull, 0x8000000000000001ull });
		const auto LeftChild = (*RuntimeScene)->FindEntity(UUID{ 0x2100000000004000ull, 0x8000000000000002ull });
		const auto RightChild = (*RuntimeScene)->FindEntity(UUID{ 0x2200000000004000ull, 0x8000000000000002ull });
		PF_CHECK(Tests, LeftRoot.has_value() && LeftChild.has_value() && RightChild.has_value());
		if (!LeftRoot || !LeftChild || !RightChild) return;
		const auto LeftInstance = LeftRoot->GetPrefabInstance();
		PF_CHECK(Tests, LeftInstance && LeftInstance->has_value() && (**LeftInstance).SourceAsset == PrefabAsset);
		PF_CHECK(Tests, LeftInstance && LeftInstance->has_value() && !(**LeftInstance).Overrides.empty());
		const auto LeftRenderer = LeftChild->GetMeshRenderer();
		const auto RightRenderer = RightChild->GetMeshRenderer();
		PF_CHECK(Tests, LeftRenderer && LeftRenderer->has_value());
		PF_CHECK(Tests, RightRenderer && RightRenderer->has_value());
		if (LeftRenderer && LeftRenderer->has_value())
			PF_CHECK(Tests, (**LeftRenderer).MaterialAsset == OverrideMaterial);
		if (RightRenderer && RightRenderer->has_value())
			PF_CHECK(Tests, (**RightRenderer).MaterialAsset == SourceMaterial);

		auto LoadedSource = PrefabAssetService::LoadSource(
			PrefabAsset, OpenedProject->GetRootPath(), OpenedProject->GetAssetRegistry());
		PF_CHECK(Tests, LoadedSource.has_value());
		if (LoadedSource && LeftInstance && LeftInstance->has_value())
		{
			if ((**LeftInstance).AppliedSourceFingerprint != LoadedSource->GetFingerprint())
				std::cerr << "G15_SAMPLE_SOURCE_FINGERPRINT_EXPECTED " << LoadedSource->GetFingerprint() << '\n';
			PF_CHECK(Tests, (**LeftInstance).AppliedSourceFingerprint == LoadedSource->GetFingerprint());
		}
#endif
	}
}

void RunAssetCookServiceTests(TestRunner& Tests)
{
	using namespace PulseForge;
	CookFixture Fixture;
	PF_CHECK(Tests, !Fixture.Root.empty());
	if (Fixture.Root.empty()) return;
	auto Snapshot = Fixture.Build();
	PF_CHECK(Tests, Snapshot.has_value());
	if (!Snapshot) return;
	AssetCookRequest Request = Fixture.Request(*Snapshot);
	auto Planned = AssetCookService::Plan(Request);
	if (!Planned)
	{
		std::cerr << "Cook planning failed: " << Planned.error().Message << " (";
		TestSupport::WritePathUtf8(std::cerr, Planned.error().Path);
		std::cerr << ")\n";
	}
	PF_CHECK(Tests, Planned.has_value());
	if (!Planned) return;
	auto First = RunCookWithBoundary("first-manifest", [&] { return AssetCookService::Cook(*Planned); });
	if (!First) std::cerr << "Cook fixture failed: " << First.error().Message << "\n";
	PF_CHECK(Tests, First.has_value());
	if (!First) return;
	const auto FirstManifestPath = First->ManifestPath;
	std::ifstream FirstManifestStream(FirstManifestPath, std::ios::binary);
	const std::string FirstManifest{ std::istreambuf_iterator<char>(FirstManifestStream), std::istreambuf_iterator<char>() };
	FirstManifestStream.close();
	PF_CHECK(Tests, !FirstManifest.empty());
	auto Second = RunCookWithBoundary("repeat-same-generation", [&] { return AssetCookService::Cook(Request); });
	if (!Second) std::cerr << "Second cook fixture failed: " << Second.error().Message << "\n";
	PF_CHECK(Tests, Second.has_value());
	if (!Second) return;
	std::ifstream SecondManifestStream(Second->ManifestPath, std::ios::binary);
	const std::string SecondManifest{ std::istreambuf_iterator<char>(SecondManifestStream), std::istreambuf_iterator<char>() };
	SecondManifestStream.close();
	PF_CHECK(Tests, FirstManifest == SecondManifest);
	PF_CHECK(Tests, First->GenerationIdentity == Second->GenerationIdentity);
	auto DistRequest = Request;
	DistRequest.Compatibility.Configuration = CookedBuildConfiguration::Dist;
	DistRequest.OutputRoot = Fixture.Root / "DistCook";
	const auto DistCook = RunCookWithBoundary("dist-provenance", [&] { return AssetCookService::Cook(DistRequest); });
	PF_CHECK(Tests, DistCook.has_value());
	if (DistCook)
	{
		PF_CHECK(Tests, DistCook->GenerationIdentity == First->GenerationIdentity);
		auto DistProvider = CookedContentProvider::Open(DistRequest.OutputRoot, Fixture.Compatibility);
		PF_CHECK(Tests, DistProvider.has_value());
		if (DistProvider)
			PF_CHECK(Tests, (*DistProvider)->GetCompatibility().Configuration == CookedBuildConfiguration::Dist);
	}
	auto InvalidAbiRequest = Request;
	InvalidAbiRequest.Compatibility.RuntimeABI = "future-runtime-abi";
	const auto InvalidAbi = AssetCookService::Plan(InvalidAbiRequest);
	PF_CHECK(Tests, !InvalidAbi && InvalidAbi.error().Code == AssetCookErrorCode::UnsupportedTarget);
	auto Provider = CookedContentProvider::Open(Fixture.Output, Fixture.Compatibility);
	PF_CHECK(Tests, Provider.has_value());
	if (!Provider) return;
	PF_CHECK(Tests, (*Provider)->GetStartupScene() == Fixture.SceneAsset);
	const auto SceneBytes = (*Provider)->ReadScene(Fixture.SceneAsset);
	PF_CHECK(Tests, SceneBytes.has_value());
	PF_CHECK(Tests, !(*Provider)->FindScript(Fixture.ScriptAsset));
	const auto RuntimeInputMap = (*Provider)->LoadGameplayInput((*Provider)->GetProjectID());
	PF_CHECK(Tests, RuntimeInputMap.has_value());
	if (RuntimeInputMap)
	{
		PF_CHECK(Tests, RuntimeInputMap->Actions.size() == 1);
		PF_CHECK(Tests, RuntimeInputMap->Actions.front().Id == "Jump");
		GameplayInputService RuntimeInput;
		PF_CHECK(Tests, RuntimeInput.SetActionMap(*RuntimeInputMap).has_value());
		PF_CHECK(Tests, RuntimeInput.QueueEvent({
			.Kind = InputEventKind::Pressed,
			.Control = { InputDevice::Keyboard, static_cast<uint16_t>(InputKey::Space) },
			.Window = 1 }));
		InputRoutingState Routing;
		Routing.FocusedWindow = 1;
		Routing.Contexts[0] = { InputContext::Gameplay, InputDeviceMask::Keyboard, 1, true };
		Routing.ContextCount = 1;
		RuntimeInput.PublishFrame(Routing);
		const ActionValue* JumpValue = RuntimeInput.FindAction("Jump");
		PF_CHECK(Tests, JumpValue && JumpValue->Pressed && JumpValue->Held);
	}
	auto MaterialRequest = Fixture.Request(*Snapshot);
	MaterialRequest.DynamicRoots = { Fixture.MaterialAsset };
	MaterialRequest.OutputRoot = Fixture.Root / "MaterialCook";
	const auto MaterialCook = RunCookWithBoundary("material-and-texture-products", [&]
		{ return AssetCookService::Cook(MaterialRequest); });
	PF_CHECK(Tests, MaterialCook.has_value());
	if (MaterialCook)
	{
		auto MaterialProvider = CookedContentProvider::Open(MaterialRequest.OutputRoot, Fixture.Compatibility);
		PF_CHECK(Tests, MaterialProvider.has_value());
		if (MaterialProvider)
		{
			auto CookedMaterial = (*MaterialProvider)->LoadMaterialProduct(Fixture.MaterialAsset);
			PF_CHECK(Tests, CookedMaterial.has_value());
			if (CookedMaterial)
			{
				const std::array<float, 4> ExpectedFactor{ 0.25f, 0.5f, 0.75f, 1.0f };
				PF_CHECK(Tests, CookedMaterial->BaseColorFactor == ExpectedFactor);
			}
		}
	}
	auto StartupOverrideRequest = Fixture.Request(*Snapshot);
	StartupOverrideRequest.StartupScene = Fixture.AlternateSceneAsset;
	StartupOverrideRequest.OutputRoot = Fixture.Root / "Override";
	auto StartupOverrideCook = RunCookWithBoundary("startup-scene-override", [&]
		{ return AssetCookService::Cook(StartupOverrideRequest); });
	PF_CHECK(Tests, StartupOverrideCook.has_value());
	if (StartupOverrideCook)
	{
		auto OverrideProvider = CookedContentProvider::Open(StartupOverrideRequest.OutputRoot, Fixture.Compatibility);
		PF_CHECK(Tests, OverrideProvider.has_value());
		if (OverrideProvider) PF_CHECK(Tests, (*OverrideProvider)->GetStartupScene() == Fixture.AlternateSceneAsset);
	}

	auto DynamicRequest = Request;
	DynamicRequest.DynamicRoots.push_back(Fixture.ScriptAsset);
	auto WithDynamic = RunCookWithBoundary("explicit-dynamic-root", [&]
		{ return AssetCookService::Cook(DynamicRequest); });
	PF_CHECK(Tests, WithDynamic.has_value());
	if (!WithDynamic) return;
	Provider = CookedContentProvider::Open(Fixture.Output, Fixture.Compatibility);
	PF_CHECK(Tests, Provider.has_value());
	if (!Provider) return;
	const auto ScriptBytes = (*Provider)->ReadScript(Fixture.ScriptAsset);
	PF_CHECK(Tests, ScriptBytes.has_value());
	const CookedAssetEntry* FormerDynamicScript = (*Provider)->FindScript(Fixture.ScriptAsset);
	PF_CHECK(Tests, FormerDynamicScript != nullptr);
	if (!FormerDynamicScript) return;
	const std::filesystem::path FormerScriptPayload = Fixture.Output / std::filesystem::u8path(FormerDynamicScript->RelativePayloadPath);
	const std::filesystem::path Sentinel = Fixture.Output / "keep-this-user-file.pfartifact";
	CookFixture::WriteText(Sentinel, "unowned sentinel");
	const auto WithoutDynamic = RunCookWithBoundary("remove-dynamic-root", [&]
		{ return AssetCookService::Cook(Request); });
	if (!WithoutDynamic) std::cerr << "Cook without dynamic root failed: " << WithoutDynamic.error().Message << "\n";
	PF_CHECK(Tests, WithoutDynamic.has_value());
	if (!WithoutDynamic) return;
	PF_CHECK(Tests, !std::filesystem::exists(FormerScriptPayload));
	PF_CHECK(Tests, std::filesystem::exists(Sentinel));
	Provider = CookedContentProvider::Open(Fixture.Output, Fixture.Compatibility);
	PF_CHECK(Tests, Provider.has_value());
	if (!Provider) return;
	PF_CHECK(Tests, !(*Provider)->FindScript(Fixture.ScriptAsset));
	std::ifstream DynamicManifestStream(FirstManifestPath, std::ios::binary);
	const std::string DynamicManifest{ std::istreambuf_iterator<char>(DynamicManifestStream), std::istreambuf_iterator<char>() };
	DynamicManifestStream.close();

	auto MissingRoot = Request;
	MissingRoot.DynamicRoots.push_back(UUID{ 99, 100 });
	auto Rejected = AssetCookService::Plan(MissingRoot);
	PF_CHECK(Tests, !Rejected && Rejected.error().Code == AssetCookErrorCode::InvalidRoot);

	const auto BeforeMutation = std::filesystem::file_size(Fixture.Source / "Assets" / "Startup.scene");
	{
		const auto ScenePath = Fixture.Source / "Assets" / "Startup.scene";
		std::ifstream Input(ScenePath, std::ios::binary);
		std::string Changed{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
		if (!Changed.empty()) Changed[0] = Changed[0] == '{' ? '[' : '{';
		CookFixture::WriteText(ScenePath, Changed);
	}
	PF_CHECK(Tests, std::filesystem::file_size(Fixture.Source / "Assets" / "Startup.scene") == BeforeMutation);
	auto Stale = RunCookWithBoundary("reject-stale-plan", [&] { return AssetCookService::Cook(*Planned); });
	PF_CHECK(Tests, !Stale && Stale.error().Code == AssetCookErrorCode::SourceChanged);
	std::ifstream RetainedManifestStream(FirstManifestPath, std::ios::binary);
	const std::string RetainedManifest{ std::istreambuf_iterator<char>(RetainedManifestStream), std::istreambuf_iterator<char>() };
	PF_CHECK(Tests, RetainedManifest == DynamicManifest);

	// Exercise production extraction and cooking against a real supported glTF/scene project copied to two roots.
	// The source assets remain untouched, and equal UUID/content inputs must not leak either absolute root into output.
	CookFixture RealFixture;
	std::error_code CopyError;
	const auto RealSource = RealFixture.Root / "R";
	std::filesystem::copy(
		std::filesystem::path(PF_COOK_FIXTURE_PROJECT).parent_path(), RealSource,
		std::filesystem::copy_options::recursive, CopyError);
	PF_CHECK(Tests, !CopyError);
	if (!CopyError)
	{
		auto RealProject = Project::Open(RealSource / "EditorWorkflow.pfproj");
		PF_CHECK(Tests, RealProject.has_value());
		if (RealProject && RealProject->GetDescription().StartScene)
		{
			auto RealSnapshot = AssetDependencyCatalogBuilder::Build(
				RealProject->GetRootPath(), RealProject->GetDescription().ID, 2);
			PF_CHECK(Tests, RealSnapshot.has_value());
			if (RealSnapshot)
			{
				const auto RealScene = *RealProject->GetDescription().StartScene;
				auto RealRequest = RealFixture.Request(*RealSnapshot);
				RealRequest.SourceProjectFile = RealProject->GetFilePath();
				RealRequest.StartupScene = RealScene;
				RealRequest.OutputRoot = RealFixture.Root / "A";
				auto RealPlan = AssetCookService::Plan(RealRequest);
				PF_CHECK(Tests, RealPlan.has_value());
				if (RealPlan)
				{
					auto RealCook = RunCookWithBoundary("real-format-relocated-root-a", [&]
						{ return AssetCookService::Cook(*RealPlan); });
					if (!RealCook) std::cerr << "Real-format cook failed: " << RealCook.error().Message << "\n";
					PF_CHECK(Tests, RealCook.has_value());
					if (RealCook)
					{
						const auto UnavailableSource = RealFixture.Root / "R unavailable";
						std::filesystem::rename(RealSource, UnavailableSource, CopyError);
						PF_CHECK(Tests, !CopyError);
						auto RealProvider = CookedContentProvider::Open(RealRequest.OutputRoot, RealFixture.Compatibility);
						PF_CHECK(Tests, RealProvider.has_value());
						const auto Records = (*RealSnapshot)->GetRegistry().GetAssets();
						const auto ModelRecord = std::find_if(Records.begin(), Records.end(), [](const AssetRecord& Record)
						{
							return Record.ProjectRelativePath == std::filesystem::path("Assets/Models/WorkflowCube.gltf");
						});
					PF_CHECK(Tests, ModelRecord != Records.end());
					if (RealProvider && ModelRecord != Records.end())
						{
							const CookedAssetEntry* MeshEntry = (*RealProvider)->FindMeshPrimitive(
								ModelRecord->ID, "mesh/0/primitive/0");
							PF_CHECK(Tests, MeshEntry && MeshEntry->RelativePayloadPath.size() < 96);
							const auto MeshPayload = (*RealProvider)->ReadMeshPrimitive(
								ModelRecord->ID, "mesh/0/primitive/0");
							PF_CHECK(Tests, MeshPayload.has_value());
							if (MeshPayload)
							{
								auto Mesh = Detail::DecodeMeshProduct(*MeshPayload);
								PF_CHECK(Tests, Mesh.has_value());
								if (Mesh)
								{
									PF_CHECK(Tests, Mesh->Vertices.size() == 24);
									PF_CHECK(Tests, Mesh->Indices.size() == 36);
								}
							}
						}
						const auto ScenePayload = (*RealProvider)->ReadScene(RealScene);
						PF_CHECK(Tests, ScenePayload.has_value());
						const auto LoadedCookedScene = (*RealProvider)->LoadScene(RealScene);
						PF_CHECK(Tests, LoadedCookedScene.has_value());
						if (LoadedCookedScene) PF_CHECK(Tests, (*LoadedCookedScene)->GetEntities().size() == 13);

						const auto SecondSource = RealFixture.Root / "R2";
						std::filesystem::copy(
							std::filesystem::path(PF_COOK_FIXTURE_PROJECT).parent_path(), SecondSource,
							std::filesystem::copy_options::recursive, CopyError);
						PF_CHECK(Tests, !CopyError);
						if (!CopyError)
						{
							auto SecondProject = Project::Open(SecondSource / "EditorWorkflow.pfproj");
							PF_CHECK(Tests, SecondProject.has_value());
							if (SecondProject)
							{
								auto SecondSnapshot = AssetDependencyCatalogBuilder::Build(
									SecondProject->GetRootPath(), SecondProject->GetDescription().ID, 2);
								PF_CHECK(Tests, SecondSnapshot.has_value());
								if (SecondSnapshot)
								{
									auto SecondRequest = RealRequest;
									SecondRequest.SourceSnapshot = *SecondSnapshot;
									SecondRequest.SourceProjectFile = SecondProject->GetFilePath();
									SecondRequest.OutputRoot = RealFixture.Root / "B";
									auto SecondCook = RunCookWithBoundary("real-format-relocated-root-b", [&]
									{ return AssetCookService::Cook(SecondRequest); });
									PF_CHECK(Tests, SecondCook.has_value());
									if (SecondCook)
									{
										std::ifstream FirstManifestFile(RealCook->ManifestPath, std::ios::binary);
										const std::string FirstManifestText{
											std::istreambuf_iterator<char>(FirstManifestFile), std::istreambuf_iterator<char>() };
										std::ifstream SecondManifestFile(SecondCook->ManifestPath, std::ios::binary);
										const std::string SecondManifestText{
											std::istreambuf_iterator<char>(SecondManifestFile), std::istreambuf_iterator<char>() };
										PF_CHECK(Tests, FirstManifestText == SecondManifestText);
									}
								}
							}
						}
					}
				}
			}
		}
	}
	TestLinkedPrefabSampleCookAndRuntimeScene(Tests);
}
