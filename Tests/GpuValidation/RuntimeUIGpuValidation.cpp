#include "Core/Application.h"
#include "Core/Layer.h"
#include "Core/Log.h"
#include "Renderer/Detail/RuntimeUIRenderPass.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace PulseForge;

	std::vector<std::byte> ReadBytes(const std::filesystem::path& Path)
	{
		std::ifstream Stream(Path, std::ios::binary);
		if (!Stream) throw std::runtime_error("Could not open runtime UI GPU fixture: " + Path.string());
		std::vector<char> Characters{ std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>() };
		if (Stream.bad()) throw std::runtime_error("Could not read runtime UI GPU fixture: " + Path.string());
		std::vector<std::byte> Bytes(Characters.size());
		for (size_t Index = 0; Index < Characters.size(); ++Index)
			Bytes[Index] = static_cast<std::byte>(static_cast<unsigned char>(Characters[Index]));
		return Bytes;
	}

	RuntimeUIElement MakeElement(uint64_t ID, RuntimeUIElementKind Kind, int32_t Order, glm::vec2 Minimum, glm::vec2 Maximum)
	{
		RuntimeUIElement Element;
		Element.ID = UUID{ 0xb018000000004000ull, ID };
		Element.Kind = Kind;
		Element.Order = Order;
		Element.OffsetMinimum = Minimum;
		Element.OffsetMaximum = Maximum;
		return Element;
	}

	class RuntimeUIGpuValidationLayer final : public Layer
	{
	public:
		RuntimeUIGpuValidationLayer() : Layer("Runtime UI rendered-pixel validation") {}

		void OnAttach() override
		{
			m_Started = std::chrono::steady_clock::now();
			try
			{
				Initialize();
			}
			catch (const std::exception& Exception)
			{
				Fail(Exception.what());
			}
		}

		void OnUpdate(Timestep) override
		{
			if (std::chrono::steady_clock::now() - m_Started > std::chrono::seconds(20))
				Fail("Timed out during runtime UI rendered-pixel validation");
			else if (m_ReadbackPending)
				ValidateCompletedFrame();
		}

		void OnRender() override
		{
			if (!m_Succeeded || m_CompletedFrames >= 3) return;
			Application& App = Application::Get();
			const RenderTargetClearValue Clear{ { 0.0f, 0.0f, 0.0f, 1.0f }, 1.0f };
			if (const auto Begun = App.BeginRenderTarget(*m_Target, Clear); !Begun)
			{
				Fail("Could not begin UI readback target: " + Begun.error().Message);
				return;
			}

			const auto Prepared = m_Pass->Prepare(m_Commands, m_Viewport,
				[this](AssetID Asset) -> const Texture* { return Asset == m_ImageID ? m_Image.get() : nullptr; },
				[this](AssetID Asset) -> std::expected<std::vector<std::byte>, std::string>
				{
					if (Asset != m_FontID) return std::unexpected("Unexpected font asset in GPU fixture");
					return m_FontBytes;
				});
			std::expected<size_t, GraphicsError> Draw = size_t{ 0 };
			if (Prepared)
				Draw = m_Pass->Draw(m_Commands, m_Viewport, 128, 96, ColorTargetFormat::RGBA8_Srgb, false);
			const GraphicsResult Ended = App.EndRenderTarget();
			if (!Prepared)
			{
				if (!Ended) PF_ERROR("Runtime UI validation target end also failed: {0}", Ended.error().Message);
				Fail("Could not prepare runtime UI textures and glyphs: " + Prepared.error().Message);
				return;
			}
			if (!Draw)
			{
				if (!Ended) PF_ERROR("Runtime UI validation target end also failed: {0}", Ended.error().Message);
				Fail("Could not draw runtime UI quads: " + Draw.error().Message);
				return;
			}
			if (!Ended)
			{
				Fail("Could not close runtime UI validation target: " + Ended.error().Message);
				return;
			}

			m_ReadbackPending = true;
		}

		void ValidateCompletedFrame()
		{
			m_ReadbackPending = false;
			Application& App = Application::Get();
			const auto* Color = m_Target->GetColorTexture();
			if (!Color)
			{
				Fail("Runtime UI validation target has no color texture");
				return;
			}
			auto Readback = App.ReadTextureForValidation(*Color);
			if (!Readback || Readback->Format != TextureFormat::RGBA8_Srgb || Readback->Width != 128 || Readback->Height != 96 ||
				Readback->Pixels.size() != 128u * 96u * 4u)
			{
				Fail(Readback ? "Runtime UI readback has unexpected dimensions or format"
							  : "Could not read rendered runtime UI pixels: " + Readback.error().Message);
				return;
			}

			const auto Pixel = [&](uint32_t X, uint32_t Y)
			{
				const size_t Offset = (static_cast<size_t>(Y) * Readback->Width + X) * 4;
				return std::array<uint8_t, 4>{ std::to_integer<uint8_t>(Readback->Pixels[Offset]),
					std::to_integer<uint8_t>(Readback->Pixels[Offset + 1]), std::to_integer<uint8_t>(Readback->Pixels[Offset + 2]),
					std::to_integer<uint8_t>(Readback->Pixels[Offset + 3]) };
			};
			const auto Blended = Pixel(8, 8);
			const auto ClippedIn = Pixel(24, 8);
			const auto ClippedOut = Pixel(100, 20);
			if (Blended[0] < 150 || Blended[0] > 220 || Blended[1] < 150 || Blended[1] > 220 || Blended[2] > 20)
			{
				Fail("Overlapping alpha quads did not produce the expected sRGB blended pixel");
				return;
			}
			if (ClippedIn[1] < 220 || ClippedIn[0] > 20 || ClippedIn[2] > 20)
			{
				Fail("The clipped child quad was not visible inside its parent clip");
				return;
			}
			if (ClippedOut[2] < 220 || ClippedOut[0] > 20 || ClippedOut[1] > 20)
			{
				Fail("The child quad escaped its parent scissor clip");
				return;
			}

			size_t BrightTextPixels = 0;
			for (uint32_t Y = 64; Y < 94; ++Y)
				for (uint32_t X = 80; X < 128; ++X)
				{
					const auto Sample = Pixel(X, Y);
					BrightTextPixels += Sample[0] > 160 && Sample[1] > 160 && Sample[2] > 160;
				}
			if (BrightTextPixels < 12)
			{
				Fail("Supported non-ASCII text produced no readable glyph coverage in the rendered pixels");
				return;
			}

			const RuntimeUIRenderStatistics Statistics = m_Pass->GetStatistics();
			if (Statistics.QuadCount < 8 || Statistics.BatchCount < 2 || Statistics.BatchCount >= Statistics.QuadCount ||
				Statistics.TextureCount != 2 || Statistics.FontAtlasCount != 1)
			{
				Fail("Runtime UI statistics were unexpected: quads=" + std::to_string(Statistics.QuadCount) + ", batches=" +
					std::to_string(Statistics.BatchCount) + ", textures=" + std::to_string(Statistics.TextureCount) + ", atlases=" +
					std::to_string(Statistics.FontAtlasCount));
				return;
			}
			if (m_CompletedFrames != 0 && Readback->Pixels != m_FirstFramePixels)
			{
				Fail("Repeated unchanged runtime UI frames changed rendered pixels");
				return;
			}
			if (m_CompletedFrames == 0)
			{
				m_FirstFramePixels = std::move(Readback->Pixels);
				PF_INFO("RUNTIME_UI_GPU=PASS; quads={0}; batches={1}; image_textures={2}; font_atlases={3}; target=128x96 sRGB; é glyph and alpha/clipping pixels verified",
					Statistics.QuadCount, Statistics.BatchCount, Statistics.TextureCount, Statistics.FontAtlasCount);
			}
			++m_CompletedFrames;
			if (m_CompletedFrames == 3)
			{
				PF_INFO("GPU_VALIDATION_RESULT=PASS; runtime UI image/text quads rendered with alpha blending, clipping, ordered batching, and stable repeated-frame output");
				App.RequestClose();
			}
		}

		void OnDetach() override
		{
			m_Pass.reset();
			m_Target.reset();
			m_Image.reset();
		}

		[[nodiscard]] bool Succeeded() const noexcept { return m_Succeeded && m_CompletedFrames == 3; }

	private:
		void Fail(const std::string& Message)
		{
			m_Succeeded = false;
			PF_ERROR("GPU_VALIDATION_RESULT=FAIL: {0}", Message);
			Application::Get().RequestClose();
		}

		void Initialize()
		{
			Application& App = Application::Get();
			const auto ShaderRoot = std::filesystem::path(PF_RUNTIME_UI_GPU_SHADER_DIRECTORY);
			const auto Vertex = ReadBytes(ShaderRoot / "RuntimeUI.vs.spv");
			const auto Fragment = ReadBytes(ShaderRoot / "RuntimeUI.ps.spv");
			m_Pass = std::make_unique<RuntimeUIRenderPass>(App);
			if (auto Initialized = m_Pass->Initialize(Vertex, Fragment); !Initialized)
				throw std::runtime_error("Could not initialize runtime UI renderer: " + Initialized.error().Message);

			TextureDesc ImageDescription;
			ImageDescription.Width = 1;
			ImageDescription.Height = 1;
			ImageDescription.Format = TextureFormat::RGBA8_Srgb;
			ImageDescription.Usage = TextureUsage::ShaderResource;
			ImageDescription.DebugName = "Runtime UI GPU validation white image";
			const std::array<std::byte, 4> White{ std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff }, std::byte{ 0xff } };
			auto Image = App.CreateTexture(ImageDescription, White);
			if (!Image) throw std::runtime_error("Could not create runtime UI validation image: " + Image.error().Message);
			m_Image = std::move(*Image);

			RenderTargetDesc TargetDescription;
			TargetDescription.Width = 128;
			TargetDescription.Height = 96;
			TargetDescription.ColorFormat = ColorTargetFormat::RGBA8_Srgb;
			TargetDescription.DepthMode = DepthAttachmentMode::None;
			TargetDescription.DebugName = "Runtime UI rendered-pixel validation target";
			auto Target = App.CreateRenderTarget(TargetDescription);
			if (!Target) throw std::runtime_error("Could not create runtime UI readback target: " + Target.error().Message);
			m_Target = std::move(*Target);
			m_FontBytes = ReadBytes(std::filesystem::path(PF_RUNTIME_UI_GPU_FONT));

			m_Viewport = { { 128.0f, 96.0f }, 128, 96 };
			m_Document.DesignSize = m_Viewport.LogicalSize;
			m_Document.Elements.reserve(5);
			auto Background = MakeElement(1, RuntimeUIElementKind::Image, 0, { 0.0f, 0.0f }, { 128.0f, 96.0f });
			Background.Image = m_ImageID;
			Background.Color = { 0.0f, 0.0f, 1.0f, 1.0f };
			m_Document.Elements.push_back(Background);

			auto ClipParent = MakeElement(2, RuntimeUIElementKind::Image, 1, { 0.0f, 0.0f }, { 80.0f, 60.0f });
			ClipParent.Image = m_ImageID;
			ClipParent.Color = { 1.0f, 1.0f, 1.0f, 0.0f };
			ClipParent.ClipChildren = true;
			m_Document.Elements.push_back(ClipParent);

			auto Green = MakeElement(3, RuntimeUIElementKind::Image, 2, { 0.0f, 0.0f }, { 80.0f, 60.0f });
			Green.Parent = ClipParent.ID;
			Green.Image = m_ImageID;
			Green.Color = { 0.0f, 1.0f, 0.0f, 1.0f };
			m_Document.Elements.push_back(Green);

			auto Red = MakeElement(4, RuntimeUIElementKind::Image, 3, { 0.0f, 0.0f }, { 16.0f, 16.0f });
			Red.Parent = ClipParent.ID;
			Red.Image = m_ImageID;
			Red.Color = { 1.0f, 0.0f, 0.0f, 0.5f };
			m_Document.Elements.push_back(Red);

			auto Text = MakeElement(5, RuntimeUIElementKind::Text, 4, { 80.0f, 62.0f }, { 128.0f, 94.0f });
			Text.Font = m_FontID;
			Text.FontSize = 24.0f;
			Text.Text = "Café";
			Text.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			m_Document.Elements.push_back(Text);

			if (auto Valid = ValidateRuntimeUIDocument(m_Document); !Valid)
				throw std::runtime_error("Runtime UI GPU fixture is invalid: " + Valid.error().Message);
			auto Layout = ComputeRuntimeUILayout(m_Document, m_Viewport);
			if (!Layout) throw std::runtime_error("Could not compute runtime UI GPU fixture layout: " + Layout.error().Message);
			m_Commands = BuildRuntimeUIDrawList(m_Document, *Layout, std::nullopt, std::nullopt);
			if (m_Commands.size() != m_Document.Elements.size())
				throw std::runtime_error("Runtime UI GPU fixture did not produce one draw command per element");
		}

		std::chrono::steady_clock::time_point m_Started{};
		RuntimeUIViewport m_Viewport{};
		RuntimeUIDocument m_Document;
		std::vector<RuntimeUIDrawCommand> m_Commands;
		std::vector<std::byte> m_FontBytes;
		std::vector<std::byte> m_FirstFramePixels;
		std::unique_ptr<RuntimeUIRenderPass> m_Pass;
		RenderTargetHandle m_Target;
		TextureHandle m_Image;
		AssetID m_ImageID{ 0xb018000000004000ull, 0x8000000000000041ull };
		AssetID m_FontID{ 0xb018000000004000ull, 0x8000000000000040ull };
		uint32_t m_CompletedFrames = 0;
		bool m_ReadbackPending = false;
		bool m_Succeeded = true;
	};

	class RuntimeUIGpuValidationApplication final : public Application
	{
	public:
		RuntimeUIGpuValidationApplication() : Application(RendererAPI::Vulkan)
		{
			auto TestLayer = std::make_unique<RuntimeUIGpuValidationLayer>();
			m_TestLayer = TestLayer.get();
			PushLayer(std::move(TestLayer));
		}

		int RunValidation()
		{
			Run();
			return m_TestLayer && m_TestLayer->Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
		}

	private:
		RuntimeUIGpuValidationLayer* m_TestLayer = nullptr;
	};
} // namespace

int main()
{
	PulseForge::Log::Init();
	RuntimeUIGpuValidationApplication App;
	return App.RunValidation();
}
