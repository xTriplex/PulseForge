#include "TestEditorSupport.h"
#include "Assets/MaterialAsset.h"
#include "Assets/AssetMetadata.h"
#include "Assets/AssetPreparationService.h"
#include "Assets/AssetRegistry.h"
#include "Assets/DerivedDataArtifactStore.h"
#include "Editor/EditorAssetPreviewService.h"
#include "Editor/EditorViewportSceneRenderView.h"
#include "Renderer/AssetCache/TextureAssetCache.h"
#include "Renderer/EnvironmentLightingCache.h"

#include <chrono>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace PulseForgeEditor
{
	struct EditorAssetPreviewServiceTestAccess
	{
		static void ThrowForOneAsset(EditorAssetPreviewService& Service) { Service.m_DecodeOverrideForTesting = &DecodeWithFailure; }

	private:
		static std::optional<EditorAssetPreviewService::CpuPreview> DecodeWithFailure(const EditorAssetPreviewService::Request& Work)
		{
			if (Work.AssetID == "decode-throws") throw std::runtime_error("injected preview decode failure");
			return EditorAssetPreviewService::Decode(Work);
		}
	};
} // namespace PulseForgeEditor

namespace
{
	struct FakeTextureLifetime final
	{
		bool Alive = true;
	};

	class FakeTexture final : public PulseForge::Texture
	{
	public:
		FakeTexture(PulseForge::TextureDesc Description, std::shared_ptr<FakeTextureLifetime> Lifetime)
			: m_Description(std::move(Description)), m_Lifetime(std::move(Lifetime))
		{
		}

		~FakeTexture() override { m_Lifetime->Alive = false; }
		[[nodiscard]] const PulseForge::TextureDesc& GetDescription() const noexcept override { return m_Description; }
		[[nodiscard]] const std::shared_ptr<FakeTextureLifetime>& GetLifetime() const noexcept { return m_Lifetime; }

	private:
		PulseForge::TextureDesc m_Description;
		std::shared_ptr<FakeTextureLifetime> m_Lifetime;
	};

	class FakeRenderTarget final : public PulseForge::RenderTarget
	{
	public:
		FakeRenderTarget(PulseForge::RenderTargetDesc Description, std::unique_ptr<FakeTexture> ColorTexture)
			: m_Description(std::move(Description)), m_ColorTexture(std::move(ColorTexture))
		{
		}

		[[nodiscard]] const PulseForge::RenderTargetDesc& GetDescription() const noexcept override { return m_Description; }
		[[nodiscard]] const PulseForge::Texture* GetColorTexture() const noexcept override { return m_ColorTexture.get(); }
		[[nodiscard]] const PulseForge::Texture* GetDepthTexture() const noexcept override { return nullptr; }

	private:
		PulseForge::RenderTargetDesc m_Description;
		std::unique_ptr<FakeTexture> m_ColorTexture;
	};

