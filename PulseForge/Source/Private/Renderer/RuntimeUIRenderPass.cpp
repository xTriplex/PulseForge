#include "Renderer/Detail/RuntimeUIRenderPass.h"

#include "Renderer/ToneMapping.h"
#include "Core/Log.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace PulseForge
{
	namespace
	{
		constexpr size_t MaximumCommands = RuntimeUIDocument::MaximumElements;
		constexpr size_t MaximumQuads = 16384;
		constexpr uint32_t AtlasExtent = 1024;
		constexpr uint32_t RasterPixelSize = 64;
		constexpr size_t MaximumFontBytes = 32u * 1024u * 1024u;
		constexpr size_t MaximumFonts = 8;

		std::optional<ScissorRect> MakeScissor(RuntimeUIRect Clip, float ScaleX, float ScaleY, uint32_t Width, uint32_t Height)
		{
			if (Clip.IsEmpty() || !std::isfinite(ScaleX) || !std::isfinite(ScaleY)) return std::nullopt;
			const double Left = std::clamp(std::floor(static_cast<double>(Clip.Left) * ScaleX), 0.0, static_cast<double>(Width));
			const double Top = std::clamp(std::floor(static_cast<double>(Clip.Top) * ScaleY), 0.0, static_cast<double>(Height));
			const double Right = std::clamp(std::ceil(static_cast<double>(Clip.Right) * ScaleX), 0.0, static_cast<double>(Width));
			const double Bottom = std::clamp(std::ceil(static_cast<double>(Clip.Bottom) * ScaleY), 0.0, static_cast<double>(Height));
			if (!(Right > Left && Bottom > Top)) return std::nullopt;
			return ScissorRect{ static_cast<uint32_t>(Left), static_cast<uint32_t>(Top),
				static_cast<uint32_t>(Right - Left), static_cast<uint32_t>(Bottom - Top) };
		}

		std::vector<uint32_t> DecodeUtf8(std::string_view Text)
		{
			std::vector<uint32_t> Result;
			Result.reserve(Text.size());
			for (size_t Index = 0; Index < Text.size();)
			{
				const uint8_t First = static_cast<uint8_t>(Text[Index]);
				if (First <= 0x7f) { Result.push_back(First); ++Index; continue; }
				const size_t Count = (First & 0xe0u) == 0xc0u ? 1 : (First & 0xf0u) == 0xe0u ? 2 : 3;
				uint32_t Codepoint = First & (Count == 1 ? 0x1fu : Count == 2 ? 0x0fu : 0x07u);
				for (size_t Offset = 1; Offset <= Count; ++Offset)
					Codepoint = (Codepoint << 6) | (static_cast<uint8_t>(Text[Index + Offset]) & 0x3fu);
				Result.push_back(Codepoint);
				Index += Count + 1;
			}
			return Result;
		}

		GraphicsError BackendError(std::string Message)
		{
			return { GraphicsErrorCode::BackendFailure, std::move(Message) };
		}
	}

	struct RuntimeUIRenderPass::FontState final
	{
		struct Glyph final
		{
			float U0 = 0.0f;
			float V0 = 0.0f;
			float U1 = 0.0f;
			float V1 = 0.0f;
			int32_t Width = 0;
			int32_t Height = 0;
			int32_t Left = 0;
			int32_t Top = 0;
			int32_t Advance = 0;
			FT_UInt Index = 0;
		};

		std::vector<std::byte> Bytes;
		FT_Library Library = nullptr;
		FT_Face Face = nullptr;
		TextureHandle Atlas;
		std::unordered_map<uint32_t, Glyph> Glyphs;
		bool WarnedUnsupportedText = false;
		bool WarnedMissingGlyph = false;
		float Ascender = 0.0f;
		float LineHeight = static_cast<float>(RasterPixelSize);

		~FontState()
		{
			if (Face) FT_Done_Face(Face);
			if (Library) FT_Done_FreeType(Library);
		}
	};

	RuntimeUIRenderPass::RuntimeUIRenderPass(RenderDevice& Runtime) noexcept : m_Runtime(Runtime) {}
	RuntimeUIRenderPass::~RuntimeUIRenderPass() = default;

	void RuntimeUIRenderPass::ResetAssetResources() noexcept
	{
		m_PreparedQuads.clear();
		m_LastDrawCount = 0;
		m_TextureBindings.clear();
		m_Fonts.clear();
		m_FailedFonts.clear();
		if (m_WhiteTexture && m_Sampler && m_ConstantsBuffer)
			(void)EnsureTextureBinding(*m_WhiteTexture);
	}

	RuntimeUIRenderStatistics RuntimeUIRenderPass::GetStatistics() const noexcept
	{
		return { .QuadCount = m_PreparedQuads.size(), .BatchCount = m_LastDrawCount,
			.TextureCount = m_TextureBindings.empty() ? 0 : m_TextureBindings.size() - 1,
			.FontAtlasCount = m_Fonts.size(), .TextureBindingCreationCount = m_TextureBindingCreationCount };
	}

	std::expected<void, GraphicsError> RuntimeUIRenderPass::Initialize(
		std::span<const std::byte> VertexBytecode, std::span<const std::byte> FragmentBytecode)
	{
		ShaderDesc VertexDescription;
		VertexDescription.Stage = ShaderStage::Vertex;
		VertexDescription.EntryPoint = "VSMain";
		VertexDescription.DebugName = "PulseForge runtime UI vertex shader";
		auto VertexShader = m_Runtime.CreateShader(VertexDescription, VertexBytecode);
		if (!VertexShader) return std::unexpected(VertexShader.error());
		m_VertexShader = std::move(*VertexShader);

		ShaderDesc FragmentDescription;
		FragmentDescription.Stage = ShaderStage::Fragment;
		FragmentDescription.EntryPoint = "PSMain";
		FragmentDescription.DebugName = "PulseForge runtime UI fragment shader";
		auto FragmentShader = m_Runtime.CreateShader(FragmentDescription, FragmentBytecode);
		if (!FragmentShader) return std::unexpected(FragmentShader.error());
		m_FragmentShader = std::move(*FragmentShader);

		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "PulseForge runtime UI texture and output encoding";
		auto Layout = m_Runtime.CreateBindingLayout(LayoutDescription);
		if (!Layout) return std::unexpected(BackendError(Layout.error().Message));
		m_Layout = std::move(*Layout);

		SamplerDesc SamplerDescription;
		SamplerDescription.Minification = SamplerFilter::Linear;
		SamplerDescription.Magnification = SamplerFilter::Linear;
		SamplerDescription.AddressU = SamplerAddressMode::ClampToEdge;
		SamplerDescription.AddressV = SamplerAddressMode::ClampToEdge;
		SamplerDescription.DebugName = "PulseForge runtime UI clamp sampler";
		auto Sampler = m_Runtime.CreateSampler(SamplerDescription);
		if (!Sampler) return std::unexpected(BackendError(Sampler.error().Message));
		m_Sampler = std::move(*Sampler);

		const std::array<uint8_t, 4> White{ 255, 255, 255, 255 };
		TextureDesc WhiteDescription;
		WhiteDescription.Width = 1;
		WhiteDescription.Height = 1;
		WhiteDescription.Format = TextureFormat::RGBA8_UNorm;
		WhiteDescription.DebugName = "PulseForge runtime UI white texel";
		auto WhiteTexture = m_Runtime.CreateTexture(WhiteDescription, std::as_bytes(std::span(White)));
		if (!WhiteTexture) return std::unexpected(BackendError(WhiteTexture.error().Message));
		m_WhiteTexture = std::move(*WhiteTexture);

		BufferDesc VertexDescriptionBuffer;
		VertexDescriptionBuffer.ByteSize = sizeof(Vertex) * MaximumQuads * 4;
		VertexDescriptionBuffer.Usage = BufferUsage::Vertex;
		VertexDescriptionBuffer.IsDynamic = true;
		VertexDescriptionBuffer.DebugName = "PulseForge runtime UI vertices";
		auto Vertices = m_Runtime.CreateBuffer(VertexDescriptionBuffer);
		if (!Vertices) return std::unexpected(BackendError(Vertices.error().Message));
		m_VertexBuffer = std::move(*Vertices);

		BufferDesc IndexDescription;
		IndexDescription.ByteSize = sizeof(uint32_t) * MaximumQuads * 6;
		IndexDescription.Usage = BufferUsage::Index;
		IndexDescription.IsDynamic = true;
		IndexDescription.DebugName = "PulseForge runtime UI indices";
		std::vector<uint32_t> Indices(MaximumQuads * 6);
		for (uint32_t Index = 0; Index < MaximumQuads; ++Index)
		{
			const uint32_t Vertex = Index * 4;
			const size_t Offset = static_cast<size_t>(Index) * 6;
			Indices[Offset + 0] = Vertex + 0;
			Indices[Offset + 1] = Vertex + 1;
			Indices[Offset + 2] = Vertex + 2;
			Indices[Offset + 3] = Vertex + 2;
			Indices[Offset + 4] = Vertex + 1;
			Indices[Offset + 5] = Vertex + 3;
		}
		auto IndexBuffer = m_Runtime.CreateBuffer(IndexDescription, std::as_bytes(std::span(Indices)));
		if (!IndexBuffer) return std::unexpected(BackendError(IndexBuffer.error().Message));
		m_IndexBuffer = std::move(*IndexBuffer);

		BufferDesc ConstantsDescription;
		ConstantsDescription.ByteSize = sizeof(Constants);
		ConstantsDescription.Usage = BufferUsage::Constant;
		ConstantsDescription.DebugName = "PulseForge runtime UI constants";
		auto ConstantsBuffer = m_Runtime.CreateBuffer(ConstantsDescription, std::as_bytes(std::span(&m_Constants, 1)));
		if (!ConstantsBuffer) return std::unexpected(BackendError(ConstantsBuffer.error().Message));
		m_ConstantsBuffer = std::move(*ConstantsBuffer);
		return EnsureTextureBinding(*m_WhiteTexture);
	}

	RuntimeUIRenderPass::FontState* RuntimeUIRenderPass::EnsureFont(AssetID Font, const FontResolver& ResolveFont)
	{
		if (const auto Found = m_Fonts.find(Font); Found != m_Fonts.end()) return Found->second.get();
		if (m_FailedFonts.contains(Font)) return nullptr;
		const auto Failed = [this, Font](std::string_view Reason) -> FontState*
		{
			m_FailedFonts.insert(Font);
			PF_CORE_WARN("Runtime UI font {} could not be prepared: {}. Visible fallback glyphs will be used.", Font.ToString(), Reason);
			return nullptr;
		};
		if (m_Fonts.size() >= MaximumFonts) return Failed("the eight-font runtime limit was reached");
		if (!ResolveFont) return Failed("no managed font resolver is available");
		auto FontBytes = ResolveFont(Font);
		if (!FontBytes || FontBytes->empty() || FontBytes->size() > MaximumFontBytes ||
			FontBytes->size() > static_cast<size_t>((std::numeric_limits<FT_Long>::max)()))
			return Failed(FontBytes ? "font bytes are empty or exceed the 32 MiB limit" : FontBytes.error());

		auto State = std::make_unique<FontState>();
		State->Bytes = std::move(*FontBytes);
		if (FT_Init_FreeType(&State->Library) != 0 ||
			FT_New_Memory_Face(State->Library, reinterpret_cast<const FT_Byte*>(State->Bytes.data()),
				static_cast<FT_Long>(State->Bytes.size()), 0, &State->Face) != 0 ||
			FT_Set_Pixel_Sizes(State->Face, 0, RasterPixelSize) != 0)
			return Failed("FreeType rejected the font data or raster size");
		State->Ascender = static_cast<float>(State->Face->size->metrics.ascender) / 64.0f;
		State->LineHeight = (std::max)(1.0f, static_cast<float>(State->Face->size->metrics.height) / 64.0f);

		std::vector<uint8_t> Pixels(static_cast<size_t>(AtlasExtent) * AtlasExtent * 4, 0);
		for (size_t Pixel = 0; Pixel < static_cast<size_t>(AtlasExtent) * AtlasExtent; ++Pixel)
		{
			Pixels[Pixel * 4] = 255;
			Pixels[Pixel * 4 + 1] = 255;
			Pixels[Pixel * 4 + 2] = 255;
		}
		uint32_t X = 2;
		uint32_t Y = 2;
		uint32_t RowHeight = 0;
		for (uint32_t Codepoint = 32; Codepoint <= 255; ++Codepoint)
		{
			const FT_UInt GlyphIndex = FT_Get_Char_Index(State->Face, Codepoint);
			if (GlyphIndex == 0 || FT_Load_Glyph(State->Face, GlyphIndex, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) continue;
			const FT_GlyphSlot Slot = State->Face->glyph;
			const uint32_t Width = Slot->bitmap.width;
			const uint32_t Height = Slot->bitmap.rows;
			if (Width >= AtlasExtent - 4 || Height >= AtlasExtent - 4)
				return Failed("a glyph exceeds the bounded atlas dimensions");
			if (X + Width + 2 >= AtlasExtent) { X = 2; Y += RowHeight + 2; RowHeight = 0; }
			if (Y + Height + 2 >= AtlasExtent) return Failed("the bounded glyph atlas is full");
			FontState::Glyph Glyph;
			Glyph.U0 = static_cast<float>(X) / AtlasExtent;
			Glyph.V0 = static_cast<float>(Y) / AtlasExtent;
			Glyph.U1 = static_cast<float>(X + Width) / AtlasExtent;
			Glyph.V1 = static_cast<float>(Y + Height) / AtlasExtent;
			Glyph.Width = static_cast<int32_t>(Width);
			Glyph.Height = static_cast<int32_t>(Height);
			Glyph.Left = Slot->bitmap_left;
			Glyph.Top = Slot->bitmap_top;
			Glyph.Advance = static_cast<int32_t>(Slot->advance.x >> 6);
			Glyph.Index = GlyphIndex;
			if (Width > 0 && Height > 0)
			{
				const int64_t SignedPitch = Slot->bitmap.pitch;
				const size_t Pitch = static_cast<size_t>(SignedPitch < 0 ? -SignedPitch : SignedPitch);
				const size_t RequiredPitch = Slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO ? (Width + 7u) / 8u : Width;
				if (!Slot->bitmap.buffer || Pitch < RequiredPitch)
					return Failed("FreeType returned an incomplete glyph bitmap");
				if (Slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && Slot->bitmap.pixel_mode != FT_PIXEL_MODE_MONO)
					return Failed("FreeType returned an unsupported glyph bitmap pixel mode");
				if (Slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY && Slot->bitmap.num_grays < 2)
					return Failed("FreeType returned an invalid grayscale coverage range");
				for (uint32_t Row = 0; Row < Height; ++Row)
				{
					const uint32_t SourceRow = SignedPitch >= 0 ? Row : Height - Row - 1;
					const uint8_t* Source = Slot->bitmap.buffer + static_cast<size_t>(SourceRow) * Pitch;
					for (uint32_t Column = 0; Column < Width; ++Column)
					{
						uint8_t Coverage = 0;
						if (Slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
							Coverage = static_cast<uint8_t>(static_cast<uint32_t>(Source[Column]) * 255u /
								(static_cast<uint32_t>(Slot->bitmap.num_grays) - 1u));
						else
							Coverage = (Source[Column >> 3] & (0x80u >> (Column & 7u))) ? 255 : 0;
						Pixels[(static_cast<size_t>(Y + Row) * AtlasExtent + X + Column) * 4 + 3] = Coverage;
					}
				}
			}
			State->Glyphs.emplace(Codepoint, Glyph);
			X += (std::max)(Width, 1u) + 2;
			RowHeight = (std::max)(RowHeight, Height);
		}
		if (!State->Glyphs.contains('?')) return Failed("the font does not provide a replacement glyph");
		TextureDesc Description;
		Description.Width = AtlasExtent;
		Description.Height = AtlasExtent;
		Description.Format = TextureFormat::RGBA8_UNorm;
		Description.DebugName = "PulseForge runtime UI Latin-1 glyph atlas";
		auto Atlas = m_Runtime.CreateTexture(Description, std::as_bytes(std::span(Pixels)));
		if (!Atlas) return Failed(Atlas.error().Message);
		State->Atlas = std::move(*Atlas);
		FontState* Result = State.get();
		m_Fonts.emplace(Font, std::move(State));
		return Result;
	}

	std::expected<void, GraphicsError> RuntimeUIRenderPass::EnsureTextureBinding(const Texture& TextureValue)
	{
		if (m_TextureBindings.contains(&TextureValue)) return {};
		if (m_TextureBindings.size() >= MaximumFonts + MaximumCommands + 1)
			return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDescription, "Runtime UI texture binding limit exceeded" });
		BindingSetDesc Description;
		Description.Layout = m_Layout;
		Description.Textures.push_back({ 0, TextureValue });
		Description.Samplers.push_back({ 0, *m_Sampler });
		Description.Buffers.push_back({ 0, *m_ConstantsBuffer });
		auto Set = m_Runtime.CreateBindingSet(Description);
		if (!Set) return std::unexpected(BackendError(Set.error().Message));
		m_TextureBindings.emplace(&TextureValue, std::move(*Set));
		++m_TextureBindingCreationCount;
		return {};
	}

	std::expected<void, GraphicsError> RuntimeUIRenderPass::Prepare(
		std::span<const RuntimeUIDrawCommand> Commands, const RuntimeUIViewport& Viewport,
		const ImageResolver& ResolveImage, const FontResolver& ResolveFont)
	{
		m_PreparedQuads.clear();
		m_LastDrawCount = 0;
		if (Commands.size() > MaximumCommands)
			return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, "Runtime UI draw list exceeds 2048 items" });
		if (Viewport.LogicalSize.x <= 0.0f || Viewport.LogicalSize.y <= 0.0f) return {};
		const auto AppendQuad = [&](RuntimeUIRect Bounds, RuntimeUIRect Clip, glm::vec4 Color,
			const Texture& TextureValue, glm::vec2 UvMinimum, glm::vec2 UvMaximum) -> bool
		{
			if (Bounds.IsEmpty() || Clip.IsEmpty() || m_PreparedQuads.size() >= MaximumQuads) return false;
			PreparedQuad Quad;
			Quad.Bounds = Bounds;
			Quad.Clip = Clip;
			Quad.Color = Color;
			Quad.UvMinimum = UvMinimum;
			Quad.UvMaximum = UvMaximum;
			Quad.Resource = &TextureValue;
			m_PreparedQuads.push_back(std::move(Quad));
			return true;
		};
		const auto AppendText = [&](std::string_view Text, RuntimeUIRect Bounds, RuntimeUIRect Clip,
			glm::vec4 Color, float FontSize, FontState* Font) -> bool
		{
			if (!Font)
			{
				float X = Bounds.Left;
				for (uint32_t Codepoint : DecodeUtf8(Text))
				{
					if (Codepoint == '\n' || Codepoint == '\r') continue;
					RuntimeUIRect Fallback{ X, Bounds.Top, X + (std::max)(4.0f, FontSize * 0.15f),
						Bounds.Top + (std::max)(4.0f, FontSize * 0.15f) };
					if (!AppendQuad(Fallback, Clip, { 1.0f, 0.0f, 1.0f, Color.a }, *m_WhiteTexture, { 0.0f, 0.0f }, { 1.0f, 1.0f })) return false;
					X += (std::max)(6.0f, FontSize * 0.3f);
				}
				return true;
			}
			const float Scale = FontSize / static_cast<float>(RasterPixelSize);
			const float StartX = Bounds.Left;
			float PenX = StartX;
			float Baseline = Bounds.Top + Font->Ascender * Scale;
			FT_UInt PreviousGlyph = 0;
			for (uint32_t Codepoint : DecodeUtf8(Text))
			{
				if (Codepoint == '\r') continue;
				if (Codepoint == '\n')
				{
					PenX = StartX;
					Baseline += Font->LineHeight * Scale;
					PreviousGlyph = 0;
					continue;
				}
				if (Codepoint == '\t') Codepoint = ' ';
				if (Codepoint > 0xff)
				{
					if (!Font->WarnedUnsupportedText)
					{
						PF_CORE_WARN("Runtime UI font has unsupported text beyond Latin-1; replacement glyphs are shown");
						Font->WarnedUnsupportedText = true;
					}
					Codepoint = '?';
				}
				auto GlyphIt = Font->Glyphs.find(Codepoint);
				if (GlyphIt == Font->Glyphs.end())
				{
					if (!Font->WarnedMissingGlyph)
					{
						PF_CORE_WARN("Runtime UI font is missing glyph U+{0:04X}; the replacement glyph is shown", Codepoint);
						Font->WarnedMissingGlyph = true;
					}
					GlyphIt = Font->Glyphs.find('?');
				}
				if (GlyphIt == Font->Glyphs.end()) continue;
				const FontState::Glyph& Glyph = GlyphIt->second;
				if (PreviousGlyph != 0 && Glyph.Index != 0 && FT_HAS_KERNING(Font->Face))
				{
					FT_Vector Kerning{};
					if (FT_Get_Kerning(Font->Face, PreviousGlyph, Glyph.Index, FT_KERNING_DEFAULT, &Kerning) == 0)
						PenX += static_cast<float>(Kerning.x >> 6) * Scale;
				}
				if (PenX > StartX && PenX + static_cast<float>(Glyph.Advance) * Scale > Bounds.Right)
				{
					PenX = StartX;
					Baseline += Font->LineHeight * Scale;
					PreviousGlyph = 0;
				}
				if (Glyph.Width > 0 && Glyph.Height > 0)
				{
					const float Left = PenX + static_cast<float>(Glyph.Left) * Scale;
					const float Top = Baseline - static_cast<float>(Glyph.Top) * Scale;
					const RuntimeUIRect GlyphBounds{ Left, Top, Left + static_cast<float>(Glyph.Width) * Scale,
						Top + static_cast<float>(Glyph.Height) * Scale };
					if (!AppendQuad(GlyphBounds, Clip, Color, *Font->Atlas, { Glyph.U0, Glyph.V0 }, { Glyph.U1, Glyph.V1 }))
						return false;
				}
				PenX += static_cast<float>(Glyph.Advance) * Scale;
				PreviousGlyph = Glyph.Index;
			}
			return true;
		};

		for (const RuntimeUIDrawCommand& Command : Commands)
		{
			if (Command.Bounds.IsEmpty() || Command.Clip.IsEmpty()) continue;
			const Texture* Image = Command.Image.IsNil() || !ResolveImage ? nullptr : ResolveImage(Command.Image);
			if (Command.Kind == RuntimeUIElementKind::Image)
			{
				const Texture& Resource = Image ? *Image : *m_WhiteTexture;
				const glm::vec4 Tint = Image ? Command.Color : glm::vec4{ 1.0f, 0.0f, 1.0f, Command.Color.a };
				if (!AppendQuad(Command.Bounds, Command.Clip, Tint, Resource, { 0.0f, 0.0f }, { 1.0f, 1.0f }))
					return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, "Runtime UI draw list exceeds the 16384 quad limit" });
				continue;
			}
			if (Command.Kind == RuntimeUIElementKind::Button)
			{
				const Texture& Resource = Image ? *Image : *m_WhiteTexture;
				glm::vec4 Background = Command.Color;
				if (!Image && Command.Focused)
					Background = { (std::min)(Background.r * 1.2f, 1.0f), (std::min)(Background.g * 1.2f, 1.0f),
						(std::min)(Background.b * 1.2f, 1.0f), Background.a };
				if (!AppendQuad(Command.Bounds, Command.Clip, Background, Resource, { 0.0f, 0.0f }, { 1.0f, 1.0f }))
					return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, "Runtime UI draw list exceeds the 16384 quad limit" });
			}
			if (Command.Text.empty()) continue;
			FontState* Font = Command.Font.IsNil() ? nullptr : EnsureFont(Command.Font, ResolveFont);
			const glm::vec4 TextColor = Command.Focused && Command.Kind == RuntimeUIElementKind::Text
				? Command.Color : Command.TextColor;
			if (!AppendText(Command.Text, Command.Bounds, Command.Clip, TextColor, Command.FontSize, Font))
				return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, "Runtime UI text exceeds the 16384 glyph limit" });
		}

		for (const PreparedQuad& Quad : m_PreparedQuads)
			if (auto Bound = EnsureTextureBinding(*Quad.Resource); !Bound) return Bound;
		return {};
	}

	std::expected<GraphicsPipeline*, GraphicsError> RuntimeUIRenderPass::EnsurePipeline(
		ColorTargetFormat TargetFormat, bool DepthAttachmentEnabled)
	{
		const auto Key = std::pair{ TargetFormat, DepthAttachmentEnabled };
		if (const auto Found = m_Pipelines.find(Key); Found != m_Pipelines.end()) return Found->second.get();
		GraphicsPipelineDesc Description;
		Description.VertexShader = m_VertexShader;
		Description.FragmentShader = m_FragmentShader;
		Description.BindingLayouts = { m_Layout };
		Description.VertexLayout.Stride = sizeof(Vertex);
		Description.VertexLayout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float2, offsetof(Vertex, Position) },
			{ VertexSemantic::Color, VertexFormat::Float4, offsetof(Vertex, Color) },
			{ VertexSemantic::TexCoord, VertexFormat::Float2, offsetof(Vertex, UV) } };
		Description.ColorFormat = TargetFormat;
		Description.DepthAttachmentEnabled = DepthAttachmentEnabled;
		Description.Rasterizer.Cull = CullMode::None;
		Description.Rasterizer.ScissorEnabled = true;
		Description.Blend.Enabled = true;
		Description.Depth.TestEnabled = false;
		Description.Depth.WriteEnabled = false;
		Description.DebugName = "PulseForge screen-space runtime UI";
		auto Pipeline = m_Runtime.CreateGraphicsPipeline(Description);
		if (!Pipeline) return std::unexpected(Pipeline.error());
		auto* Result = Pipeline->get();
		m_Pipelines.emplace(Key, std::move(*Pipeline));
		return Result;
	}

	std::expected<size_t, GraphicsError> RuntimeUIRenderPass::Draw(
		std::span<const RuntimeUIDrawCommand>, const RuntimeUIViewport& Viewport,
		uint32_t TargetWidth, uint32_t TargetHeight, ColorTargetFormat TargetFormat, bool DepthAttachmentEnabled)
	{
		m_LastDrawCount = 0;
		if (m_PreparedQuads.empty() || Viewport.LogicalSize.x <= 0.0f || Viewport.LogicalSize.y <= 0.0f || TargetWidth == 0 || TargetHeight == 0)
			return size_t{ 0 };
		auto Pipeline = EnsurePipeline(TargetFormat, DepthAttachmentEnabled);
		if (!Pipeline) return std::unexpected(Pipeline.error());

		std::vector<Vertex> Vertices;
		std::vector<std::optional<ScissorRect>> Scissors;
		Vertices.reserve(m_PreparedQuads.size() * 4);
		Scissors.reserve(m_PreparedQuads.size());
		const float ScaleX = static_cast<float>(TargetWidth) / Viewport.LogicalSize.x;
		const float ScaleY = static_cast<float>(TargetHeight) / Viewport.LogicalSize.y;
		const auto ToClip = [&](float X, float Y) { return glm::vec2{ X / Viewport.LogicalSize.x * 2.0f - 1.0f,
			1.0f - Y / Viewport.LogicalSize.y * 2.0f }; };
		for (const PreparedQuad& Quad : m_PreparedQuads)
		{
			const auto Scissor = MakeScissor(Quad.Clip, ScaleX, ScaleY, TargetWidth, TargetHeight);
			Scissors.push_back(Scissor);
			const glm::vec2 TopLeft = ToClip(Quad.Bounds.Left, Quad.Bounds.Top);
			const glm::vec2 TopRight = ToClip(Quad.Bounds.Right, Quad.Bounds.Top);
			const glm::vec2 BottomLeft = ToClip(Quad.Bounds.Left, Quad.Bounds.Bottom);
			const glm::vec2 BottomRight = ToClip(Quad.Bounds.Right, Quad.Bounds.Bottom);
			Vertices.push_back({ TopLeft, Quad.Color, Quad.UvMinimum });
			Vertices.push_back({ TopRight, Quad.Color, { Quad.UvMaximum.x, Quad.UvMinimum.y } });
			Vertices.push_back({ BottomLeft, Quad.Color, { Quad.UvMinimum.x, Quad.UvMaximum.y } });
			Vertices.push_back({ BottomRight, Quad.Color, Quad.UvMaximum });
		}
		if (auto Updated = m_Runtime.WriteBuffer(*m_VertexBuffer, 0, std::as_bytes(std::span(Vertices))); !Updated)
			return std::unexpected(BackendError(Updated.error().Message));
		m_Constants.EncodeSrgb = ShouldShaderEncodeSrgb(TargetFormat, m_Runtime.GetOutputColorEncoding()) ? 1u : 0u;
		if (auto Updated = m_Runtime.WriteBuffer(*m_ConstantsBuffer, 0, std::as_bytes(std::span(&m_Constants, 1))); !Updated)
			return std::unexpected(BackendError(Updated.error().Message));

		size_t DrawCount = 0;
		for (size_t Index = 0; Index < m_PreparedQuads.size();)
		{
			if (!Scissors[Index]) { ++Index; continue; }
			const ScissorRect Clip = *Scissors[Index];
			const Texture* Resource = m_PreparedQuads[Index].Resource;
			size_t End = Index + 1;
			while (End < m_PreparedQuads.size() && Scissors[End] && m_PreparedQuads[End].Resource == Resource &&
				Scissors[End]->X == Clip.X && Scissors[End]->Y == Clip.Y && Scissors[End]->Width == Clip.Width &&
				Scissors[End]->Height == Clip.Height) ++End;
			const auto Binding = m_TextureBindings.find(Resource);
			if (Binding == m_TextureBindings.end())
				return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDescription, "Runtime UI texture binding was not prepared" });
			const std::array<const BindingSet*, 1> Sets{ Binding->second.get() };
			DrawIndexedArguments Arguments;
			Arguments.IndexCount = static_cast<uint32_t>((End - Index) * 6);
			Arguments.FirstIndex = static_cast<uint32_t>(Index * 6);
			Arguments.Scissor = Clip;
			if (const auto Draw = m_Runtime.DrawIndexed(**Pipeline, *m_VertexBuffer, *m_IndexBuffer, Arguments, Sets); !Draw)
				return std::unexpected(Draw.error());
			++DrawCount;
			Index = End;
		}
		m_LastDrawCount = DrawCount;
		return DrawCount;
	}
}
