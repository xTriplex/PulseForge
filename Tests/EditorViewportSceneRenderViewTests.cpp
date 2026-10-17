#include "TestEditorSupport.h"
#include "Assets/MaterialAsset.h"
#include "Editor/EditorAssetPreviewService.h"
#include "EditorViewportSceneRenderView.h"

#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <thread>
#include <vector>

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
			const PulseForge::TextureDesc&, std::span<const PulseForge::TextureSubresourceData>) override
		{
			return std::unexpected(PulseForge::TextureError{ PulseForge::TextureErrorCode::BackendFailure, "unused" });
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
		size_t RenderTargetCreations = 0;
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
			if (Stats.PeakCpuBytes < PreviousPeak) MonotonicPeak = false;
			PreviousPeak = Stats.PeakCpuBytes;
			if (Stats.PeakCpuBytes == RequestCount * 4 && Stats.PendingRequests == 0) break;
			std::this_thread::yield();
		}
		const AssetPreviewStats FinalStats = Previews.GetStats();
		PF_CHECK(Tests, MonotonicPeak);
		PF_CHECK(Tests, FinalStats.PendingRequests == 0);
		PF_CHECK(Tests, FinalStats.PeakCpuBytes == RequestCount * 4);
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
	TestPreviewStatisticsWhileWorkerDecodes(Tests);
#ifdef PF_EDITOR_RENDERER_OPENGL
	TestOpenGLSceneViewportFallback(Tests);
#else
	TestSceneViewportTargetLifecycle(Tests);
#endif
}