	class FakeRenderDevice final : public PulseForge::RenderDevice
	{
	public:
		[[nodiscard]] std::pair<unsigned int, unsigned int> GetOutputFramebufferSize() const noexcept override { return { 1280, 720 }; }
		[[nodiscard]] PulseForge::OutputColorEncoding GetOutputColorEncoding() const noexcept override
		{
			return PulseForge::OutputColorEncoding::SrgbAttachment;
		}
		[[nodiscard]] PulseForge::RenderTargetCreateResult CreateRenderTarget(const PulseForge::RenderTargetDesc& Description) override
		{
			++RenderTargetCreations;
			if (FailNextRenderTarget)
			{
				FailNextRenderTarget = false;
				return std::unexpected(
					PulseForge::RenderTargetError{ PulseForge::RenderTargetErrorCode::BackendFailure, "injected target failure" });
			}

			std::unique_ptr<FakeTexture> ColorTexture;
			if (!NextTargetWithoutColor)
			{
				PulseForge::TextureDesc TextureDescription;
				TextureDescription.Width = Description.Width;
				TextureDescription.Height = Description.Height;
				TextureDescription.Format = PulseForge::TextureFormat::RGBA8_Srgb;
				TextureDescription.DebugName = Description.DebugName;
				LastTextureLifetime = std::make_shared<FakeTextureLifetime>();
				ColorTexture = std::make_unique<FakeTexture>(std::move(TextureDescription), LastTextureLifetime);
			}
			NextTargetWithoutColor = false;
			return PulseForge::RenderTargetHandle(std::make_unique<FakeRenderTarget>(Description, std::move(ColorTexture)));
		}
		[[nodiscard]] PulseForge::DepthCubemapCreateResult CreateDepthCubemap(uint32_t, const std::string&) override
		{
			return std::unexpected(PulseForge::DepthCubemapError{ PulseForge::DepthCubemapErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsResult BeginRenderTarget(
			const PulseForge::RenderTarget&, const PulseForge::RenderTargetClearValue&) override
		{
			++ClearBegins;
			if (FailNextClearBegin)
			{
				FailNextClearBegin = false;
				return std::unexpected(
					PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "injected begin failure" });
			}
			return {};
		}
		[[nodiscard]] PulseForge::GraphicsResult EndRenderTarget() override
		{
			++ClearEnds;
			if (FailNextClearEnd)
			{
				FailNextClearEnd = false;
				return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "injected end failure" });
			}
			return {};
		}
		[[nodiscard]] PulseForge::BufferCreateResult CreateBuffer(const PulseForge::BufferDesc&, std::span<const std::byte>) override
		{
			return std::unexpected(PulseForge::BufferCreateError{ PulseForge::BufferCreateErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::BufferUpdateResult WriteBuffer(const PulseForge::Buffer&, uint64_t, std::span<const std::byte>) override
		{
			return std::unexpected(PulseForge::BufferUpdateError{ PulseForge::BufferUpdateErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::TextureCreateResult CreateTexture(
			const PulseForge::TextureDesc& Description, std::span<const PulseForge::TextureSubresourceData>) override
		{
			++TextureCreations;
			if (TextureCreationFailuresRemaining > 0)
			{
				--TextureCreationFailuresRemaining;
				return std::unexpected(PulseForge::TextureError{
					PulseForge::TextureErrorCode::BackendFailure, "injected texture creation failure" });
			}
			LastTextureLifetime = std::make_shared<FakeTextureLifetime>();
			return PulseForge::TextureHandle(std::make_unique<FakeTexture>(Description, LastTextureLifetime));
		}
		[[nodiscard]] PulseForge::SamplerCreateResult CreateSampler(const PulseForge::SamplerDesc&) override
		{
			return std::unexpected(PulseForge::TextureError{ PulseForge::TextureErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::BindingLayoutCreateResult CreateBindingLayout(const PulseForge::BindingLayoutDesc&) override
		{
			return std::unexpected(PulseForge::BindingError{ PulseForge::BindingErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::BindingSetCreateResult CreateBindingSet(const PulseForge::BindingSetDesc&) override
		{
			return std::unexpected(PulseForge::BindingError{ PulseForge::BindingErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::ShaderCreateResult CreateShader(const PulseForge::ShaderDesc&, std::span<const std::byte>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsPipelineCreateResult CreateGraphicsPipeline(const PulseForge::GraphicsPipelineDesc&) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsResult Draw(const PulseForge::GraphicsPipeline&, const PulseForge::Buffer&,
			const PulseForge::DrawArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsResult Draw(const PulseForge::GraphicsPipeline&, const PulseForge::Mesh&,
			const PulseForge::DrawArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsResult DrawIndexed(const PulseForge::GraphicsPipeline&, const PulseForge::Buffer&,
			const PulseForge::Buffer&, const PulseForge::DrawIndexedArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}
		[[nodiscard]] PulseForge::GraphicsResult DrawIndexed(const PulseForge::GraphicsPipeline&, const PulseForge::Mesh&,
			const PulseForge::DrawIndexedArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::BackendFailure, "unused" });
		}

		bool FailNextRenderTarget = false;
		bool NextTargetWithoutColor = false;
		bool FailNextClearBegin = false;
		bool FailNextClearEnd = false;
		size_t TextureCreationFailuresRemaining = 0;
		size_t RenderTargetCreations = 0;
		size_t TextureCreations = 0;
		size_t ClearBegins = 0;
		size_t ClearEnds = 0;
		std::shared_ptr<FakeTextureLifetime> LastTextureLifetime;
	};

	class FakeTextureRegistry final : public PulseForgeEditor::EditorTextureRegistry
	{
	public:
		[[nodiscard]] std::expected<uint64_t, std::string> RegisterTexture(const PulseForge::Texture& Texture) override
		{
			++RegistrationAttempts;
			const auto* Fake = dynamic_cast<const FakeTexture*>(&Texture);
			if (!Fake) return std::unexpected("unexpected texture implementation");
			if (FailNextRegistration)
			{
				FailNextRegistration = false;
				return std::unexpected("injected registration failure");
			}
			const uint64_t ID = NextID++;
			m_Textures.emplace(ID, Fake->GetLifetime());
			return ID;
		}

		void UnregisterTexture(uint64_t TextureID) noexcept override
		{
			const auto Found = m_Textures.find(TextureID);
			if (Found == m_Textures.end())
			{
				UnknownUnregistrations.push_back(TextureID);
				return;
			}
			const std::shared_ptr<FakeTextureLifetime> Lifetime = Found->second.lock();
			UnregisterSawLiveTexture.push_back(Lifetime && Lifetime->Alive);
			m_Textures.erase(Found);
		}

		bool FailNextRegistration = false;
		size_t RegistrationAttempts = 0;
		std::vector<bool> UnregisterSawLiveTexture;
		std::vector<uint64_t> UnknownUnregistrations;
		[[nodiscard]] size_t RegisteredCount() const noexcept { return m_Textures.size(); }

	private:
		uint64_t NextID = 1;
		std::map<uint64_t, std::weak_ptr<FakeTextureLifetime>> m_Textures;
	};

	bool WritePpm(const std::filesystem::path& Path, uint32_t Width, uint32_t Height,
		std::array<char, 3> Color = { 90, 100, 110 })
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output << "P6\n" << Width << ' ' << Height << "\n255\n";
		for (uint64_t Pixel = 0; Pixel < static_cast<uint64_t>(Width) * Height; ++Pixel)
		{
			Output.write(Color.data(), static_cast<std::streamsize>(Color.size()));
		}
		return Output.good();
	}

	std::filesystem::path MakePreviewTestRoot(TestRunner& Tests, std::string_view Name);

	void TestTextureUploadFailureReleasesCpuProductAndRequiresExplicitRetry(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::filesystem::path Root = MakePreviewTestRoot(Tests, "TextureUploadFailure");
		if (Root.empty()) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup() { std::error_code Error; std::filesystem::remove_all(Path, Error); }
		} CleanupTemporary{ Root };
		const std::filesystem::path AssetDirectory = Root / "Assets" / "Textures";
		std::error_code Error;
		std::filesystem::create_directories(AssetDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;

		const std::filesystem::path FirstPath = AssetDirectory / "First.ppm";
		const std::filesystem::path OtherPath = AssetDirectory / "Other.ppm";
		PF_CHECK(Tests, WritePpm(FirstPath, 128, 128,
			{ static_cast<char>(30), static_cast<char>(70), static_cast<char>(110) }));
		PF_CHECK(Tests, WritePpm(OtherPath, 128, 128,
			{ static_cast<char>(120), static_cast<char>(90), static_cast<char>(60) }));
		const auto FirstMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstPath);
		const auto OtherMetadata = AssetMetadataSerializer::CreateForNewAsset(OtherPath);
		PF_CHECK(Tests, FirstMetadata.has_value() && OtherMetadata.has_value());
		if (!FirstMetadata || !OtherMetadata) return;
		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(Root).has_value());
		auto RegistrySnapshot = std::make_shared<const AssetRegistry>(Registry);
		AssetPreparationService Preparation;
		DerivedDataArtifactStoreOptions StoreOptions;
		StoreOptions.Root = Root / "DerivedCache";
		StoreOptions.Enabled = false;
		DerivedDataArtifactStore Store(StoreOptions);
		AssetUploadBudget UploadBudget;
		FakeRenderDevice Runtime;
		TextureAssetCache Textures(Runtime, Root, RegistrySnapshot, Preparation, &UploadBudget, Store, "texture-upload-1");

		const auto WaitUntilCpuReady = [&]()
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				if (Preparation.GetStats().CpuReadyJobs > 0) return true;
				std::this_thread::yield();
			}
			return Preparation.GetStats().CpuReadyJobs > 0;
		};
		const auto AdvanceUntilRevision = [&](const AssetID& Asset, uint64_t Revision)
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				UploadBudget.BeginFrame();
				(void)Textures.AdvancePending();
				if (Textures.GetResourceRevision(Asset, TextureFormat::RGBA8_Srgb) >= Revision) return true;
				std::this_thread::yield();
			}
			return Textures.GetResourceRevision(Asset, TextureFormat::RGBA8_Srgb) >= Revision;
		};

		(void)Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Textures.HasPendingWork());
		PF_CHECK(Tests, WaitUntilCpuReady());
		PF_CHECK(Tests, AdvanceUntilRevision(FirstMetadata->ID, 1));
		PF_CHECK(Tests, !Textures.HasPendingWork());
		auto Original = Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Original.has_value());
		if (!Original) return;
		const Texture* OriginalTexture = &Original->get();
		PF_CHECK(Tests, Runtime.TextureCreations == 1);

		PF_CHECK(Tests, WritePpm(FirstPath, 128, 128,
			{ static_cast<char>(200), static_cast<char>(20), static_cast<char>(40) }));
		Textures.SetSessionKey("texture-upload-replacement");
		(void)Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Textures.HasPendingWork());
		PF_CHECK(Tests, WaitUntilCpuReady());
		Runtime.TextureCreationFailuresRemaining = 1;
		UploadBudget.BeginFrame();
		(void)Textures.AdvancePending();
		PF_CHECK(Tests, Textures.GetResourceRevision(FirstMetadata->ID, TextureFormat::RGBA8_Srgb) == 1);
		auto Retained = Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Retained && &Retained->get() == OriginalTexture);
		PF_CHECK(Tests, Runtime.TextureCreations == 2);
		PF_CHECK(Tests, Preparation.GetStats().CompletedRetainedPayloadBytes == 0);
		PF_CHECK(Tests, !Textures.HasPendingWork());

		for (size_t Poll = 0; Poll < 20; ++Poll)
		{
			UploadBudget.BeginFrame();
			(void)Textures.AdvancePending();
		}
		PF_CHECK(Tests, Runtime.TextureCreations == 2);
		Retained = Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Retained && &Retained->get() == OriginalTexture);

		// A second product can complete after the failed upload released its completed CPU payload budget.
		(void)Textures.GetOrLoad(OtherMetadata->ID);
		PF_CHECK(Tests, WaitUntilCpuReady());
		PF_CHECK(Tests, AdvanceUntilRevision(OtherMetadata->ID, 1));
		PF_CHECK(Tests, Textures.GetResourceRevision(OtherMetadata->ID, TextureFormat::RGBA8_Srgb) == 1);

		// Retry is explicit: a new source/session generation clears the terminal upload failure.
		Textures.SetSessionKey("texture-upload-explicit-retry");
		(void)Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Textures.HasPendingWork());
		PF_CHECK(Tests, WaitUntilCpuReady());
		PF_CHECK(Tests, AdvanceUntilRevision(FirstMetadata->ID, 2));
		auto Replaced = Textures.GetOrLoad(FirstMetadata->ID);
		PF_CHECK(Tests, Replaced && &Replaced->get() != OriginalTexture);
		PF_CHECK(Tests, Textures.GetResourceRevision(FirstMetadata->ID, TextureFormat::RGBA8_Srgb) == 2);
		Preparation.Shutdown();
	}

	bool WriteTinyHdr(const std::filesystem::path& Path, uint8_t Red)
	{
		const std::string Header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n";
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output) return false;
		Output.write(Header.data(), static_cast<std::streamsize>(Header.size()));
		for (size_t Pixel = 0; Pixel < 4; ++Pixel)
		{
			const std::array<uint8_t, 4> Rgbe{ Red, 64, 32, 129 };
			Output.write(reinterpret_cast<const char*>(Rgbe.data()), static_cast<std::streamsize>(Rgbe.size()));
		}
		return Output.good();
	}

	void TestEnvironmentUploadFailureIsTerminalUntilNewSession(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::filesystem::path Root = MakePreviewTestRoot(Tests, "EnvironmentUploadFailure");
		if (Root.empty()) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup() { std::error_code Error; std::filesystem::remove_all(Path, Error); }
		} CleanupTemporary{ Root };
		const std::filesystem::path AssetDirectory = Root / "Assets" / "Environments";
		std::error_code Error;
		std::filesystem::create_directories(AssetDirectory, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		const std::filesystem::path HdrPath = AssetDirectory / "tiny.hdr";
		PF_CHECK(Tests, WriteTinyHdr(HdrPath, 128));
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(HdrPath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata) return;
		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(Root).has_value());
		AssetPreparationService Preparation;
		DerivedDataArtifactStoreOptions StoreOptions;
		StoreOptions.Root = Root / "DerivedCache";
		StoreOptions.Enabled = false;
		DerivedDataArtifactStore Store(StoreOptions);
		AssetUploadBudget UploadBudget;
		EnvironmentProcessingDesc Processing;
		Processing.EnvironmentFaceSize = 2;
		Processing.IrradianceFaceSize = 1;
		Processing.SpecularFaceSize = 2;
		Processing.BrdfLutSize = 2;
		Processing.IrradianceSamples = 2;
		Processing.PrefilterSamples = 2;
		Processing.BrdfSamples = 2;
		FakeRenderDevice Runtime;
		EnvironmentLightingCache Environments(Runtime, Root, std::make_shared<const AssetRegistry>(Registry), Preparation,
			&UploadBudget, Store, "environment-upload-1", nullptr, Processing);

		const auto WaitUntilCpuReady = [&]()
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				if (Preparation.GetStats().CpuReadyJobs > 0) return true;
				std::this_thread::yield();
			}
			return Preparation.GetStats().CpuReadyJobs > 0;
		};
		const auto AdvanceUntilRevision = [&](uint64_t Revision)
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < Deadline)
			{
				UploadBudget.BeginFrame();
				(void)Environments.AdvancePending();
				if (Environments.GetResourceRevision(Metadata->ID) >= Revision) return true;
				std::this_thread::yield();
			}
			return Environments.GetResourceRevision(Metadata->ID) >= Revision;
		};

		(void)Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Environments.HasPendingWork());
		PF_CHECK(Tests, WaitUntilCpuReady());
		PF_CHECK(Tests, AdvanceUntilRevision(1));
		auto Original = Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Original && Original->has_value());
		if (!Original || !*Original) return;
		const EnvironmentLightingTextures* OriginalTextures = &Original->value().get();
		const Texture* OriginalEnvironmentTexture = OriginalTextures->Environment.get();

		PF_CHECK(Tests, WriteTinyHdr(HdrPath, 220));
		Environments.SetSessionKey("environment-upload-replacement");
		(void)Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, WaitUntilCpuReady());
		Runtime.TextureCreationFailuresRemaining = 1;
		const size_t AttemptsBeforeFailure = Runtime.TextureCreations;
		UploadBudget.BeginFrame();
		(void)Environments.AdvancePending();
		PF_CHECK(Tests, Runtime.TextureCreations == AttemptsBeforeFailure + 1);
		PF_CHECK(Tests, Environments.GetResourceRevision(Metadata->ID) == 1);
		PF_CHECK(Tests, !Environments.HasPendingWork());
		auto Retained = Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Retained && Retained->has_value() &&
			Retained->value().get().Environment.get() == OriginalEnvironmentTexture);
		PF_CHECK(Tests, Preparation.GetStats().CompletedRetainedPayloadBytes == 0);
		for (size_t Poll = 0; Poll < 20; ++Poll)
		{
			UploadBudget.BeginFrame();
			(void)Environments.AdvancePending();
		}
		PF_CHECK(Tests, Runtime.TextureCreations == AttemptsBeforeFailure + 1);
		Retained = Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Retained && Retained->has_value() &&
			Retained->value().get().Environment.get() == OriginalEnvironmentTexture);

		Environments.SetSessionKey("environment-upload-explicit-retry");
		(void)Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Environments.HasPendingWork());
		PF_CHECK(Tests, WaitUntilCpuReady());
		PF_CHECK(Tests, AdvanceUntilRevision(2));
		PF_CHECK(Tests, !Environments.HasPendingWork());
		auto Replaced = Environments.GetOrLoad(Metadata->ID);
		PF_CHECK(Tests, Replaced && Replaced->has_value() &&
			Replaced->value().get().Environment.get() != OriginalEnvironmentTexture);
		Preparation.Shutdown();
	}

	std::filesystem::path MakePreviewTestRoot(TestRunner& Tests, std::string_view Name)
	{
		std::error_code Error;
		const auto Identifier = PulseForge::UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return {};
		const std::filesystem::path Root = std::filesystem::temp_directory_path(Error) /
										   (std::string("PulseForgePreview-") + std::string(Name) + "-" + Identifier->ToString());
		PF_CHECK(Tests, !Error);
		if (Error) return {};
		std::filesystem::create_directories(Root, Error);
		PF_CHECK(Tests, !Error);
		return Error ? std::filesystem::path{} : Root;
	}

	template <typename Predicate>
	bool WaitForPreviewState(PulseForgeEditor::EditorAssetPreviewService& Previews, Predicate&& IsReady, bool Pump)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			if (Pump) Previews.PumpUploads();
			if (IsReady(Previews.GetStats())) return true;
			std::this_thread::yield();
		}
		return false;
	}

	void TestPreviewStatisticsWhileWorkerDecodes(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier) return;
		std::error_code FileError;
		const std::filesystem::path TemporaryRoot =
			std::filesystem::temp_directory_path(FileError) / ("PulseForgePreviewStatistics-" + Identifier->ToString());
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		std::filesystem::create_directories(TemporaryRoot, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} CleanupTemporary{ TemporaryRoot };

		const std::filesystem::path MaterialPath = TemporaryRoot / "preview.material";
		MaterialAssetDesc Description;
		const auto Serialized = MaterialAssetSerializer::Serialize(Description);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		{
			std::ofstream Output(MaterialPath, std::ios::binary | std::ios::trunc);
			Output.write(Serialized->data(), static_cast<std::streamsize>(Serialized->size()));
			const std::string Padding(256u * 1024u, ' ');
			Output.write(Padding.data(), static_cast<std::streamsize>(Padding.size()));
			PF_CHECK(Tests, Output.good());
			if (!Output.good()) return;
		}

		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		EditorAssetPreviewService Previews;
		AssetPreviewServiceLimits Limits;
		Limits.MaximumQueuedRequests = 48;
		Limits.MaximumCpuBytes = 1024;
		Previews.Initialize(Runtime, Registry, Limits);
		const std::string ProjectID = "preview-statistics-project";
		Previews.SetProject(ProjectID);
		constexpr size_t RequestCount = 40;
		for (size_t Index = 0; Index < RequestCount; ++Index)
			Previews.RequestMaterialSwatch(ProjectID, "asset-" + std::to_string(Index), MaterialPath);

		bool MonotonicPeak = true;
		size_t PreviousPeak = 0;
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < Deadline)
		{
			const AssetPreviewStats Stats = Previews.GetStats();
			if (Stats.PeakQueuedResultCpuBytes < PreviousPeak) MonotonicPeak = false;
			PreviousPeak = Stats.PeakQueuedResultCpuBytes;
			if (Stats.PeakQueuedResultCpuBytes == RequestCount * 4 && Stats.PendingRequests == 0) break;
			std::this_thread::yield();
		}
		const AssetPreviewStats FinalStats = Previews.GetStats();
		PF_CHECK(Tests, MonotonicPeak);
		PF_CHECK(Tests, FinalStats.PendingRequests == 0);
		PF_CHECK(Tests, FinalStats.PeakQueuedResultCpuBytes == RequestCount * 4);
		Previews.Shutdown();
	}

	void TestPreviewOversizedResultDoesNotBlockWorker(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Root = MakePreviewTestRoot(Tests, "Oversized");
		if (Root.empty()) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} CleanupTemporary{ Root };

		const std::filesystem::path LargeImage = Root / "large.PPM";
		const std::filesystem::path SmallImage = Root / "small.ppm";
		PF_CHECK(Tests, WritePpm(LargeImage, 8, 8));
		PF_CHECK(Tests, WritePpm(SmallImage, 1, 1));

		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		EditorAssetPreviewService Previews;
		AssetPreviewServiceLimits Limits;
		Limits.MaximumCpuBytes = 4;
		Limits.MaximumUploadBytesPerUpdate = 4;
		Previews.Initialize(Runtime, Registry, Limits);
		Previews.SetProject("oversized-result-project");
		Previews.RequestImage("oversized-result-project", "large", LargeImage);
		Previews.RequestImage("oversized-result-project", "small", SmallImage);

		const bool SmallCompleted = WaitForPreviewState(
			Previews, [&](const AssetPreviewStats&) { return Previews.FindPreview("oversized-result-project", "small").has_value(); },
			true);
		PF_CHECK(Tests, SmallCompleted);
		const AssetPreviewStats Stats = Previews.GetStats();
		PF_CHECK(Tests, !Stats.WorkerFailed);
		PF_CHECK(Tests, Stats.FailedEntries == 1 && Stats.ReadyEntries == 1);
		PF_CHECK(Tests, Stats.DecodesFailed == 1 && Stats.DecodesSucceeded == 1);
		PF_CHECK(Tests, Stats.PeakQueuedResultCpuBytes <= Limits.MaximumCpuBytes);
		PF_CHECK(Tests, !Previews.FindPreview("oversized-result-project", "large").has_value());
		Previews.Shutdown();
	}

	void TestPreviewBudgetWaitInvalidationAndShutdown(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Root = MakePreviewTestRoot(Tests, "WaitRelease");
		if (Root.empty()) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} CleanupTemporary{ Root };
		const std::filesystem::path SmallImage = Root / "small.ppm";
		PF_CHECK(Tests, WritePpm(SmallImage, 1, 1));

		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		EditorAssetPreviewService Previews;
		AssetPreviewServiceLimits Limits;
		Limits.MaximumCpuBytes = 4;
		Limits.MaximumUploadBytesPerUpdate = 4;
		Previews.Initialize(Runtime, Registry, Limits);
		Previews.SetProject("blocked-project");
		Previews.RequestImage("blocked-project", "queued-result", SmallImage);
		Previews.RequestImage("blocked-project", "blocked-worker", SmallImage);
		const bool WorkerBlocked = WaitForPreviewState(
			Previews, [&](const AssetPreviewStats& Stats)
			{ return Stats.QueuedResultCpuBytes == 4 && Stats.CpuBudgetWaits > 0 && Stats.PendingRequests == 0; }, false);
		PF_CHECK(Tests, WorkerBlocked);

		Previews.SetProject("replacement-project");
		Previews.RequestImage("replacement-project", "current-project", SmallImage);
		const bool CurrentProjectCompleted = WaitForPreviewState(
			Previews, [&](const AssetPreviewStats&) { return Previews.FindPreview("replacement-project", "current-project").has_value(); },
			true);
		PF_CHECK(Tests, CurrentProjectCompleted);
		PF_CHECK(Tests, !Previews.FindPreview("blocked-project", "queued-result").has_value());
		PF_CHECK(Tests, Previews.GetStats().StaleResultsDiscarded > 0);
		Previews.Shutdown();

		FakeRenderDevice ShutdownRuntime;
		FakeTextureRegistry ShutdownRegistry;
		EditorAssetPreviewService ShutdownPreviews;
		ShutdownPreviews.Initialize(ShutdownRuntime, ShutdownRegistry, Limits);
		ShutdownPreviews.SetProject("shutdown-project");
		ShutdownPreviews.RequestImage("shutdown-project", "queued-result", SmallImage);
		ShutdownPreviews.RequestImage("shutdown-project", "blocked-worker", SmallImage);
		const bool ShutdownWorkerBlocked = WaitForPreviewState(
			ShutdownPreviews, [&](const AssetPreviewStats& Stats)
			{ return Stats.QueuedResultCpuBytes == 4 && Stats.CpuBudgetWaits > 0 && Stats.PendingRequests == 0; }, false);
		PF_CHECK(Tests, ShutdownWorkerBlocked);
		const auto ShutdownStart = std::chrono::steady_clock::now();
		ShutdownPreviews.Shutdown();
		PF_CHECK(Tests, std::chrono::steady_clock::now() - ShutdownStart < std::chrono::seconds(5));
	}

	void TestPreviewZeroBudgetsAndDecodeException(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Root = MakePreviewTestRoot(Tests, "ZeroAndException");
		if (Root.empty()) return;
		struct Cleanup final
		{
			std::filesystem::path Path;
			~Cleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} CleanupTemporary{ Root };
		const std::filesystem::path SmallImage = Root / "small.PPM";
		PF_CHECK(Tests, WritePpm(SmallImage, 1, 1));

		const auto RunZeroBudget = [&](AssetPreviewServiceLimits Limits, std::string_view ProjectID)
		{
			FakeRenderDevice Runtime;
			FakeTextureRegistry Registry;
			EditorAssetPreviewService Previews;
			Previews.Initialize(Runtime, Registry, Limits);
			Previews.SetProject(std::string(ProjectID));
			Previews.RequestImage(ProjectID, "zero-budget", SmallImage);
			const bool Failed =
				WaitForPreviewState(Previews, [](const AssetPreviewStats& Stats) { return Stats.FailedEntries == 1; }, true);
			PF_CHECK(Tests, Failed);
			PF_CHECK(Tests, !Previews.GetStats().WorkerFailed);
			PF_CHECK(Tests, !Previews.FindPreview(ProjectID, "zero-budget").has_value());
			Previews.Shutdown();
		};
		AssetPreviewServiceLimits ZeroCpu;
		ZeroCpu.MaximumCpuBytes = 0;
		RunZeroBudget(ZeroCpu, "zero-cpu-project");
		AssetPreviewServiceLimits ZeroUploads;
		ZeroUploads.MaximumUploadsPerUpdate = 0;
		RunZeroBudget(ZeroUploads, "zero-count-project");
		AssetPreviewServiceLimits ZeroUploadBytes;
		ZeroUploadBytes.MaximumUploadBytesPerUpdate = 0;
		RunZeroBudget(ZeroUploadBytes, "zero-upload-bytes-project");

		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		EditorAssetPreviewService Previews;
		Previews.Initialize(Runtime, Registry);
		EditorAssetPreviewServiceTestAccess::ThrowForOneAsset(Previews);
		Previews.SetProject("decode-exception-project");
		Previews.RequestImage("decode-exception-project", "decode-throws", SmallImage);
		Previews.RequestImage("decode-exception-project", "decode-recovers", SmallImage);
		const bool Recovered = WaitForPreviewState(
			Previews, [&](const AssetPreviewStats&)
			{ return Previews.FindPreview("decode-exception-project", "decode-recovers").has_value(); }, true);
		PF_CHECK(Tests, Recovered);
		PF_CHECK(Tests, Previews.GetStats().FailedEntries == 1);
		PF_CHECK(Tests, !Previews.GetStats().WorkerFailed);
		Previews.Shutdown();
	}

