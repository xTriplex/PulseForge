#include "Assets/AssetCookService.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetRegistry.h"
#include "Assets/Project.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/SceneAssetService.h"
#include "Assets/AssetCookService.h"
#include "Assets/AssetDependencyCatalog.h"
#include "Assets/Detail/AssetCpuProducts.h"
#include "Assets/AssetTypes.h"
#include "Assets/MaterialAsset.h"
#include "Assets/MaterialAssetCache.h"
#include "Assets/MaterialGraphSource.h"
#include "Assets/MaterialGraphCompiler.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Runtime/CookedContentProvider.h"
#include "Runtime/PlayerSession.h"
#include "Scripting/BlueprintAsset.h"
#include "Scripting/ScriptRuntime.h"
#include "Scene/SceneSerializer.h"
#include "TestSupport.h"

#include <filesystem>
#include <array>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <span>
#include <iostream>
#include <limits>
#include <future>
#include <string>
#include <string_view>
#include <thread>

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
		std::filesystem::path SceneSourcePath;
		AssetID SceneAsset;
		AssetID AlternateSceneAsset;
		AssetID ScriptAsset;
		AssetID BlueprintAsset;
		std::filesystem::path BlueprintSourcePath;
		AssetID MaterialAsset;
		AssetID GraphMaterialAsset;
		AssetID ColorGraphMaterialAsset;
		AssetID ColorGraphTextureAsset;
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
			if (!SceneData.CreateEntity("Cooked scene content")) { Root.clear(); return; }
			auto SceneText = SceneSerializer::Serialize(SceneData);
			if (!SceneText) { Root.clear(); return; }
			SceneSourcePath = Source / "Assets" / "Startup.scene";
			WriteText(SceneSourcePath, *SceneText);
			auto SceneMetadata = AssetMetadataSerializer::CreateForNewAsset(SceneSourcePath);
			if (!SceneMetadata) { Root.clear(); return; }
			SceneAsset = SceneMetadata->ID;
			const auto AlternateScenePath = Source / "Assets" / "Alternate.scene";
			WriteText(AlternateScenePath, *SceneText);
			auto AlternateSceneMetadata = AssetMetadataSerializer::CreateForNewAsset(AlternateScenePath);
			if (!AlternateSceneMetadata) { Root.clear(); return; }
			AlternateSceneAsset = AlternateSceneMetadata->ID;
			const auto ScriptPath = Source / "Assets" / "Scripts" / "Dynamic.lua";
			std::filesystem::create_directories(ScriptPath.parent_path());
			WriteText(ScriptPath, "function OnUpdate() end\n");
			auto ScriptMetadata = AssetMetadataSerializer::CreateForNewAsset(ScriptPath);
			if (!ScriptMetadata) { Root.clear(); return; }
			ScriptAsset = ScriptMetadata->ID;
			BlueprintProgramSource OnCreate;
			OnCreate.Constants = { 2.0f };
			OnCreate.ScalarPropertyIds = { "health" };
			OnCreate.Instructions = {
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(30, 1) },
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(30, 2) },
				{ BlueprintOpcode::Add, 0, 0, UUID(30, 3) },
				{ BlueprintOpcode::StoreScalar, 0, 0, UUID(30, 4) },
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(30, 5) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(30, 6) },
				{ BlueprintOpcode::Return, 0, 0, UUID(30, 7) }
			};
			BlueprintProgramSource OnUpdate;
			OnUpdate.Constants = { 1.0f };
			OnUpdate.ScalarPropertyIds = { "health" };
			OnUpdate.Instructions = {
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(31, 1) },
				{ BlueprintOpcode::PushConstant, 0, 0, UUID(31, 2) },
				{ BlueprintOpcode::Add, 0, 0, UUID(31, 3) },
				{ BlueprintOpcode::StoreScalar, 0, 0, UUID(31, 4) },
				{ BlueprintOpcode::LoadScalar, 0, 0, UUID(31, 5) },
				{ BlueprintOpcode::WriteScalarProperty, 0, 0, UUID(31, 6) },
				{ BlueprintOpcode::Return, 0, 0, UUID(31, 7) }
			};
			const std::array<BlueprintEventProgramSource, 2> BlueprintEvents{
				BlueprintEventProgramSource{ BlueprintEventKind::OnCreate, std::move(OnCreate) },
				BlueprintEventProgramSource{ BlueprintEventKind::OnUpdate, std::move(OnUpdate) }
			};
			auto BlueprintBytes = SerializeBlueprintAsset(BlueprintEvents);
			if (!BlueprintBytes) { Root.clear(); return; }
			BlueprintSourcePath = Source / "Assets" / "Behavior.pfblueprint";
			{
				std::ofstream Output(BlueprintSourcePath, std::ios::binary | std::ios::trunc);
				Output.write(reinterpret_cast<const char*>(BlueprintBytes->data()), static_cast<std::streamsize>(BlueprintBytes->size()));
				if (!Output) { Root.clear(); return; }
			}
			auto BlueprintMetadata = AssetMetadataSerializer::CreateForNewAsset(BlueprintSourcePath);
			if (!BlueprintMetadata) { Root.clear(); return; }
			BlueprintAsset = BlueprintMetadata->ID;
			Scene AttachedScene;
			if (!SceneSerializer::LoadFromFile(SceneSourcePath, AttachedScene)) { Root.clear(); return; }
			auto AttachedEntity = AttachedScene.GetEntities().front();
			BlueprintComponent AttachedBlueprint{ BlueprintAsset, true };
			if (!AttachedBlueprint.SetFloatProperty("health", 40.0f) || !AttachedEntity.SetBlueprint(AttachedBlueprint))
				{ Root.clear(); return; }
			const auto AttachedSceneText = SceneSerializer::Serialize(AttachedScene);
			if (!AttachedSceneText) { Root.clear(); return; }
			WriteText(SceneSourcePath, *AttachedSceneText);
			const auto MaterialPath = Source / "Assets" / "Cooked.material";
			MaterialAssetDesc MaterialDescription;
			MaterialDescription.BaseColorFactor = { 0.25f, 0.5f, 0.75f, 1.0f };
			auto MaterialText = MaterialAssetSerializer::Serialize(MaterialDescription);
			if (!MaterialText) { Root.clear(); return; }
			WriteText(MaterialPath, *MaterialText);
			auto MaterialMetadata = AssetMetadataSerializer::CreateForNewAsset(MaterialPath);
			if (!MaterialMetadata) { Root.clear(); return; }
			MaterialAsset = MaterialMetadata->ID;
			MaterialGraphSource Graph;
			Graph.GraphID = UUID(10, 1);
			Graph.Nodes = {
				{ UUID(10, 2), MaterialGraphNodeKind::FloatConstant, 0.4f, {} },
				{ UUID(10, 3), MaterialGraphNodeKind::FloatParameter, 0.1f, "Accent" },
				{ UUID(10, 4), MaterialGraphNodeKind::Add, 0.0f, {} },
				{ UUID(10, 5), MaterialGraphNodeKind::PbrOutput, 0.0f, {} }
			};
			Graph.Connections = {
				{ UUID(11, 1), UUID(10, 2), UUID(10, 4), 0 },
				{ UUID(11, 2), UUID(10, 3), UUID(10, 4), 1 },
				{ UUID(11, 3), UUID(10, 4), UUID(10, 5), 0 }
			};
			MaterialAssetDesc GraphDescription;
			GraphDescription.Graph = Graph;
			auto GraphBytes = MaterialAssetSerializer::Serialize(GraphDescription);
			if (!GraphBytes) { Root.clear(); return; }
			const auto GraphPath = Source / "Assets" / "Graph.material";
			WriteText(GraphPath, *GraphBytes);
			auto GraphMetadata = AssetMetadataSerializer::CreateForNewAsset(GraphPath);
			if (!GraphMetadata) { Root.clear(); return; }
			GraphMaterialAsset = GraphMetadata->ID;
			const std::filesystem::path SampleTexturePath = std::filesystem::path(PF_COOK_FIXTURE_PROJECT).parent_path().parent_path() /
				"PBRMaterials" / "Assets" / "Textures" / "AlbedoTiles.ppm";
			const std::filesystem::path FixtureTexturePath = Source / "Assets" / "AlbedoTiles.ppm";
			std::filesystem::copy_file(SampleTexturePath, FixtureTexturePath,
				std::filesystem::copy_options::overwrite_existing, Error);
			if (Error) { Root.clear(); return; }
			auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(FixtureTexturePath);
			if (!TextureMetadata) { Root.clear(); return; }
			ColorGraphTextureAsset = TextureMetadata->ID;
			MaterialGraphSource ColorGraph;
			ColorGraph.GraphID = UUID(10, 6);
			ColorGraph.Nodes = {
				{ UUID(10, 7), MaterialGraphNodeKind::UV0, 0.0f, {}, 80.0, 100.0 },
				{ UUID(10, 9), MaterialGraphNodeKind::BaseColorTextureSample, 0.0f, {}, 280.0, 100.0 },
				{ UUID(10, 8), MaterialGraphNodeKind::PbrOutput, 0.0f, {}, 500.0, 100.0 }
			};
			ColorGraph.Connections = {
				{ UUID(11, 4), UUID(10, 7), UUID(10, 9), 0 },
				{ UUID(11, 5), UUID(10, 9), UUID(10, 8), 0 }
			};
			MaterialAssetDesc ColorGraphDescription;
			ColorGraphDescription.BaseColorTexture = ColorGraphTextureAsset;
			ColorGraphDescription.BaseColorFactor = { 0.25f, 0.5f, 0.75f, 0.6f };
			ColorGraphDescription.MetallicFactor = 0.35f;
			ColorGraphDescription.RoughnessFactor = 0.42f;
			ColorGraphDescription.AlphaMode = MaterialAlphaMode::Mask;
			ColorGraphDescription.AlphaCutoff = 0.31f;
			ColorGraphDescription.DoubleSided = true;
			ColorGraphDescription.Graph = std::move(ColorGraph);
			auto ColorGraphBytes = MaterialAssetSerializer::Serialize(ColorGraphDescription);
			if (!ColorGraphBytes) { Root.clear(); return; }
			const auto ColorGraphPath = Source / "Assets" / "GraphColor.material";
			WriteText(ColorGraphPath, *ColorGraphBytes);
			auto ColorGraphMetadata = AssetMetadataSerializer::CreateForNewAsset(ColorGraphPath);
			if (!ColorGraphMetadata) { Root.clear(); return; }
			ColorGraphMaterialAsset = ColorGraphMetadata->ID;
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

	void TestSkeletalMeshCookedProviderRoundTrip(TestRunner& Tests)
	{
		CookFixture Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty());
		if (Fixture.Root.empty()) return;

		const std::filesystem::path ModelDirectory = Fixture.Source / "Assets" / "Models";
		std::error_code Error;
		std::filesystem::create_directories(ModelDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		std::vector<std::byte> Buffer;
		for (const float Value : std::array<float, 9>{ 0, 0, 0, 1, 0, 0, 0, 1, 0 })
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				Buffer.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		}
		for (size_t Index = 0; Index < 12; ++Index) Buffer.push_back(std::byte{ 0 });
		for (const float Value : std::array<float, 12>{ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 })
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				Buffer.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		}
		for (const uint16_t Index : std::array<uint16_t, 3>{ 0, 1, 2 })
		{
			Buffer.push_back(static_cast<std::byte>(Index & 0xff));
			Buffer.push_back(static_cast<std::byte>(Index >> 8));
		}
		Buffer.push_back(std::byte{ 0 });
		Buffer.push_back(std::byte{ 0 });
		for (const float Value : std::array<float, 16>{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 })
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				Buffer.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		}
		const auto BufferPath = ModelDirectory / "Skinned.bin";
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Buffer.data()), static_cast<std::streamsize>(Buffer.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(BufferPath).has_value());
		const auto GltfPath = ModelDirectory / "Skinned.gltf";
		CookFixture::WriteText(GltfPath,
			"{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"Skinned.bin\",\"byteLength\":168}],"
			"\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
			"{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
			"{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":104,\"byteLength\":64}],"
			"\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
			"{\"bufferView\":1,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
			"{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
			"{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
			"{\"bufferView\":4,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"}],"
			"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1,\"WEIGHTS_0\":2},\"indices\":3}]}],"
			"\"nodes\":[{\"mesh\":0,\"skin\":0,\"name\":\"mesh\"},{\"name\":\"root\",\"children\":[2]},{\"name\":\"joint\"}],"
			"\"skins\":[{\"joints\":[2],\"inverseBindMatrices\":4}]}");
		const auto MeshMetadata = AssetMetadataSerializer::CreateForNewAsset(GltfPath);
		PF_CHECK(Tests, MeshMetadata.has_value());
		if (!MeshMetadata) return;

		auto Snapshot = Fixture.Build();
		if (!Snapshot) std::cerr << "Skeletal cook fixture snapshot failed: " << Snapshot.error().Message << "\n";
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		auto Request = Fixture.Request(*Snapshot);
		Request.DynamicRoots.push_back(MeshMetadata->ID);
		auto Plan = AssetCookService::Plan(Request);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		constexpr std::string_view Subresource = "skeletal/node/0/mesh/0/primitive/0";
		const auto Planned = std::find_if(Plan->Products.begin(), Plan->Products.end(), [&](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == MeshMetadata->ID && Product.Product == CookedAssetProductKind::SkeletalMesh && Product.Subresource == Subresource; });
		PF_CHECK(Tests, Planned != Plan->Products.end());
		if (Planned == Plan->Products.end()) return;

		auto Cooked = AssetCookService::Cook(*Plan);
		if (!Cooked) std::cerr << "Skeletal cook fixture cook failed: " << Cooked.error().Message << "\n";
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		auto Provider = CookedContentProvider::Open(Fixture.Output, Fixture.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		const auto Product = (*Provider)->LoadSkeletalMesh(MeshMetadata->ID, Subresource);
		PF_CHECK(Tests, Product.has_value());
		if (Product)
		{
			PF_CHECK(Tests, Product->Geometry.Vertices.size() == 3);
			PF_CHECK(Tests, Product->JointNodeIndices.size() == 1);
			if (Product->JointNodeIndices.size() == 1 && Product->JointNodeIndices.front() < Product->SkeletonNodes.size())
				PF_CHECK(Tests, Product->SkeletonNodes[Product->JointNodeIndices.front()].Name == "joint");
			else PF_CHECK(Tests, false);
			PF_CHECK(Tests, Product->VertexInfluences.size() == 3);
		}
	}

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
		const auto SystemTemporaryDirectory = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto TemporaryDirectory = std::filesystem::weakly_canonical(SystemTemporaryDirectory, Error);
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
		if (!Plan)
			std::cerr << "G15_SAMPLE_COOK_PLAN_ERROR code=" << static_cast<unsigned>(Plan.error().Code)
				<< " message=" << Plan.error().Message << std::endl;
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
		Scene AuthoredScene;
		auto AuthoredSceneLoad = SceneAssetService::Load(
			StartupScene, OpenedProject->GetRootPath(), OpenedProject->GetAssetRegistry(), AuthoredScene);
		PF_CHECK(Tests, AuthoredSceneLoad.has_value());
		const auto AuthoredLeftRoot = AuthoredScene.FindEntity(UUID{ 0x2100000000004000ull, 0x8000000000000001ull });
		PF_CHECK(Tests, AuthoredLeftRoot.has_value());
		std::string AuthoredAppliedFingerprint;
		if (AuthoredLeftRoot)
		{
			const auto AuthoredMetadata = AuthoredLeftRoot->GetPrefabInstance();
			PF_CHECK(Tests, AuthoredMetadata && AuthoredMetadata->has_value());
			if (AuthoredMetadata && AuthoredMetadata->has_value())
				AuthoredAppliedFingerprint = (**AuthoredMetadata).AppliedSourceFingerprint;
		}
		if (LoadedSource && LeftInstance && LeftInstance->has_value())
		{
			PF_CHECK(Tests, !AuthoredAppliedFingerprint.empty());
			PF_CHECK(Tests, (**LeftInstance).AppliedSourceFingerprint == AuthoredAppliedFingerprint);
		}
