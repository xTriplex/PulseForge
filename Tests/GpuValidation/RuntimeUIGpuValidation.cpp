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
				[this](AssetID Asset) -> const Texture*
				{
					if (Asset == m_ImageID) return m_Image.get();
					if (Asset == m_BackdropImageID) return m_BackdropTexture.get();
					return nullptr;
				},
				[this](AssetID Asset) -> std::expected<std::vector<std::byte>, std::string>
				{
					if (Asset == m_InvalidFontID) return std::vector<std::byte>{ std::byte{ 0x50 }, std::byte{ 0x46 }, std::byte{ 0x00 } };
					if (Asset == m_FontID) return m_FontBytes;
					return std::unexpected("Unexpected font asset in GPU fixture");
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
			const auto DescribePixel = [](const std::array<uint8_t, 4>& Value)
			{
				return "[" + std::to_string(Value[0]) + "," + std::to_string(Value[1]) + "," +
					std::to_string(Value[2]) + "," + std::to_string(Value[3]) + "]";
			};
			const auto Blended = Pixel(8, 8);
			const auto ClippedIn = Pixel(24, 8);
			const auto ClippedOut = Pixel(80, 8);
			if (Blended[0] < 150 || Blended[0] > 220 || Blended[1] < 150 || Blended[1] > 220 || Blended[2] > 20)
			{
				Fail("Overlapping alpha quads did not produce the expected sRGB blended pixel");
				return;
			}
			if (ClippedIn[1] < 220 || ClippedIn[0] > 20 || ClippedIn[2] > 20)
			{
				Fail("The sampled white texel did not preserve the clipped child tint");
				return;
			}
			if (ClippedOut[0] < 220 || ClippedOut[1] < 220 || ClippedOut[2] > 48)
			{
				Fail("The overhanging red child escaped its clip or changed the sampled yellow background: " +
					DescribePixel(ClippedOut));
				return;
			}
			const auto SampledRed = Pixel(48, 8);
			const auto SampledLight = Pixel(80, 50);
			const auto SampledRedLower = Pixel(48, 80);
			const auto TransparentCorner = Pixel(112, 92);
			const auto InvalidFontFallback = Pixel(10, 74);
			// Pixel centers straddle the adjacent nearest-neighbor color regions by a few percent under linear sampling.
			if (SampledRed[0] > 48 || SampledRed[1] > 48 || SampledRed[2] > 48 ||
				SampledLight[0] < 220 || SampledLight[1] < 220 || SampledLight[2] > 48 ||
				SampledRedLower[0] < 220 || SampledRedLower[1] > 48 || SampledRedLower[2] < 220 ||
				TransparentCorner[0] > 20 || TransparentCorner[1] > 20 || TransparentCorner[2] > 20 ||
				InvalidFontFallback[0] < 220 || InvalidFontFallback[1] > 40 || InvalidFontFallback[2] < 220)
			{
				Fail("Image UV/alpha sampling or the visible invalid-font fallback was incorrect: red=" + DescribePixel(SampledRed) +
					", light=" + DescribePixel(SampledLight) + ", lower=" + DescribePixel(SampledRedLower) +
					", transparent=" + DescribePixel(TransparentCorner) + ", fallback=" + DescribePixel(InvalidFontFallback));
				return;
			}

			size_t GlyphStrokePixels = 0;
			for (uint32_t Y = 61; Y < 88; ++Y)
				for (uint32_t X = 80; X < 108; ++X)
				{
					const auto Sample = Pixel(X, Y);
					GlyphStrokePixels += Sample[0] > 220 && Sample[1] > 220 && Sample[2] > 220;
				}
			size_t GlyphCounterPixels = 0;
			for (uint32_t Y = 74; Y < 85; ++Y)
				for (uint32_t X = 88; X < 97; ++X)
				{
					const auto Sample = Pixel(X, Y);
					GlyphCounterPixels += Sample[0] > 220 && Sample[1] < 40 && Sample[2] > 220;
				}
			const auto TransparentGlyphEdge = Pixel(81, 69);
			if (TransparentGlyphEdge[1] > 32)
			{
				Fail("Opaque glyph-atlas padding contaminated a transparent scaled glyph edge: " + DescribePixel(TransparentGlyphEdge));
				return;
			}
			if (GlyphStrokePixels < 12 || GlyphCounterPixels < 4)
			{
				Fail("The O glyph did not preserve both white strokes and the magenta transparent counter");
				return;
			}

			const RuntimeUIRenderStatistics Statistics = m_Pass->GetStatistics();
			if (Statistics.QuadCount < 10 || Statistics.BatchCount < 2 || Statistics.BatchCount >= Statistics.QuadCount ||
				Statistics.TextureCount != 3 || Statistics.FontAtlasCount != 1)
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
				PF_INFO("RUNTIME_UI_GPU=PASS; quads={0}; batches={1}; image_textures={2}; font_atlases={3}; patterned UV/alpha, clipping, invalid-font fallback, and glyph-counter pixels verified",
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
			ImageDescription.Width = 4;
			ImageDescription.Height = 2;
			ImageDescription.Format = TextureFormat::RGBA8_Srgb;
			ImageDescription.Usage = TextureUsage::ShaderResource;
			ImageDescription.DebugName = "Runtime UI GPU validation asymmetric RGBA image";
			const std::array<uint8_t, 32> Pattern{
				255, 255, 255, 255, 255, 0, 0, 255, 255, 255, 0, 255, 255, 0, 255, 0,
				0, 255, 255, 255, 255, 0, 255, 255, 255, 255, 0, 255, 255, 255, 255, 0 };
			std::array<std::byte, Pattern.size()> PatternBytes{};
			for (size_t Index = 0; Index < Pattern.size(); ++Index) PatternBytes[Index] = static_cast<std::byte>(Pattern[Index]);
			auto Image = App.CreateTexture(ImageDescription, PatternBytes);
			if (!Image) throw std::runtime_error("Could not create runtime UI validation image: " + Image.error().Message);
			m_Image = std::move(*Image);
			TextureDesc BackdropDescription;
			BackdropDescription.Width = 1;
			BackdropDescription.Height = 1;
			BackdropDescription.Format = TextureFormat::RGBA8_Srgb;
			BackdropDescription.Usage = TextureUsage::ShaderResource;
			BackdropDescription.DebugName = "Runtime UI GPU validation glyph backdrop";
			const std::array<std::byte, 4> WhitePixel{ std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };
			auto BackdropTexture = App.CreateTexture(BackdropDescription, WhitePixel);
			if (!BackdropTexture)
				throw std::runtime_error("Could not create runtime UI glyph backdrop: " + BackdropTexture.error().Message);
			m_BackdropTexture = std::move(*BackdropTexture);

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
			m_Document.Elements.reserve(8);
			auto Background = MakeElement(1, RuntimeUIElementKind::Image, 0, { 0.0f, 0.0f }, { 128.0f, 96.0f });
			Background.Image = m_ImageID;
			Background.Color = { 1.0f, 1.0f, 1.0f, 1.0f };
			m_Document.Elements.push_back(Background);

			auto ClipParent = MakeElement(2, RuntimeUIElementKind::Image, 1, { 0.0f, 0.0f }, { 72.0f, 60.0f });
			ClipParent.Image = m_ImageID;
			ClipParent.Color = { 1.0f, 1.0f, 1.0f, 0.0f };
			ClipParent.ClipChildren = true;
			m_Document.Elements.push_back(ClipParent);

			auto Green = MakeElement(3, RuntimeUIElementKind::Image, 2, { 0.0f, 0.0f }, { 128.0f, 60.0f });
			Green.Parent = ClipParent.ID;
			Green.Image = m_ImageID;
			Green.Color = { 0.0f, 1.0f, 0.0f, 1.0f };
			m_Document.Elements.push_back(Green);

			auto Red = MakeElement(4, RuntimeUIElementKind::Image, 3, { 0.0f, 0.0f }, { 16.0f, 16.0f });
			Red.Parent = ClipParent.ID;
			Red.Image = m_ImageID;
			Red.Color = { 1.0f, 0.0f, 0.0f, 0.5f };
			m_Document.Elements.push_back(Red);

			auto ClippedRed = MakeElement(5, RuntimeUIElementKind::Image, 4, { 64.0f, 0.0f }, { 96.0f, 16.0f });
			ClippedRed.Parent = ClipParent.ID;
			ClippedRed.Image = m_ImageID;
			ClippedRed.Color = { 1.0f, 0.0f, 0.0f, 1.0f };
			m_Document.Elements.push_back(ClippedRed);

			auto TextBackdrop = MakeElement(6, RuntimeUIElementKind::Image, 5, { 80.0f, 60.0f }, { 110.0f, 90.0f });
			TextBackdrop.Image = m_BackdropImageID;
			TextBackdrop.Color = { 1.0f, 0.0f, 1.0f, 1.0f };
			m_Document.Elements.push_back(TextBackdrop);

			auto Text = MakeElement(7, RuntimeUIElementKind::Text, 6, { 80.0f, 62.0f }, { 128.0f, 94.0f });
			Text.Font = m_FontID;
			Text.FontSize = 30.0f;
			Text.Text = "OéO";
			Text.TextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			m_Document.Elements.push_back(Text);

			auto InvalidFontText = MakeElement(8, RuntimeUIElementKind::Text, 7, { 8.0f, 72.0f }, { 64.0f, 94.0f });
			InvalidFontText.Font = m_InvalidFontID;
			InvalidFontText.FontSize = 40.0f;
			InvalidFontText.Text = "X";
			m_Document.Elements.push_back(InvalidFontText);

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
		TextureHandle m_BackdropTexture;
		AssetID m_ImageID{ 0xb018000000004000ull, 0x8000000000000041ull };
		AssetID m_BackdropImageID{ 0xb018000000004000ull, 0x8000000000000042ull };
		AssetID m_FontID{ 0xb018000000004000ull, 0x8000000000000040ull };
		AssetID m_InvalidFontID{ 0xb018000000004000ull, 0x8000000000000043ull };
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