#ifdef PF_EDITOR_RENDERER_OPENGL
	void TestOpenGLSceneViewportFallback(TestRunner& Tests)
	{
		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		PulseForgeEditor::EditorViewportSceneRenderView View;
		const auto Result = View.EnsureTarget(320.0f, 200.0f, 1.0f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, !Result);
		PF_CHECK(Tests, !View.HasTarget());
		PF_CHECK(Tests, Runtime.RenderTargetCreations == 0);
		PF_CHECK(Tests, Registry.RegistrationAttempts == 0);
	}
#else
	void TestSceneViewportTargetLifecycle(TestRunner& Tests)
	{
		FakeRenderDevice Runtime;
		FakeTextureRegistry Registry;
		PulseForgeEditor::EditorViewportSceneRenderView View;

		const auto Initial = View.EnsureTarget(100.0f, 80.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, Initial && *Initial);
		PF_CHECK(Tests, Runtime.RenderTargetCreations == 1 && Registry.RegistrationAttempts == 1);
		PF_CHECK(Tests, View.HasTarget() && View.GetTextureID() == 1);
		PF_CHECK(Tests, View.GetWidth() == 128 && View.GetHeight() == 96);
		PF_CHECK(Tests, Registry.RegisteredCount() == 1);
		PulseForge::RenderTargetClearValue Background;
		Runtime.FailNextClearBegin = true;
		PF_CHECK(Tests, !View.Render(Runtime, true, 0.0f, Background) && !View.GetSceneError().empty());
		PF_CHECK(Tests, Runtime.ClearBegins == 1 && Runtime.ClearEnds == 0);
		PF_CHECK(Tests, !View.Render(Runtime, true, 0.0f, Background) && View.GetSceneError().empty());
		PF_CHECK(Tests, Runtime.ClearBegins == 2 && Runtime.ClearEnds == 1);
		Runtime.FailNextClearEnd = true;
		PF_CHECK(Tests, !View.Render(Runtime, true, 0.0f, Background) && !View.GetSceneError().empty());
		PF_CHECK(Tests, !View.Render(Runtime, true, 0.0f, Background) && View.GetSceneError().empty());
		PF_CHECK(Tests, Runtime.ClearBegins == 4 && Runtime.ClearEnds == 3);

		const auto Unchanged = View.EnsureTarget(100.0f, 80.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, Unchanged && !*Unchanged);
		PF_CHECK(Tests, Runtime.RenderTargetCreations == 1 && Registry.RegistrationAttempts == 1);

		const PulseForge::RenderTarget* OriginalTarget = View.GetTarget();
		const auto InvalidExtent = View.EnsureTarget(-1.0f, 80.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, !InvalidExtent && !View.GetTargetError().empty());
		PF_CHECK(Tests, View.GetTarget() == OriginalTarget && View.GetTextureID() == 1);
		PF_CHECK(Tests, Registry.RegisteredCount() == 1);
		const auto RecoveredExtent = View.EnsureTarget(100.0f, 80.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, RecoveredExtent && !*RecoveredExtent && View.GetTargetError().empty());

		Runtime.FailNextRenderTarget = true;
		const auto TargetFailure = View.EnsureTarget(200.0f, 150.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, !TargetFailure && !View.GetTargetError().empty());
		PF_CHECK(Tests, View.GetTarget() == OriginalTarget && View.GetTextureID() == 1);
		PF_CHECK(Tests, View.GetWidth() == 128 && View.GetHeight() == 96 && Registry.RegisteredCount() == 1);
		const auto SameExtentAfterFailure = View.EnsureTarget(100.0f, 80.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, SameExtentAfterFailure && !*SameExtentAfterFailure && View.GetTargetError().empty());

		Registry.FailNextRegistration = true;
		const auto RegistrationFailure = View.EnsureTarget(200.0f, 150.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, !RegistrationFailure && !View.GetTargetError().empty());
		PF_CHECK(Tests, View.GetTarget() == OriginalTarget && View.GetTextureID() == 1);
		PF_CHECK(Tests, View.GetWidth() == 128 && View.GetHeight() == 96 && Registry.RegisteredCount() == 1);
		PF_CHECK(Tests, Runtime.LastTextureLifetime && !Runtime.LastTextureLifetime->Alive);

		const auto Replacement = View.EnsureTarget(200.0f, 150.0f, 1.25f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, Replacement && *Replacement && View.GetTextureID() == 2);
		PF_CHECK(Tests, View.GetWidth() == 256 && View.GetHeight() == 160 && Registry.RegisteredCount() == 1);
		PF_CHECK(Tests, Registry.UnregisterSawLiveTexture.size() == 1 && Registry.UnregisterSawLiveTexture.front());

		const PulseForge::RenderTarget* ReplacementTarget = View.GetTarget();
		Runtime.NextTargetWithoutColor = true;
		const auto MissingColor = View.EnsureTarget(300.0f, 150.0f, 1.0f, 1.0f, Runtime, Registry);
		PF_CHECK(Tests, !MissingColor && !View.GetTargetError().empty());
		PF_CHECK(Tests, View.GetTarget() == ReplacementTarget && View.GetTextureID() == 2);
		PF_CHECK(Tests, Registry.RegisteredCount() == 1);

		View.ResetTarget();
		PF_CHECK(Tests, !View.HasTarget() && View.GetTextureID() == 0);
		PF_CHECK(Tests, View.GetWidth() == 0 && View.GetHeight() == 0 && View.GetTargetError().empty());
		PF_CHECK(Tests, Registry.RegisteredCount() == 0);
		PF_CHECK(Tests, Registry.UnregisterSawLiveTexture.size() == 2 && Registry.UnregisterSawLiveTexture.back());
		PF_CHECK(Tests, Registry.UnknownUnregistrations.empty());
		FakeTextureRegistry ReplacementRegistry;
		const auto ProjectReplacement = View.EnsureTarget(72.0f, 48.0f, 1.0f, 1.0f, Runtime, ReplacementRegistry);
		PF_CHECK(Tests, ProjectReplacement && *ProjectReplacement && Registry.RegisteredCount() == 0);
		PF_CHECK(Tests, ReplacementRegistry.RegisteredCount() == 1 && View.GetTextureID() == 1);
		View.ResetScene();
		View.ResetTarget();
		PF_CHECK(Tests, ReplacementRegistry.RegisteredCount() == 0);
		PF_CHECK(Tests, ReplacementRegistry.UnregisterSawLiveTexture.size() == 1 && ReplacementRegistry.UnregisterSawLiveTexture.front());
		PF_CHECK(Tests, ReplacementRegistry.UnknownUnregistrations.empty());

		{
			PulseForgeEditor::EditorViewportSceneRenderView DestructedView;
			const auto Created = DestructedView.EnsureTarget(64.0f, 64.0f, 1.0f, 1.0f, Runtime, Registry);
			PF_CHECK(Tests, Created && *Created && Registry.RegisteredCount() == 1);
		}
		PF_CHECK(Tests, Registry.RegisteredCount() == 0);
		PF_CHECK(Tests, Registry.UnregisterSawLiveTexture.size() == 3 && Registry.UnregisterSawLiveTexture.back());
		PF_CHECK(Tests, Registry.UnknownUnregistrations.empty());
	}
#endif
} // namespace

void RunEditorViewportSceneRenderViewTests(TestRunner& Tests)
{
	TestTextureUploadFailureReleasesCpuProductAndRequiresExplicitRetry(Tests);
	TestEnvironmentUploadFailureIsTerminalUntilNewSession(Tests);
	TestPreviewStatisticsWhileWorkerDecodes(Tests);
	TestPreviewOversizedResultDoesNotBlockWorker(Tests);
	TestPreviewBudgetWaitInvalidationAndShutdown(Tests);
	TestPreviewZeroBudgetsAndDecodeException(Tests);
#ifdef PF_EDITOR_RENDERER_OPENGL
	TestOpenGLSceneViewportFallback(Tests);
#else
	TestSceneViewportTargetLifecycle(Tests);
#endif
}