#endif
	}

	void TestLuaAuthoringSampleCookAndRuntime(TestRunner& Tests)
	{
#if defined(PF_LUA_AUTHORING_SAMPLE_PROJECT)
		constexpr AssetID ScriptAsset{ 0xd006000000004000ull, 0x8000000000000001ull };
		constexpr AssetID ModuleAsset{ 0xd007000000004000ull, 0x8000000000000016ull };
		constexpr AssetID StartupScene{ 0xd001000000004000ull, 0x8000000000000001ull };
		constexpr AssetID PrefabAsset{ 0xd002000000004000ull, 0x8000000000000001ull };
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		std::error_code Error;
		const auto SystemTemporaryDirectory = std::filesystem::temp_directory_path(Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto TemporaryDirectory = std::filesystem::weakly_canonical(SystemTemporaryDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const std::filesystem::path TemporaryRoot = TemporaryDirectory /
			std::filesystem::u8path("PulseForge-LuaAuthoringCook-" + ID->ToString());
		const std::filesystem::path CookOutputRoot = TemporaryRoot.parent_path() /
			std::filesystem::u8path("PulseForge-LuaAuthoringCook-output-" + ID->ToString());
		struct OwnedRoot final
		{
			std::filesystem::path ProjectPath;
			std::filesystem::path OutputPath;
			~OwnedRoot()
			{
				std::error_code Error;
				std::filesystem::remove_all(ProjectPath, Error);
				Error.clear();
				std::filesystem::remove_all(OutputPath, Error);
			}
		} Cleanup{ TemporaryRoot, CookOutputRoot };
		std::filesystem::copy(std::filesystem::path(PF_LUA_AUTHORING_SAMPLE_PROJECT).parent_path(), TemporaryRoot,
			std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto ProjectValue = Project::Open(TemporaryRoot / std::filesystem::path(PF_LUA_AUTHORING_SAMPLE_PROJECT).filename());
		PF_CHECK(Tests, ProjectValue.has_value());
		if (!ProjectValue) return;
		auto Snapshot = AssetDependencyCatalogBuilder::Build(
			ProjectValue->GetRootPath(), ProjectValue->GetDescription().ID, 1);
		if (!Snapshot) std::cerr << "Lua authoring sample catalog failed: " << Snapshot.error().Message << "\n";
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		AssetCookRequest Request{
			*Snapshot, ProjectValue->GetFilePath(), StartupScene, {},
#if defined(_WIN32)
			{ "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#elif defined(__APPLE__)
			{ "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#else
			{ "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#endif
			CookOutputRoot
		};
		auto Plan = AssetCookService::Plan(Request);
		if (!Plan) std::cerr << "Lua authoring sample cook plan failed: " << Plan.error().Message << '\n';
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto HasProduct = [&](AssetID Asset)
		{
			return std::any_of(Plan->Products.begin(), Plan->Products.end(), [Asset](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == Asset && Product.Product == CookedAssetProductKind::Script; });
		};
		PF_CHECK(Tests, HasProduct(ScriptAsset) && HasProduct(ModuleAsset));
		auto Cooked = AssetCookService::Cook(*Plan);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		auto Provider = CookedContentProvider::Open(Request.OutputRoot, Request.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		PF_CHECK(Tests, (*Provider)->FindScriptByProjectPath("Assets/Scripts/LinkedInstancePulse.lua") != nullptr);
		PF_CHECK(Tests, (*Provider)->FindScriptByProjectPath("Assets/Scripts/shared/motion.lua") != nullptr);
		auto RuntimeScene = (*Provider)->LoadScene(StartupScene);
		PF_CHECK(Tests, RuntimeScene.has_value());
		if (!RuntimeScene) return;
		const UUID LeftChildID{ 0x2100000000004000ull, 0x8000000000000002ull };
		const UUID RightChildID{ 0x2200000000004000ull, 0x8000000000000002ull };
		const auto LeftChild = (*RuntimeScene)->FindEntity(LeftChildID);
		const auto RightChild = (*RuntimeScene)->FindEntity(RightChildID);
		PF_CHECK(Tests, LeftChild && RightChild);
		if (!LeftChild || !RightChild) return;
		ScriptRuntime Runtime(*Provider);
		PF_CHECK(Tests, Runtime.Start(**RuntimeScene).has_value());
		PF_CHECK(Tests, Runtime.Advance(**RuntimeScene, Timestep(1.0 / 60.0)).has_value());
		const auto LeftTransform = LeftChild->GetTransform();
		const auto RightTransform = RightChild->GetTransform();
		PF_CHECK(Tests, LeftTransform && LeftTransform->Translation.x == 1.25f);
		PF_CHECK(Tests, RightTransform && RightTransform->Translation.x == 0.4f);
		PF_CHECK(Tests, Runtime.GetDiagnostics().empty());
		Runtime.Stop();
		PF_CHECK(Tests, (*Provider)->FindPrefab(PrefabAsset) != nullptr);
#endif
	}

	void TestPhysicsQueriesSampleCookedPlayer(TestRunner& Tests)
	{
#if defined(PF_PHYSICS_QUERIES_SAMPLE_PROJECT)
		constexpr AssetID SceneAsset{ 0xb017000000004000ull, 0x8000000000000001ull };
		constexpr AssetID QueryScript{ 0xb017000000004000ull, 0x8000000000000003ull };
		constexpr AssetID SensorScript{ 0xb017000000004000ull, 0x8000000000000005ull };
		constexpr AssetID SphereTarget{ 0xb017000000004000ull, 0x8000000000000011ull };
		constexpr AssetID SensorEntity{ 0xb017000000004000ull, 0x8000000000000014ull };
		constexpr AssetID MovingEntity{ 0xb017000000004000ull, 0x8000000000000012ull };
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		std::error_code Error;
		const auto TemporaryDirectory = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(Error), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto SourceRoot = TemporaryDirectory / std::filesystem::u8path("PulseForge-PhysicsQueries-" + ID->ToString());
		const auto OutputRoot = TemporaryDirectory / std::filesystem::u8path("PulseForge-PhysicsQueries-Cook-" + ID->ToString());
		struct OwnedRoots final
		{
			std::filesystem::path Source;
			std::filesystem::path Output;
			~OwnedRoots()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Source, CleanupError);
				CleanupError.clear();
				std::filesystem::remove_all(Output, CleanupError);
			}
		} Cleanup{ SourceRoot, OutputRoot };
		std::filesystem::copy(std::filesystem::path(PF_PHYSICS_QUERIES_SAMPLE_PROJECT).parent_path(), SourceRoot,
			std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto OpenedProject = Project::Open(SourceRoot / std::filesystem::path(PF_PHYSICS_QUERIES_SAMPLE_PROJECT).filename());
		PF_CHECK(Tests, OpenedProject.has_value());
		if (!OpenedProject) return;
		auto Snapshot = AssetDependencyCatalogBuilder::Build(
			OpenedProject->GetRootPath(), OpenedProject->GetDescription().ID, 1);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		AssetCookRequest Request{
			*Snapshot, OpenedProject->GetFilePath(), SceneAsset, {},
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
		if (!Plan) std::cerr << "G17 sample cook plan failed: " << Plan.error().Message << '\n';
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto HasProduct = [&](AssetID Asset, CookedAssetProductKind Kind)
		{
			return std::any_of(Plan->Products.begin(), Plan->Products.end(), [Asset, Kind](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == Asset && Product.Product == Kind; });
		};
		PF_CHECK(Tests, HasProduct(QueryScript, CookedAssetProductKind::Script));
		PF_CHECK(Tests, HasProduct(SensorScript, CookedAssetProductKind::Script));

		// SceneRuntime is the shared coordinator used by Editor Play. Exercise the same authored
		// scripts and physics interactions through that source-project path before testing the cook.
		const auto SourceSceneRecord = OpenedProject->GetAssetRegistry().Find(SceneAsset);
		PF_CHECK(Tests, SourceSceneRecord.has_value());
		if (!SourceSceneRecord) return;
		Scene SourcePlayScene;
		const auto SourceScenePath = OpenedProject->GetRootPath() / SourceSceneRecord->ProjectRelativePath;
		const auto SourceSceneLoad = SceneSerializer::LoadFromFile(SourceScenePath, SourcePlayScene);
		PF_CHECK(Tests, SourceSceneLoad.has_value());
		if (!SourceSceneLoad) return;
		bool EditorPlayStopQueued = false;
		SceneRuntimeServices EditorPlayServices;
		EditorPlayServices.QueueStop = [&EditorPlayStopQueued]()
		{
			EditorPlayStopQueued = true;
			return true;
		};
		SceneRuntimeDesc EditorPlayDescription;
		EditorPlayDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		EditorPlayDescription.Physics.Gravity = glm::vec3(0.0f);
		SceneRuntime EditorPlay(*OpenedProject, EditorPlayDescription, std::move(EditorPlayServices));
		const auto EditorPlayStart = EditorPlay.Start(SourcePlayScene);
		PF_CHECK(Tests, EditorPlayStart.has_value());
		if (!EditorPlayStart) return;
		for (uint32_t Frame = 0; Frame < 300 && !EditorPlayStopQueued; ++Frame)
		{
			const auto Advanced = EditorPlay.Advance(SourcePlayScene, Timestep(1.0 / 60.0));
			if (!Advanced)
			{
				std::cerr << "G17 Editor Play sample failed: " << Advanced.error().Message << '\n';
				break;
			}
		}
		const bool EditorPlayHadDiagnostics = !EditorPlay.GetScriptDiagnostics().empty();
		EditorPlay.Stop(); // The host applies the queued Stop only after Advance returns.
		PF_CHECK(Tests, EditorPlayStopQueued && !EditorPlay.IsRunning());
		PF_CHECK(Tests, !EditorPlayHadDiagnostics);
		PF_CHECK(Tests, !SourcePlayScene.FindEntity(SensorEntity).has_value());

		auto Cooked = AssetCookService::Cook(*Plan);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		auto Provider = CookedContentProvider::Open(OutputRoot, Request.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		PF_CHECK(Tests, (*Provider)->FindScript(QueryScript) != nullptr && (*Provider)->FindScript(SensorScript) != nullptr);
		auto CookedScene = (*Provider)->LoadScene(SceneAsset);
		PF_CHECK(Tests, CookedScene.has_value());
		if (!CookedScene) return;
		const auto Sphere = (*CookedScene)->FindEntity(SphereTarget);
		const auto Trigger = (*CookedScene)->FindEntity(SensorEntity);
		PF_CHECK(Tests, Sphere && Trigger);
		if (!Sphere || !Trigger) return;
		const auto SphereCollider = Sphere->GetCollider();
		const auto TriggerCollider = Trigger->GetCollider();
		PF_CHECK(Tests, SphereCollider && SphereCollider->has_value() && (**SphereCollider).Shape == ColliderShape::Sphere);
		PF_CHECK(Tests, TriggerCollider && TriggerCollider->has_value() && (**TriggerCollider).IsSensor);

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.Gravity = glm::vec3(0.0f);
		PlayerSession Session(*Provider, RuntimeDescription);
		PF_CHECK(Tests, Session.Start().has_value());
		uint32_t Frames = 0;
		while (Session.IsRunning() && Frames < 300)
		{
			auto Frame = Session.Advance(Timestep(1.0 / 60.0));
			PF_CHECK(Tests, Frame.has_value());
			if (!Frame) break;
			++Frames;
		}
		PF_CHECK(Tests, Frames > 0 && Frames < 300);
		PF_CHECK(Tests, !Session.IsRunning());
#else
		(void)Tests;
#endif
	}

	void TestRuntimeUISampleCookedPlayer(TestRunner& Tests)
	{
#if defined(PF_RUNTIME_UI_SAMPLE_PROJECT)
		constexpr AssetID MenuScene{ 0xb018000000004000ull, 0x8000000000000010ull };
		constexpr AssetID GameScene{ 0xb018000000004000ull, 0x8000000000000011ull };
		constexpr AssetID MenuDocument{ 0xb018000000004000ull, 0x8000000000000030ull };
		constexpr AssetID GameDocument{ 0xb018000000004000ull, 0x8000000000000031ull };
		constexpr AssetID Font{ 0xb018000000004000ull, 0x8000000000000040ull };
		constexpr AssetID Image{ 0xb018000000004000ull, 0x8000000000000041ull };
		constexpr UUID ScoreText{ 0xb018000000004000ull, 0x8000000000000132ull };
		const auto ID = UUID::Generate();
		PF_CHECK(Tests, ID.has_value());
		if (!ID) return;
		std::error_code Error;
		const auto TemporaryDirectory = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(Error), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto SourceRoot = TemporaryDirectory / std::filesystem::u8path("PulseForge-RuntimeUI-" + ID->ToString());
		const auto OutputRoot = TemporaryDirectory / std::filesystem::u8path("PulseForge-RuntimeUI-Cook-" + ID->ToString());
		struct OwnedRoots final
		{
			std::filesystem::path Source;
			std::filesystem::path Output;
			~OwnedRoots()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Source, CleanupError);
				CleanupError.clear();
				std::filesystem::remove_all(Output, CleanupError);
			}
		} Cleanup{ SourceRoot, OutputRoot };
		std::filesystem::copy(std::filesystem::path(PF_RUNTIME_UI_SAMPLE_PROJECT).parent_path(), SourceRoot,
			std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto ProjectValue = Project::Open(SourceRoot / std::filesystem::path(PF_RUNTIME_UI_SAMPLE_PROJECT).filename());
		if (!ProjectValue)
			std::cerr << "RUNTIME_UI_SAMPLE_PROJECT_OPEN_FAILURE " << ProjectValue.error().Message << std::endl;
		PF_CHECK(Tests, ProjectValue.has_value());
		if (!ProjectValue) return;
		auto Snapshot = AssetDependencyCatalogBuilder::Build(ProjectValue->GetRootPath(), ProjectValue->GetDescription().ID, 1);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		AssetCookRequest Request{
			*Snapshot, ProjectValue->GetFilePath(), MenuScene, {},
#if defined(_WIN32)
			{ "Windows", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#elif defined(__APPLE__)
			{ "macOS", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#else
			{ "Linux", "Vulkan", std::string(CookedRuntimeABIIdentifier) },
#endif
			OutputRoot
		};
		Request.DynamicRoots.push_back(GameScene);
		auto Plan = AssetCookService::Plan(Request);
		PF_CHECK(Tests, Plan.has_value());
		if (!Plan) return;
		const auto HasProduct = [&](AssetID Asset, CookedAssetProductKind Kind)
		{
			return std::any_of(Plan->Products.begin(), Plan->Products.end(), [Asset, Kind](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == Asset && Product.Product == Kind; });
		};
		PF_CHECK(Tests, HasProduct(MenuDocument, CookedAssetProductKind::RuntimeUI));
		PF_CHECK(Tests, HasProduct(GameDocument, CookedAssetProductKind::RuntimeUI));
		PF_CHECK(Tests, HasProduct(Font, CookedAssetProductKind::Font));
		PF_CHECK(Tests, HasProduct(Image, CookedAssetProductKind::Texture));
		auto Cooked = AssetCookService::Cook(*Plan);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		auto Provider = CookedContentProvider::Open(OutputRoot, Request.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		PF_CHECK(Tests, (*Provider)->ReadRuntimeUI(MenuDocument).has_value());
		PF_CHECK(Tests, (*Provider)->ReadRuntimeUI(GameDocument).has_value());
		auto CookedFont = (*Provider)->ReadFont(Font);
		PF_CHECK(Tests, CookedFont && !CookedFont->empty());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(*Provider, RuntimeDescription);
		PF_CHECK(Tests, Session.Start().has_value());
		if (!Session.IsRunning()) return;
		const RuntimeUIViewport Viewport{ { 960.0f, 540.0f }, 960, 540 };
		const auto Pointer = [&](RuntimeUIInputKind Kind, glm::vec2 Position)
		{
			const RuntimeUIInputEvent Event{ Kind, Position, {} };
			if (!Session.SubmitRuntimeUIFrame(Viewport, std::span(&Event, 1))) return false;
			return Session.Advance(Timestep(1.0 / 60.0)).has_value();
		};
		PF_CHECK(Tests, Session.HasRuntimeUI());
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerPressed, { 480.0f, 260.0f }));
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerReleased, { 480.0f, 260.0f }));
		PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, Session.GetActiveSceneAsset() && *Session.GetActiveSceneAsset() == GameScene);
		if (!Session.GetActiveSceneAsset() || *Session.GetActiveSceneAsset() != GameScene) return;
		for (uint32_t Frame = 0; Frame < 90; ++Frame)
		{
			const auto Advanced = Session.Advance(Timestep(1.0 / 60.0));
			PF_CHECK(Tests, Advanced.has_value());
			if (!Advanced) return;
		}
		const auto FindScore = [&]() -> std::string
		{
			for (const RuntimeUIDrawCommand& Command : Session.GetRuntimeUIDrawCommands())
				if (Command.Element == ScoreText) return Command.Text;
			return {};
		};
		PF_CHECK(Tests, FindScore().starts_with("Score: ") && FindScore() != "Score: 0");
		RuntimeUIInputAdapter PassiveHostAdapter;
		const auto SubmitPassiveViewport = [&](RuntimeUIViewport CurrentViewport)
		{
			RuntimeUIHostInputSample Sample{
				.SessionGeneration = Session.GetRuntimeUISessionGeneration(),
				.TreeGeneration = Session.GetRuntimeUIInputGeneration(), .InputOwner = 19,
				.Viewport = CurrentViewport, .ViewportOrigin = { 23.0f, 41.0f }, .DeltaSeconds = 1.0 / 60.0,
				.Focused = true, .PointerPosition = glm::vec2{ 180.0f, 55.0f }, .PointerPressed = true,
				.PointerHeld = true, .Navigation = { 0.9f, 0.0f }, .ActivatePressed = true, .ActivateHeld = true };
			const RuntimeUIHostFrame Frame = BuildRuntimeUIHostFrame(
				PassiveHostAdapter, std::move(Sample), RuntimeUIInputPolicy::Passive);
			PF_CHECK(Tests, Frame.Viewport.LogicalSize == CurrentViewport.LogicalSize);
			PF_CHECK(Tests, Frame.Viewport.FramebufferWidth == CurrentViewport.FramebufferWidth &&
				Frame.Viewport.FramebufferHeight == CurrentViewport.FramebufferHeight);
			PF_CHECK(Tests, std::all_of(Frame.Events.begin(), Frame.Events.end(), [](const RuntimeUIInputEvent& Event)
			{
				return Event.Kind == RuntimeUIInputKind::CaptureCancelled || Event.Kind == RuntimeUIInputKind::FocusLost;
			}));
			if (!Session.SubmitRuntimeUIFrame(Frame.Viewport, Frame.Events)) return false;
			return Session.Advance(Timestep(1.0 / 60.0)).has_value();
		};
		const RuntimeUIViewport WideViewport{ { 1279.0f, 721.0f }, 1919, 1081 };
		PF_CHECK(Tests, SubmitPassiveViewport(WideViewport));
		PF_CHECK(Tests, Session.GetRuntimeUIViewport().LogicalSize == WideViewport.LogicalSize &&
			Session.GetRuntimeUIViewport().FramebufferWidth == WideViewport.FramebufferWidth &&
			Session.GetRuntimeUIViewport().FramebufferHeight == WideViewport.FramebufferHeight);
		const auto FindScoreCommand = [&]() -> const RuntimeUIDrawCommand*
		{
			for (const RuntimeUIDrawCommand& Command : Session.GetRuntimeUIDrawCommands())
				if (Command.Element == ScoreText) return &Command;
			return nullptr;
		};
		const RuntimeUIDrawCommand* ResizedScore = FindScoreCommand();
		PF_CHECK(Tests, ResizedScore && std::abs(ResizedScore->Bounds.Left - (130.0f * 1279.0f / 960.0f)) < 0.01f &&
			std::abs(ResizedScore->Bounds.Top - (30.0f * 721.0f / 540.0f)) < 0.01f &&
			std::abs(ResizedScore->Bounds.Right - (480.0f * 1279.0f / 960.0f)) < 0.01f &&
			std::abs(ResizedScore->Bounds.Bottom - (90.0f * 721.0f / 540.0f)) < 0.01f);
		const RuntimeUIRect ResizedScoreBounds = ResizedScore ? ResizedScore->Bounds : RuntimeUIRect{};
		const RuntimeUIViewport ScaledFramebufferViewport{ WideViewport.LogicalSize, 2558, 1442 };
		PF_CHECK(Tests, SubmitPassiveViewport(ScaledFramebufferViewport));
		PF_CHECK(Tests, Session.GetRuntimeUIViewport().FramebufferWidth == 2558 &&
			Session.GetRuntimeUIViewport().FramebufferHeight == 1442);
		const RuntimeUIDrawCommand* FramebufferScaledScore = FindScoreCommand();
		PF_CHECK(Tests, FramebufferScaledScore && FramebufferScaledScore->Bounds.Left == ResizedScoreBounds.Left &&
			FramebufferScaledScore->Bounds.Top == ResizedScoreBounds.Top &&
			FramebufferScaledScore->Bounds.Right == ResizedScoreBounds.Right &&
			FramebufferScaledScore->Bounds.Bottom == ResizedScoreBounds.Bottom);
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerPressed, { 840.0f, 58.0f }));
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerReleased, { 840.0f, 58.0f }));
		PF_CHECK(Tests, Session.IsPaused());
		const std::string PausedScore = FindScore();
		for (uint32_t Frame = 0; Frame < 30; ++Frame)
			PF_CHECK(Tests, Session.Advance(Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, FindScore() == PausedScore);
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerPressed, { 480.0f, 250.0f }));
		PF_CHECK(Tests, Pointer(RuntimeUIInputKind::PointerReleased, { 480.0f, 250.0f }));
		PF_CHECK(Tests, !Session.IsPaused());
		Session.Stop();
#endif
	}

	void TestBlueprintCookedProgramProduct(TestRunner& Tests)
	{
		CookFixture Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty() && !Fixture.BlueprintAsset.IsNil() && !Fixture.BlueprintSourcePath.empty());
		if (Fixture.Root.empty()) return;
		auto Snapshot = Fixture.Build();
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		auto Request = Fixture.Request(*Snapshot);
		Request.OutputRoot = Fixture.Root / "BlueprintCook";
		const auto Cooked = AssetCookService::Cook(Request);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;

		std::error_code RemoveError;
		PF_CHECK(Tests, std::filesystem::remove(Fixture.BlueprintSourcePath, RemoveError) && !RemoveError);
		auto Provider = CookedContentProvider::Open(Request.OutputRoot, Fixture.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		const CookedAssetEntry* Entry = (*Provider)->FindBlueprintProgram(Fixture.BlueprintAsset);
		PF_CHECK(Tests, Entry != nullptr && Entry->PayloadVersion == BlueprintAssetFormatVersion);
		auto Program = (*Provider)->LoadBlueprintProgram(Fixture.BlueprintAsset);
		PF_CHECK(Tests, Program && Program->GetEvents().size() == 2);
		if (Program && Program->GetEvents().size() == 2)
		{
			PF_CHECK(Tests, Program->GetEvents().front().Event == BlueprintEventKind::OnCreate);
			PF_CHECK(Tests, Program->GetEvents().front().Program.GetSource().Instructions[5].SourceNode == UUID(30, 6));
		}

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		PlayerSession Session(*Provider, RuntimeDescription);
		const auto Started = Session.Start();
		PF_CHECK(Tests, Started.has_value());
		if (Started && Session.GetActiveScene())
		{
			const auto RuntimeEntity = Session.GetActiveScene()->GetEntities().front();
			const auto RuntimeBlueprint = RuntimeEntity.GetBlueprint();
			PF_CHECK(Tests, RuntimeBlueprint && RuntimeBlueprint->has_value() &&
				RuntimeBlueprint->value().FindFloatProperty("health") == 42.0f);
			const auto Advanced = Session.Advance(Timestep(1.0 / 60.0));
			PF_CHECK(Tests, Advanced.has_value());
			const auto UpdatedBlueprint = RuntimeEntity.GetBlueprint();
			PF_CHECK(Tests, UpdatedBlueprint && UpdatedBlueprint->has_value() &&
				UpdatedBlueprint->value().FindFloatProperty("health") == 43.0f);
		}
		Session.Stop();
	}

	void TestSavedPbrGraphMaterialCookedProduct(TestRunner& Tests)
	{
#if defined(PF_COOK_FIXTURE_PROJECT)
		constexpr AssetID FloorMaterial{ 0x1000000000004000ull, 0x8000000000000008ull };
		const auto Unique = UUID::Generate();
		PF_CHECK(Tests, Unique.has_value());
		if (!Unique) return;
		std::error_code Error;
		const auto TemporaryRoot = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(Error), Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const auto SourceRoot = TemporaryRoot / std::filesystem::u8path("PulseForge-PbrGraphCook-" + Unique->ToString());
		const auto OutputRoot = TemporaryRoot / std::filesystem::u8path("PulseForge-PbrGraphCooked-" + Unique->ToString());
		struct OwnedRoots final
		{
			std::filesystem::path Source;
			std::filesystem::path Output;
			~OwnedRoots()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Source, CleanupError);
				CleanupError.clear();
				std::filesystem::remove_all(Output, CleanupError);
			}
		} Cleanup{ SourceRoot, OutputRoot };
		const auto SampleRoot = std::filesystem::path(PF_COOK_FIXTURE_PROJECT).parent_path().parent_path() / "PBRMaterials";
		std::filesystem::copy(SampleRoot, SourceRoot, std::filesystem::copy_options::recursive, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		auto ProjectValue = Project::Open(SourceRoot / "PBRMaterials.pfproj");
		PF_CHECK(Tests, ProjectValue.has_value());
		if (!ProjectValue) return;
		auto Snapshot = AssetDependencyCatalogBuilder::Build(
			ProjectValue->GetRootPath(), ProjectValue->GetDescription().ID, 1);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		const AssetID StartupScene = ProjectValue->GetDescription().StartScene.value_or(AssetID{});
		AssetCookRequest Request{
			*Snapshot, ProjectValue->GetFilePath(), StartupScene, {},
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
		const auto GraphProduct = std::find_if(Plan->Products.begin(), Plan->Products.end(), [FloorMaterial](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == FloorMaterial && Product.Product == CookedAssetProductKind::GraphMaterial; });
		PF_CHECK(Tests, GraphProduct != Plan->Products.end());
		const auto SceneProduct = std::find_if(Plan->Products.begin(), Plan->Products.end(), [StartupScene](const AssetCookPlannedProduct& Product)
			{ return Product.Asset == StartupScene && Product.Product == CookedAssetProductKind::Scene; });
		PF_CHECK(Tests, SceneProduct != Plan->Products.end());
		if (SceneProduct != Plan->Products.end())
			PF_CHECK(Tests, std::find(SceneProduct->RuntimeDependencies.begin(), SceneProduct->RuntimeDependencies.end(),
				CookedAssetDependency{ FloorMaterial, CookedAssetProductKind::GraphMaterial, {} }) != SceneProduct->RuntimeDependencies.end());
		const auto Cooked = AssetCookService::Cook(*Plan);
		PF_CHECK(Tests, Cooked.has_value());
		if (!Cooked) return;
		std::filesystem::remove_all(SourceRoot, Error);
		PF_CHECK(Tests, !Error && !std::filesystem::exists(SourceRoot));
		if (Error || std::filesystem::exists(SourceRoot)) return;
		auto Provider = CookedContentProvider::Open(OutputRoot, Request.Compatibility);
		PF_CHECK(Tests, Provider.has_value());
		if (!Provider) return;
		PF_CHECK(Tests, (*Provider)->FindGraphMaterial(FloorMaterial) != nullptr);
		const auto Loaded = (*Provider)->LoadGraphMaterialProduct(FloorMaterial);
		PF_CHECK(Tests, Loaded && Loaded->SpirV.size() >= 20 && Loaded->SpirV[0] == std::byte{ 0x03 } &&
			Loaded->SpirV[1] == std::byte{ 0x02 } && Loaded->SpirV[2] == std::byte{ 0x23 } && Loaded->SpirV[3] == std::byte{ 0x07 });
		MaterialAssetCache RuntimeMaterials(*Provider);
		PF_CHECK(Tests, RuntimeMaterials.IsRegistered(FloorMaterial));
		const auto RuntimeMaterial = RuntimeMaterials.GetOrLoad(FloorMaterial);
		const glm::vec4 ExpectedBaseColor{ 0.32f, 0.35f, 0.38f, 1.0f };
		PF_CHECK(Tests, RuntimeMaterial && RuntimeMaterial->get().BaseColorTexture == AssetID(0xa100000000004000ull,
			0x8000000000000001ull) && RuntimeMaterial->get().BaseColorFactor == ExpectedBaseColor);
#endif
	}
}

void RunMaterialGraphSourceTests(TestRunner& Tests)
{
	using namespace PulseForge;
	MaterialGraphSource Source;
	Source.GraphID = UUID(1, 1);
	Source.Nodes = {
		{ UUID(1, 2), MaterialGraphNodeKind::FloatConstant, 0.25f, {} },
		{ UUID(1, 3), MaterialGraphNodeKind::FloatConstant, 0.75f, {} },
		{ UUID(1, 4), MaterialGraphNodeKind::Add, 0.0f, {} },
		{ UUID(1, 5), MaterialGraphNodeKind::PbrOutput, 0.0f, {} }
	};
	Source.Connections = {
		{ UUID(2, 1), UUID(1, 2), UUID(1, 4), 0 },
		{ UUID(2, 2), UUID(1, 3), UUID(1, 4), 1 },
		{ UUID(2, 3), UUID(1, 4), UUID(1, 5), 0 }
	};
	PF_CHECK(Tests, ValidateMaterialGraphSource(Source).IsValid());
	auto UnsupportedNodeKind = Source;
	UnsupportedNodeKind.Nodes.front().Kind = static_cast<MaterialGraphNodeKind>(0xff);
	const auto UnsupportedNodeValidation = ValidateMaterialGraphSource(UnsupportedNodeKind);
	PF_CHECK(Tests, !UnsupportedNodeValidation.IsValid() &&
		std::any_of(UnsupportedNodeValidation.Diagnostics.begin(), UnsupportedNodeValidation.Diagnostics.end(),
			[](const MaterialGraphDiagnostic& Diagnostic)
			{ return Diagnostic.Node == UUID(1, 2) && Diagnostic.Message == "Node kind is not supported"; }));
	PF_CHECK(Tests, !SerializeMaterialGraphSource(UnsupportedNodeKind));
	const auto UnsupportedNodeCompile = MaterialGraphCompiler::Compile(MaterialGraphCompileRequest{
		UnsupportedNodeKind, {}, "diagnostic-fixture" });
	PF_CHECK(Tests, !UnsupportedNodeCompile && UnsupportedNodeCompile.error().Code == MaterialGraphCompileErrorCode::InvalidGraph);
	auto MissingOutputInput = Source;
	std::erase_if(MissingOutputInput.Connections, [](const MaterialGraphConnection& Connection)
		{ return Connection.ID == UUID(2, 3); });
	const auto InvalidGraphCompile = MaterialGraphCompiler::Compile(MaterialGraphCompileRequest{
		MissingOutputInput, {}, "diagnostic-fixture" });
	PF_CHECK(Tests, !InvalidGraphCompile && InvalidGraphCompile.error().Code == MaterialGraphCompileErrorCode::InvalidGraph &&
		std::any_of(InvalidGraphCompile.error().Diagnostics.begin(), InvalidGraphCompile.error().Diagnostics.end(),
			[](const MaterialGraphDiagnostic& Diagnostic)
			{ return Diagnostic.Node == UUID(1, 5) &&
				Diagnostic.Pin == GetMaterialGraphPinID(UUID(1, 5), false, 0) && !Diagnostic.Message.empty(); }));
	const auto Serialized = SerializeMaterialGraphSource(Source);
	PF_CHECK(Tests, Serialized.has_value());
	if (!Serialized) return;
	const auto Restored = DeserializeMaterialGraphSource(*Serialized);
	PF_CHECK(Tests, Restored.has_value());
	if (Restored) PF_CHECK(Tests, *Restored == Source);
	auto LegacySourceBytes = *Serialized;
	const size_t VersionValue = LegacySourceBytes.find("\"version\": 3");
	PF_CHECK(Tests, VersionValue != std::string::npos);
	if (VersionValue != std::string::npos) LegacySourceBytes.replace(VersionValue, 12, "\"version\": 1");
	const auto MigratedSource = DeserializeMaterialGraphSource(LegacySourceBytes);
	PF_CHECK(Tests, MigratedSource && MigratedSource->Version == MaterialGraphSource::CurrentVersion &&
		MigratedSource->Nodes.size() == Source.Nodes.size());
	auto VersionTwoSourceBytes = *Serialized;
	if (VersionValue != std::string::npos) VersionTwoSourceBytes.replace(VersionValue, 12, "\"version\": 2");
	const auto MigratedVersionTwoSource = DeserializeMaterialGraphSource(VersionTwoSourceBytes);
	PF_CHECK(Tests, MigratedVersionTwoSource && MigratedVersionTwoSource->Version == MaterialGraphSource::CurrentVersion);

	MaterialGraphSource ColorSource;
	ColorSource.GraphID = UUID(3, 1);
	ColorSource.Nodes = {
		{ UUID(3, 2), MaterialGraphNodeKind::ColorConstant, 0.0f, {}, 0.0, 0.0, { 0.2f, 0.4f, 0.6f } },
		{ UUID(3, 3), MaterialGraphNodeKind::PbrOutput, 0.0f, {} }
	};
	ColorSource.Connections = { { UUID(3, 4), UUID(3, 2), UUID(3, 3), 0 } };
	PF_CHECK(Tests, ValidateMaterialGraphSource(ColorSource).IsValid());
	const auto ColorIR = LowerMaterialGraphToIR(ColorSource);
	const std::array<float, 3> ExpectedColor{ 0.2f, 0.4f, 0.6f };
	PF_CHECK(Tests, ColorIR && ColorIR->Expressions[ColorIR->OutputExpression].Op == MaterialGraphExpressionOp::ColorConstant &&
		ColorIR->Expressions[ColorIR->OutputExpression].Color == ExpectedColor);
	const auto ColorSerialized = SerializeMaterialGraphSource(ColorSource);
	const auto ColorRestored = ColorSerialized ? DeserializeMaterialGraphSource(*ColorSerialized) :
		std::expected<MaterialGraphSource, std::string>(std::unexpected("serialization failed"));
	PF_CHECK(Tests, ColorRestored && *ColorRestored == ColorSource);
	MaterialGraphSource TextureSource;
	TextureSource.GraphID = UUID(4, 1);
	TextureSource.Nodes = {
		{ UUID(4, 2), MaterialGraphNodeKind::UV0, 0.0f, {}, 0.0, 0.0 },
		{ UUID(4, 3), MaterialGraphNodeKind::BaseColorTextureSample, 0.0f, {}, 220.0, 0.0 },
		{ UUID(4, 4), MaterialGraphNodeKind::PbrOutput, 0.0f, {}, 440.0, 0.0 }
	};
	TextureSource.Connections = {
		{ UUID(4, 5), UUID(4, 2), UUID(4, 3), 0 },
		{ UUID(4, 6), UUID(4, 3), UUID(4, 4), 0 }
	};
	PF_CHECK(Tests, ValidateMaterialGraphSource(TextureSource).IsValid());
	const auto TextureIR = LowerMaterialGraphToIR(TextureSource);
	PF_CHECK(Tests, TextureIR && TextureIR->Expressions.size() == 2 &&
		TextureIR->Expressions[TextureIR->OutputExpression].Op == MaterialGraphExpressionOp::BaseColorTextureSample &&
		TextureIR->Expressions[TextureIR->OutputExpression].Left == 0 &&
		TextureIR->Expressions.front().Op == MaterialGraphExpressionOp::UV0);
	const auto TextureSerialized = SerializeMaterialGraphSource(TextureSource);
	const auto TextureRestored = TextureSerialized ? DeserializeMaterialGraphSource(*TextureSerialized) :
		std::expected<MaterialGraphSource, std::string>(std::unexpected("serialization failed"));
	PF_CHECK(Tests, TextureRestored && *TextureRestored == TextureSource);
	auto PbrOutputs = Source;
	PbrOutputs.Connections.push_back({ UUID(2, 4), UUID(1, 2), UUID(1, 5), 1 });
	PbrOutputs.Connections.push_back({ UUID(2, 5), UUID(1, 3), UUID(1, 5), 2 });
	PF_CHECK(Tests, ValidateMaterialGraphSource(PbrOutputs).IsValid());
	const auto PbrOutputsSerialized = SerializeMaterialGraphSource(PbrOutputs);
	const auto PbrOutputsRestored = PbrOutputsSerialized ? DeserializeMaterialGraphSource(*PbrOutputsSerialized) :
		std::expected<MaterialGraphSource, std::string>(std::unexpected("serialization failed"));
	PF_CHECK(Tests, PbrOutputsRestored && *PbrOutputsRestored == PbrOutputs);
	const auto PbrOutputIR = LowerMaterialGraphToIR(PbrOutputs);
	PF_CHECK(Tests, PbrOutputIR && PbrOutputIR->MetallicExpression && PbrOutputIR->RoughnessExpression &&
		PbrOutputIR->Expressions[*PbrOutputIR->MetallicExpression].SourceNode == UUID(1, 2) &&
		PbrOutputIR->Expressions[*PbrOutputIR->RoughnessExpression].SourceNode == UUID(1, 3));
	auto InvalidMetallicType = PbrOutputs;
	InvalidMetallicType.Nodes.push_back({ UUID(1, 6), MaterialGraphNodeKind::ColorConstant, 0.0f, {} });
	InvalidMetallicType.Connections[3].OutputNode = UUID(1, 6);
	const auto InvalidPbrTypes = ValidateMaterialGraphSource(InvalidMetallicType);
	PF_CHECK(Tests, std::any_of(InvalidPbrTypes.Diagnostics.begin(), InvalidPbrTypes.Diagnostics.end(),
		[](const MaterialGraphDiagnostic& Diagnostic)
		{ return Diagnostic.Message == "PBR metallic and roughness inputs accept only scalar values" &&
			Diagnostic.Node == UUID(1, 5) && Diagnostic.Pin == GetMaterialGraphPinID(UUID(1, 5), false, 1); }));
	auto InvalidTextureCoordinates = TextureSource;
	InvalidTextureCoordinates.Nodes.front().Kind = MaterialGraphNodeKind::ColorConstant;
	const auto InvalidTextureTypes = ValidateMaterialGraphSource(InvalidTextureCoordinates);
	PF_CHECK(Tests, std::any_of(InvalidTextureTypes.Diagnostics.begin(), InvalidTextureTypes.Diagnostics.end(),
		[](const MaterialGraphDiagnostic& Diagnostic)
		{ return Diagnostic.Message == "Base-color texture sampling requires UV0 coordinates" &&
			Diagnostic.Node == UUID(4, 3) && Diagnostic.Pin == GetMaterialGraphPinID(UUID(4, 3), false, 0); }));
	auto InvalidUVOutput = TextureSource;
	InvalidUVOutput.Connections.back().OutputNode = UUID(4, 2);
	const auto InvalidUVTypes = ValidateMaterialGraphSource(InvalidUVOutput);
	PF_CHECK(Tests, std::any_of(InvalidUVTypes.Diagnostics.begin(), InvalidUVTypes.Diagnostics.end(),
		[](const MaterialGraphDiagnostic& Diagnostic)
		{ return Diagnostic.Message == "PBR base color accepts scalar or color values, not UV coordinates"; }));
	auto ScalarArithmeticGraph = Source;
	ScalarArithmeticGraph.Nodes.push_back({ UUID(3, 5), MaterialGraphNodeKind::ColorConstant, 0.0f, {}, 0.0, 0.0,
		{ 0.1f, 0.2f, 0.3f } });
	ScalarArithmeticGraph.Connections.push_back({ UUID(3, 6), UUID(3, 5), UUID(1, 4), 0 });
	const auto RejectedColorArithmetic = ValidateMaterialGraphSource(ScalarArithmeticGraph);
	PF_CHECK(Tests, std::any_of(RejectedColorArithmetic.Diagnostics.begin(), RejectedColorArithmetic.Diagnostics.end(),
		[](const MaterialGraphDiagnostic& Diagnostic)
		{ return Diagnostic.Message == "Scalar arithmetic inputs accept only scalar values"; }));
	const auto IR = LowerMaterialGraphToIR(Source);
	PF_CHECK(Tests, IR && IR->Expressions.size() == 3 && IR->OutputNode == UUID(1, 5));
	if (IR)
	{
		PF_CHECK(Tests, IR->Expressions[IR->OutputExpression].SourceNode == UUID(1, 4));
		PF_CHECK(Tests, IR->Expressions[IR->OutputExpression].Op == MaterialGraphExpressionOp::Add);
	}
	MaterialGraphCompileRequest CompileRequest;
	CompileRequest.Source = Source;
	CompileRequest.ToolchainFingerprint = GetMaterialGraphCompilerToolchainFingerprint();
	PF_CHECK(Tests, CompileRequest.ToolchainFingerprint.find("dxc-1.9.2602.24") != std::string::npos);
	const auto CacheID = UUID::Generate();
	PF_CHECK(Tests, CacheID.has_value());
	if (!CacheID) return;
	MaterialGraphCompileRequest ColorCompileRequest;
	ColorCompileRequest.Source = ColorSource;
	ColorCompileRequest.ToolchainFingerprint = CompileRequest.ToolchainFingerprint;
	const auto ColorCompiled = MaterialGraphCompiler::Compile(ColorCompileRequest);
	PF_CHECK(Tests, ColorCompiled && ColorCompiled->SemanticRecipe.find("float3 GraphMaterialBaseColor(float2 GraphUV0)") != std::string::npos &&
		ColorCompiled->SemanticRecipe.find("float3(") != std::string::npos);
	MaterialGraphCompileRequest TextureCompileRequest;
	TextureCompileRequest.Source = TextureSource;
	TextureCompileRequest.ToolchainFingerprint = CompileRequest.ToolchainFingerprint;
	const auto TextureCompiled = MaterialGraphCompiler::Compile(TextureCompileRequest);
	PF_CHECK(Tests, TextureCompiled && TextureCompiled->SemanticRecipe.find("PULSEFORGE_GRAPH_SAMPLES_BASE_COLOR_TEXTURE") != std::string::npos &&
		TextureCompiled->SemanticRecipe.find("DiffuseTexture.Sample(DiffuseSampler, e0).rgb") != std::string::npos &&
		TextureCompiled->SemanticRecipe.find("float3 GraphMaterialBaseColor(float2 GraphUV0)") != std::string::npos);
	MaterialGraphCompileRequest PbrOutputsCompileRequest;
	PbrOutputsCompileRequest.Source = PbrOutputs;
	PbrOutputsCompileRequest.ToolchainFingerprint = CompileRequest.ToolchainFingerprint;
	const auto PbrOutputsCompiled = MaterialGraphCompiler::Compile(PbrOutputsCompileRequest);
	PF_CHECK(Tests, PbrOutputsCompiled &&
		PbrOutputsCompiled->SemanticRecipe.find("PULSEFORGE_GRAPH_OVERRIDES_METALLIC") != std::string::npos &&
		PbrOutputsCompiled->SemanticRecipe.find("PULSEFORGE_GRAPH_OVERRIDES_ROUGHNESS") != std::string::npos &&
		PbrOutputsCompiled->SemanticRecipe.find("GraphMaterialMetallic(float2 GraphUV0)") != std::string::npos &&
		PbrOutputsCompiled->SemanticRecipe.find("GraphMaterialRoughness(float2 GraphUV0)") != std::string::npos);
	std::error_code CacheRootError;
	const auto CacheRoot = std::filesystem::canonical(std::filesystem::temp_directory_path(CacheRootError), CacheRootError) /
		("PF-MaterialGraphDDC-" + CacheID->ToString());
	PF_CHECK(Tests, !CacheRootError && CacheRoot.is_absolute());
	if (CacheRootError) return;
	struct MaterialGraphCacheCleanup final
	{
		std::filesystem::path Root;
		~MaterialGraphCacheCleanup() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
	} CacheCleanup{ CacheRoot };
	DerivedDataArtifactStore CompilerArtifacts({ CacheRoot, true, 8 * 1024 * 1024, 4 * 1024 * 1024, 16 });
	CompileRequest.ArtifactStore = &CompilerArtifacts;
	auto Compiled = MaterialGraphCompiler::Compile(CompileRequest);
	PF_CHECK(Tests, Compiled.has_value());
	if (Compiled)
	{
		PF_CHECK(Tests, Compiled->SourceNodes.size() == 3 &&
			std::find(Compiled->SourceNodes.begin(), Compiled->SourceNodes.end(), UUID(1, 4)) != Compiled->SourceNodes.end());
		PF_CHECK(Tests, Compiled->SpirV.size() >= 20);
		PF_CHECK(Tests, Compiled->SpirV[0] == std::byte{ 0x03 } && Compiled->SpirV[1] == std::byte{ 0x02 } &&
			Compiled->SpirV[2] == std::byte{ 0x23 } && Compiled->SpirV[3] == std::byte{ 0x07 });
		PF_CHECK(Tests, Compiled->RecipeFingerprint.size() == 32);
		auto Moved = Source;
		std::reverse(Moved.Nodes.begin(), Moved.Nodes.end());
		std::reverse(Moved.Connections.begin(), Moved.Connections.end());
		Moved.Nodes.front().X = 512.0;
		Moved.Nodes.front().Y = 144.0;
		const auto MovedSerialized = SerializeMaterialGraphSource(Moved);
		PF_CHECK(Tests, MovedSerialized && *MovedSerialized != *Serialized);
		auto Recompiled = MaterialGraphCompiler::Compile(MaterialGraphCompileRequest{
			Moved, {}, CompileRequest.ToolchainFingerprint });
		PF_CHECK(Tests, Recompiled && Recompiled->RecipeFingerprint == Compiled->RecipeFingerprint);
		auto Rerouted = Source;
		Rerouted.Nodes.push_back({ UUID(1, 6), MaterialGraphNodeKind::Reroute, 0.0f, {} });
		std::erase_if(Rerouted.Connections, [](const MaterialGraphConnection& Connection)
			{ return Connection.ID == UUID(2, 1); });
		Rerouted.Connections.push_back({ UUID(2, 8), UUID(1, 2), UUID(1, 6), 0 });
		Rerouted.Connections.push_back({ UUID(2, 9), UUID(1, 6), UUID(1, 4), 0 });
		const auto ReroutedIR = LowerMaterialGraphToIR(Rerouted);
		PF_CHECK(Tests, ReroutedIR && ReroutedIR->Expressions.size() == IR->Expressions.size());
		MaterialGraphCompileRequest ReroutedRequest = CompileRequest;
		ReroutedRequest.Source = Rerouted;
		const auto ReroutedCompile = MaterialGraphCompiler::Compile(ReroutedRequest);
		PF_CHECK(Tests, ReroutedCompile && ReroutedCompile->RecipeFingerprint == Compiled->RecipeFingerprint);
		auto SemanticEdit = Source;
		SemanticEdit.Nodes[0].Value += 0.125f;
		auto SemanticRecompiled = MaterialGraphCompiler::Compile(MaterialGraphCompileRequest{
			SemanticEdit, {}, CompileRequest.ToolchainFingerprint });
		PF_CHECK(Tests, SemanticRecompiled && SemanticRecompiled->RecipeFingerprint != Compiled->RecipeFingerprint);
		MaterialGraphCompileRequest CachedCompile = CompileRequest;
		CachedCompile.CompilerExecutable = "missing-after-first-compile.exe";
		auto CacheHit = MaterialGraphCompiler::Compile(CachedCompile);
		PF_CHECK(Tests, CacheHit && CacheHit->SpirV == Compiled->SpirV &&
			CacheHit->RecipeFingerprint == Compiled->RecipeFingerprint);
		int CachedGenerationChecks = 0;
		CachedCompile.IsGenerationCurrent = [&CachedGenerationChecks](uint64_t)
			{ return ++CachedGenerationChecks == 1; };
		const auto StaleCacheHit = MaterialGraphCompiler::Compile(CachedCompile);
		PF_CHECK(Tests, !StaleCacheHit && StaleCacheHit.error().Code == MaterialGraphCompileErrorCode::StaleGeneration &&
			CachedGenerationChecks == 2);
		auto ParameterGraph = Source;
		ParameterGraph.Nodes[0].Kind = MaterialGraphNodeKind::FloatParameter;
		ParameterGraph.Nodes[0].ParameterName = "BaseTint";
		MaterialGraphCompileRequest ParameterRequest{ ParameterGraph, {}, CompileRequest.ToolchainFingerprint };
		ParameterRequest.ArtifactStore = &CompilerArtifacts;
		auto ParameterCompiled = MaterialGraphCompiler::Compile(ParameterRequest);
		PF_CHECK(Tests, ParameterCompiled.has_value());
		if (ParameterCompiled)
		{
			ParameterGraph.Nodes[0].Value += 0.2f;
			ParameterRequest.Source = ParameterGraph;
			ParameterRequest.CompilerExecutable = "missing-after-parameter-edit.exe";
			auto ParameterValueChanged = MaterialGraphCompiler::Compile(ParameterRequest);
			PF_CHECK(Tests, ParameterValueChanged &&
				ParameterValueChanged->RecipeFingerprint == ParameterCompiled->RecipeFingerprint);
			PF_CHECK(Tests, ParameterValueChanged && ParameterValueChanged->Parameters.size() == 1 &&
				std::abs(ParameterValueChanged->Parameters[0].DefaultValue - 0.45f) < 0.0001f);
		}
	}
	MaterialGraphCompileRequest MissingCompiler;
	MissingCompiler.Source = Source;
	MissingCompiler.CompilerExecutable = "missing-dxc.exe";
	MissingCompiler.ToolchainFingerprint = "test";
	const auto Missing = MaterialGraphCompiler::Compile(MissingCompiler);
	PF_CHECK(Tests, !Missing && Missing.error().Code == MaterialGraphCompileErrorCode::CompilerUnavailable);
	MaterialGraphCompileRequest UnsupportedTarget = MissingCompiler;
	UnsupportedTarget.Target = MaterialGraphCompileTarget::Direct3DDXIL;
	const auto Unsupported = MaterialGraphCompiler::Compile(UnsupportedTarget);
	PF_CHECK(Tests, !Unsupported && Unsupported.error().Code == MaterialGraphCompileErrorCode::UnsupportedTarget);
	MaterialGraphCompileRequest StaleCompile;
	StaleCompile.Source = Source;
	StaleCompile.ToolchainFingerprint = "test";
	StaleCompile.IsGenerationCurrent = [](uint64_t) { return false; };
	const auto Stale = MaterialGraphCompiler::Compile(StaleCompile);
	PF_CHECK(Tests, !Stale && Stale.error().Code == MaterialGraphCompileErrorCode::StaleGeneration);
	std::stop_source CancelBeforeLaunch;
	CancelBeforeLaunch.request_stop();
	MaterialGraphCompileRequest CancelledCompile;
	CancelledCompile.Source = Source;
	CancelledCompile.ToolchainFingerprint = "test";
	CancelledCompile.CancellationToken = CancelBeforeLaunch.get_token();
	const auto Cancelled = MaterialGraphCompiler::Compile(CancelledCompile);
	PF_CHECK(Tests, !Cancelled && Cancelled.error().Code == MaterialGraphCompileErrorCode::Cancelled);
#if defined(PF_MATERIAL_GRAPH_COMPILER_FIXTURE)
	MaterialGraphCompileRequest VulkanBindingFlags;
	VulkanBindingFlags.Source = Source;
	VulkanBindingFlags.Source.Nodes[0].Value = 779.0f;
	VulkanBindingFlags.CompilerExecutable = PF_MATERIAL_GRAPH_COMPILER_FIXTURE;
	VulkanBindingFlags.ToolchainFingerprint = "process-fixture";
	const auto VerifiedVulkanBindingFlags = MaterialGraphCompiler::Compile(VulkanBindingFlags);
	PF_CHECK(Tests, VerifiedVulkanBindingFlags.has_value());
	MaterialGraphCompileRequest ProcessFailure;
	ProcessFailure.Source = Source;
	ProcessFailure.Source.Nodes[0].Value = 777.0f;
	ProcessFailure.CompilerExecutable = PF_MATERIAL_GRAPH_COMPILER_FIXTURE;
	ProcessFailure.ToolchainFingerprint = "process-fixture";
	const auto Crashed = MaterialGraphCompiler::Compile(ProcessFailure);
	PF_CHECK(Tests, !Crashed && Crashed.error().Code == MaterialGraphCompileErrorCode::CompilerFailed);
	MaterialGraphCompileRequest PartialOutput = ProcessFailure;
	PartialOutput.Source.Nodes[0].Value = 0.5f;
	const auto InvalidOutput = MaterialGraphCompiler::Compile(PartialOutput);
	PF_CHECK(Tests, !InvalidOutput && InvalidOutput.error().Code == MaterialGraphCompileErrorCode::OutputInvalid);
	MaterialGraphCompileRequest MissingParameterBinding = ProcessFailure;
	MissingParameterBinding.Source.Nodes[0].Kind = MaterialGraphNodeKind::FloatParameter;
	MissingParameterBinding.Source.Nodes[0].ParameterName = "MissingBinding";
	MissingParameterBinding.Source.Nodes[0].Value = 0.25f;
	const auto MissingBindingOutput = MaterialGraphCompiler::Compile(MissingParameterBinding);
	PF_CHECK(Tests, !MissingBindingOutput &&
		MissingBindingOutput.error().Code == MaterialGraphCompileErrorCode::OutputInvalid);
	MaterialGraphCompileRequest ProcessTimeout = ProcessFailure;
	ProcessTimeout.Source.Nodes[0].Value = 778.0f;
	ProcessTimeout.Timeout = std::chrono::milliseconds(100);
	const auto TimedOut = MaterialGraphCompiler::Compile(ProcessTimeout);
	PF_CHECK(Tests, !TimedOut && TimedOut.error().Code == MaterialGraphCompileErrorCode::TimedOut);
	MaterialGraphCompileRequest StaleDuringProcess = ProcessTimeout;
	StaleDuringProcess.Timeout = std::chrono::seconds(2);
	std::atomic_size_t GenerationChecks = 0;
	StaleDuringProcess.IsGenerationCurrent = [&](uint64_t) { return ++GenerationChecks == 1; };
	const auto StaleChild = MaterialGraphCompiler::Compile(StaleDuringProcess);
	PF_CHECK(Tests, !StaleChild && StaleChild.error().Code == MaterialGraphCompileErrorCode::StaleGeneration);
	MaterialGraphCompileRequest CancelDuringProcess = ProcessTimeout;
	CancelDuringProcess.Timeout = std::chrono::seconds(2);
	std::stop_source CancelRunning;
	CancelDuringProcess.CancellationToken = CancelRunning.get_token();
	std::jthread CancelTimer([&](std::stop_token Stop)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		if (!Stop.stop_requested()) CancelRunning.request_stop();
	});
	const auto CancelledChild = MaterialGraphCompiler::Compile(CancelDuringProcess);
	CancelTimer.request_stop();
	PF_CHECK(Tests, !CancelledChild && CancelledChild.error().Code == MaterialGraphCompileErrorCode::Cancelled);
#endif
	MaterialAssetDesc Material;
	Material.Graph = Source;
	const auto MaterialBytes = MaterialAssetSerializer::Serialize(Material);
	PF_CHECK(Tests, MaterialBytes.has_value());
	if (MaterialBytes)
	{
		const auto MaterialRoundTrip = MaterialAssetSerializer::Deserialize(*MaterialBytes);
		PF_CHECK(Tests, MaterialRoundTrip.has_value());
		if (MaterialRoundTrip) PF_CHECK(Tests, MaterialRoundTrip->Graph == Material.Graph);
		PF_CHECK(Tests, MaterialBytes->find("\"version\": 5") != std::string::npos);
	}
	const auto FixedMaterial = MaterialAssetSerializer::Serialize(MaterialAssetDesc{});
	PF_CHECK(Tests, FixedMaterial && FixedMaterial->find("\"version\": 4") != std::string::npos);
	if (Compiled)
	{
		CookedGraphMaterialProduct Product;
		Product.RecipeFingerprint = Compiled->RecipeFingerprint;
		Product.Material.BaseColorFactor = { 0.25f, 0.5f, 0.75f, 0.6f };
		Product.Material.MetallicFactor = 0.35f;
		Product.Material.RoughnessFactor = 0.42f;
		Product.Material.AlphaMode = CookedMaterialAlphaMode::Mask;
		Product.Material.AlphaCutoff = 0.31f;
		Product.Material.DoubleSided = true;
		for (const auto& Parameter : Compiled->Parameters)
			Product.Parameters.push_back({ Parameter.ID, Parameter.Name, Parameter.DefaultValue });
		Product.SpirV = Compiled->SpirV;
		auto ProductBytes = Detail::EncodeGraphMaterialProduct(Product);
		PF_CHECK(Tests, ProductBytes.has_value());
		if (ProductBytes)
		{
			auto ProductReadBack = Detail::DecodeGraphMaterialProduct(*ProductBytes);
			PF_CHECK(Tests, ProductReadBack && ProductReadBack->RecipeFingerprint == Product.RecipeFingerprint &&
				ProductReadBack->SpirV == Product.SpirV && ProductReadBack->Parameters == Product.Parameters &&
				ProductReadBack->Material.BaseColorFactor == Product.Material.BaseColorFactor &&
				ProductReadBack->Material.MetallicFactor == Product.Material.MetallicFactor &&
				ProductReadBack->Material.RoughnessFactor == Product.Material.RoughnessFactor &&
				ProductReadBack->Material.AlphaMode == Product.Material.AlphaMode &&
				ProductReadBack->Material.AlphaCutoff == Product.Material.AlphaCutoff &&
				ProductReadBack->Material.DoubleSided == Product.Material.DoubleSided);
			if (ProductReadBack)
			{
				size_t MaterialStateOffset = 48;
				for (const auto& Parameter : Product.Parameters)
					MaterialStateOffset += 16 + sizeof(uint32_t) + Parameter.Name.size() + sizeof(float);
				uint32_t MaterialStateBytes = 0;
				if (MaterialStateOffset + sizeof(uint32_t) <= ProductBytes->size())
					std::memcpy(&MaterialStateBytes, ProductBytes->data() + MaterialStateOffset, sizeof(MaterialStateBytes));
				auto LegacyGraphProduct = *ProductBytes;
				if (MaterialStateBytes > 0 && MaterialStateOffset + sizeof(uint32_t) + MaterialStateBytes <= LegacyGraphProduct.size())
				{
					LegacyGraphProduct.erase(LegacyGraphProduct.begin() + static_cast<std::ptrdiff_t>(MaterialStateOffset),
						LegacyGraphProduct.begin() + static_cast<std::ptrdiff_t>(MaterialStateOffset + sizeof(uint32_t) + MaterialStateBytes));
					LegacyGraphProduct[4] = std::byte{ 1 };
					const auto LegacyProductReadBack = Detail::DecodeGraphMaterialProduct(LegacyGraphProduct);
					PF_CHECK(Tests, LegacyProductReadBack && LegacyProductReadBack->ArtifactVersion == Detail::GraphMaterialProductSchemaVersion &&
						LegacyProductReadBack->Material.BaseColorFactor == CookedMaterialProduct{}.BaseColorFactor);
				}
				else PF_CHECK(Tests, false);
			}
			auto FutureProduct = *ProductBytes;
			FutureProduct[4] = std::byte{ 3 };
			PF_CHECK(Tests, !Detail::DecodeGraphMaterialProduct(FutureProduct));
			ProductBytes->front() ^= std::byte{ 1 };
			PF_CHECK(Tests, !Detail::DecodeGraphMaterialProduct(*ProductBytes));
		}
	}
	{
		CookedGraphMaterialProduct MaximumBoundedProduct;
		MaximumBoundedProduct.RecipeFingerprint = std::string(32, 'a');
		MaximumBoundedProduct.SpirV.resize(16u * 1024u * 1024u);
		MaximumBoundedProduct.SpirV[0] = std::byte{ 0x03 };
		MaximumBoundedProduct.SpirV[1] = std::byte{ 0x02 };
		MaximumBoundedProduct.SpirV[2] = std::byte{ 0x23 };
		MaximumBoundedProduct.SpirV[3] = std::byte{ 0x07 };
		for (uint64_t Index = 0; Index < 256; ++Index)
		{
			std::string Name = "Parameter_" + std::to_string(Index);
			Name.resize(64, '_');
			MaximumBoundedProduct.Parameters.push_back({ UUID(8, Index + 1), std::move(Name), 0.0f });
		}
		auto MaximumBytes = Detail::EncodeGraphMaterialProduct(MaximumBoundedProduct);
		PF_CHECK(Tests, MaximumBytes.has_value());
		if (MaximumBytes)
		{
			const auto MaximumRoundTrip = Detail::DecodeGraphMaterialProduct(*MaximumBytes);
			PF_CHECK(Tests, MaximumRoundTrip && MaximumRoundTrip->Parameters == MaximumBoundedProduct.Parameters &&
				MaximumRoundTrip->SpirV == MaximumBoundedProduct.SpirV &&
				MaximumRoundTrip->Material.BaseColorFactor == MaximumBoundedProduct.Material.BaseColorFactor);
		}
	}
	auto Cyclic = Source;
	Cyclic.Connections.push_back({ UUID(2, 4), UUID(1, 4), UUID(1, 2), 0 });
	PF_CHECK(Tests, !ValidateMaterialGraphSource(Cyclic).IsValid());
	auto InvalidPresentation = Source;
	InvalidPresentation.Nodes.front().X = std::numeric_limits<double>::infinity();
	PF_CHECK(Tests, !ValidateMaterialGraphSource(InvalidPresentation).IsValid());
	const auto UnsupportedVersion = Serialized->find("\"version\": 3");
	PF_CHECK(Tests, UnsupportedVersion != std::string::npos);
	if (UnsupportedVersion != std::string::npos)
	{
		auto Future = *Serialized;
		Future.replace(UnsupportedVersion, std::string("\"version\": 1").size(), "\"version\": 99");
		PF_CHECK(Tests, !DeserializeMaterialGraphSource(Future).has_value());
	}
}

void RunAssetCookServiceTests(TestRunner& Tests)
{
	RunMaterialGraphSourceTests(Tests);
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
	auto CookedScene = (*Provider)->LoadScene(Fixture.SceneAsset);
	PF_CHECK(Tests, CookedScene.has_value());
	if (CookedScene) PF_CHECK(Tests, (*CookedScene)->GetEntities().size() == 1);
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
	auto GraphMaterialRequest = Fixture.Request(*Snapshot);
	GraphMaterialRequest.DynamicRoots = { Fixture.GraphMaterialAsset, Fixture.ColorGraphMaterialAsset };
	GraphMaterialRequest.OutputRoot = Fixture.Root / "GraphMaterialCook";
	const auto GraphMaterialCook = RunCookWithBoundary("saved-graph-material-cook", [&]
		{ return AssetCookService::Cook(GraphMaterialRequest); });
	PF_CHECK(Tests, GraphMaterialCook.has_value());
	if (GraphMaterialCook)
	{
		auto GraphProvider = CookedContentProvider::Open(GraphMaterialRequest.OutputRoot, Fixture.Compatibility);
		PF_CHECK(Tests, GraphProvider.has_value());
		if (GraphProvider)
		{
			PF_CHECK(Tests, (*GraphProvider)->FindGraphMaterial(Fixture.GraphMaterialAsset) != nullptr);
			PF_CHECK(Tests, (*GraphProvider)->FindGraphMaterial(Fixture.ColorGraphMaterialAsset) != nullptr);
			PF_CHECK(Tests, (*GraphProvider)->FindTexture(Fixture.ColorGraphTextureAsset) != nullptr);
			auto CookedGraph = (*GraphProvider)->LoadGraphMaterialProduct(Fixture.GraphMaterialAsset);
			auto CookedColorGraph = (*GraphProvider)->LoadGraphMaterialProduct(Fixture.ColorGraphMaterialAsset);
			auto CookedColorRenderState = (*GraphProvider)->LoadMaterialProduct(Fixture.ColorGraphMaterialAsset);
			PF_CHECK(Tests, CookedGraph.has_value());
			PF_CHECK(Tests, CookedColorGraph && CookedColorGraph->RecipeFingerprint.size() == 32 &&
				CookedColorGraph->SpirV.size() >= 20 && CookedColorGraph->Parameters.empty());
			const std::array<float, 4> ExpectedColorMaterial{ 0.25f, 0.5f, 0.75f, 0.6f };
			PF_CHECK(Tests, CookedColorRenderState && CookedColorRenderState->BaseColorFactor == ExpectedColorMaterial &&
				CookedColorRenderState->MetallicFactor == 0.35f && CookedColorRenderState->RoughnessFactor == 0.42f &&
				CookedColorRenderState->AlphaMode == CookedMaterialAlphaMode::Mask &&
				CookedColorRenderState->AlphaCutoff == 0.31f && CookedColorRenderState->DoubleSided);
			MaterialAssetCache RuntimeMaterials(*GraphProvider);
			const auto CachedGraphMaterial = RuntimeMaterials.GetOrLoad(Fixture.ColorGraphMaterialAsset);
			PF_CHECK(Tests, CachedGraphMaterial &&
				CachedGraphMaterial->get().BaseColorTexture == Fixture.ColorGraphTextureAsset &&
				CachedGraphMaterial->get().BaseColorFactor.r == ExpectedColorMaterial[0] &&
				CachedGraphMaterial->get().BaseColorFactor.g == ExpectedColorMaterial[1] &&
				CachedGraphMaterial->get().BaseColorFactor.b == ExpectedColorMaterial[2] &&
				CachedGraphMaterial->get().BaseColorFactor.a == ExpectedColorMaterial[3] &&
				CachedGraphMaterial->get().MetallicFactor == 0.35f && CachedGraphMaterial->get().RoughnessFactor == 0.42f &&
				CachedGraphMaterial->get().AlphaMode == MaterialAlphaMode::Mask &&
				CachedGraphMaterial->get().AlphaCutoff == 0.31f && CachedGraphMaterial->get().DoubleSided);
			if (CookedGraph)
			{
				PF_CHECK(Tests, CookedGraph->RecipeFingerprint.size() == 32);
				PF_CHECK(Tests, CookedGraph->SpirV.size() >= 20);
				PF_CHECK(Tests, CookedGraph->Parameters.size() == 1 &&
					CookedGraph->Parameters[0].ID == UUID(10, 3) && CookedGraph->Parameters[0].Name == "Accent" &&
					std::abs(CookedGraph->Parameters[0].DefaultValue - 0.1f) < 0.0001f);
			}
			PF_CHECK(Tests, !(*GraphProvider)->FindMaterial(Fixture.GraphMaterialAsset));
		}
#ifdef PF_GRAPH_MATERIAL_COOK_TEST_OUTPUT
		std::error_code RuntimeFixtureError;
		const std::filesystem::path RuntimeFixtureRoot = std::filesystem::u8path(PF_GRAPH_MATERIAL_COOK_TEST_OUTPUT);
		std::filesystem::remove_all(RuntimeFixtureRoot, RuntimeFixtureError);
		if (!RuntimeFixtureError)
			std::filesystem::copy(GraphMaterialRequest.OutputRoot, RuntimeFixtureRoot,
				std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
				RuntimeFixtureError);
		if (!RuntimeFixtureError)
		{
			std::ofstream AssetIdentity(RuntimeFixtureRoot / "graph-material-asset-id.txt", std::ios::binary | std::ios::trunc);
			AssetIdentity << Fixture.GraphMaterialAsset.ToString();
			if (!AssetIdentity) RuntimeFixtureError = std::make_error_code(std::errc::io_error);
		}
		PF_CHECK(Tests, !RuntimeFixtureError);
#endif
	}
	if (GraphMaterialCook)
	{
		const std::filesystem::path GraphSourcePath = Fixture.Source / "Assets" / "Graph.material";
		std::ifstream SourceStream(GraphSourcePath, std::ios::binary);
		const std::string ValidSource{ std::istreambuf_iterator<char>(SourceStream), std::istreambuf_iterator<char>() };
		PF_CHECK(Tests, !ValidSource.empty());
		std::ifstream ManifestStream(GraphMaterialCook->ManifestPath, std::ios::binary);
		const std::string LastGoodManifest{ std::istreambuf_iterator<char>(ManifestStream), std::istreambuf_iterator<char>() };
		PF_CHECK(Tests, !LastGoodManifest.empty());
		auto InvalidSource = ValidSource;
		const std::string ExistingNodeID = UUID(10, 4).ToString();
		const size_t NodeDeclaration = InvalidSource.find(ExistingNodeID);
		PF_CHECK(Tests, NodeDeclaration != std::string::npos);
		if (NodeDeclaration != std::string::npos)
		{
			InvalidSource.replace(NodeDeclaration, ExistingNodeID.size(), UUID(99, 999).ToString());
			CookFixture::WriteText(GraphSourcePath, InvalidSource);
			const auto InvalidSnapshot = Fixture.Build();
			CookFixture::WriteText(GraphSourcePath, ValidSource);
			PF_CHECK(Tests, !InvalidSnapshot && InvalidSnapshot.error().Message.find("Material dependency analysis failed") != std::string::npos);
			std::ifstream RetainedManifestStream(GraphMaterialCook->ManifestPath, std::ios::binary);
			const std::string RetainedManifest{ std::istreambuf_iterator<char>(RetainedManifestStream), std::istreambuf_iterator<char>() };
			PF_CHECK(Tests, RetainedManifest == LastGoodManifest);
			auto LastGoodProvider = CookedContentProvider::Open(GraphMaterialRequest.OutputRoot, Fixture.Compatibility);
			PF_CHECK(Tests, LastGoodProvider.has_value());
			if (LastGoodProvider)
			{
				const auto LastGoodProduct = (*LastGoodProvider)->LoadGraphMaterialProduct(Fixture.GraphMaterialAsset);
				PF_CHECK(Tests, LastGoodProduct && LastGoodProduct->RecipeFingerprint.size() == 32);
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
	TestSkeletalMeshCookedProviderRoundTrip(Tests);
	TestLuaAuthoringSampleCookAndRuntime(Tests);
	TestPhysicsQueriesSampleCookedPlayer(Tests);
	TestRuntimeUISampleCookedPlayer(Tests);
	TestBlueprintCookedProgramProduct(Tests);
	TestSavedPbrGraphMaterialCookedProduct(Tests);
}
