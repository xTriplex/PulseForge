#include "Editor/EditorImGuiRenderer.h"

#include "Core/Application.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <span>

namespace PulseForgeEditor
{
	namespace
	{
		std::expected<std::vector<std::byte>, std::string> ReadShaderBytecode(const std::filesystem::path& Path)
		{
			std::ifstream Input(Path, std::ios::binary | std::ios::ate);
			if (!Input)
				return std::unexpected("Could not open editor shader: " + Path.string());

			const std::streampos End = Input.tellg();
			if (End <= 0 || static_cast<uint64_t>(End) > (std::numeric_limits<size_t>::max)())
				return std::unexpected("Editor shader has an invalid bytecode size: " + Path.string());

			const size_t Size = static_cast<size_t>(End);
			if (Size % sizeof(uint32_t) != 0)
				return std::unexpected("Editor SPIR-V shader is not word-aligned: " + Path.string());

			std::vector<std::byte> Bytecode(Size);
			Input.seekg(0, std::ios::beg);
			Input.read(reinterpret_cast<char*>(Bytecode.data()), static_cast<std::streamsize>(Bytecode.size()));
			if (!Input)
				return std::unexpected("Could not read editor shader: " + Path.string());
			return Bytecode;
		}

		float ColorChannel(ImU32 Color, int Shift)
		{
			return static_cast<float>((Color >> Shift) & 0xffu) / 255.0f;
		}
	}

	EditorImGuiRenderer::~EditorImGuiRenderer()
	{
		Shutdown();
	}

	std::expected<void, std::string> EditorImGuiRenderer::Initialize(
		PulseForge::Application& Runtime,
		const std::filesystem::path& ShaderDirectory)
	{
		if (m_Initialized)
			return std::unexpected("The editor ImGui renderer is already initialized");

		try
		{
			m_Runtime = &Runtime;
			const auto VertexBytecode = ReadShaderBytecode(ShaderDirectory / "EditorImGui.vs.spv");
			if (!VertexBytecode)
				return std::unexpected(VertexBytecode.error());
			const auto FragmentBytecode = ReadShaderBytecode(ShaderDirectory / "EditorImGui.ps.spv");
			if (!FragmentBytecode)
				return std::unexpected(FragmentBytecode.error());

			PulseForge::ShaderDesc VertexDescription;
			VertexDescription.Stage = PulseForge::ShaderStage::Vertex;
			VertexDescription.EntryPoint = "VSMain";
			VertexDescription.DebugName = "PulseForge editor ImGui vertex shader";
			auto VertexShader = Runtime.CreateShader(VertexDescription, *VertexBytecode);
			if (!VertexShader)
				return std::unexpected("Could not create editor vertex shader: " + VertexShader.error().Message);
			m_VertexShader = std::move(*VertexShader);

			PulseForge::ShaderDesc FragmentDescription;
			FragmentDescription.Stage = PulseForge::ShaderStage::Fragment;
			FragmentDescription.EntryPoint = "PSMain";
			FragmentDescription.DebugName = "PulseForge editor ImGui fragment shader";
			auto FragmentShader = Runtime.CreateShader(FragmentDescription, *FragmentBytecode);
			if (!FragmentShader)
				return std::unexpected("Could not create editor fragment shader: " + FragmentShader.error().Message);
			m_FragmentShader = std::move(*FragmentShader);

			PulseForge::BindingLayoutDesc LayoutDescription;
			LayoutDescription.Visibility = PulseForge::ShaderVisibility::Fragment;
			LayoutDescription.Items = {
				{ PulseForge::BindingResourceType::Texture2D, 0 },
				{ PulseForge::BindingResourceType::Sampler, 0 }
			};
			LayoutDescription.DebugName = "PulseForge editor ImGui texture";
			auto Layout = Runtime.CreateBindingLayout(LayoutDescription);
			if (!Layout)
				return std::unexpected("Could not create editor texture layout: " + Layout.error().Message);
			m_BindingLayout = std::move(*Layout);

			PulseForge::SamplerDesc SamplerDescription;
			SamplerDescription.Minification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.Magnification = PulseForge::SamplerFilter::Linear;
			SamplerDescription.AddressU = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.AddressV = PulseForge::SamplerAddressMode::ClampToEdge;
			SamplerDescription.DebugName = "PulseForge editor ImGui sampler";
			auto Sampler = Runtime.CreateSampler(SamplerDescription);
			if (!Sampler)
				return std::unexpected("Could not create editor sampler: " + Sampler.error().Message);
			m_Sampler = std::move(*Sampler);

			PulseForge::GraphicsPipelineDesc PipelineDescription;
			PipelineDescription.VertexShader = m_VertexShader;
			PipelineDescription.FragmentShader = m_FragmentShader;
			PipelineDescription.BindingLayouts = { m_BindingLayout };
			PipelineDescription.VertexLayout.Stride = sizeof(Vertex);
			PipelineDescription.VertexLayout.Attributes = {
				{ PulseForge::VertexSemantic::Position, PulseForge::VertexFormat::Float2, offsetof(Vertex, Position) },
				{ PulseForge::VertexSemantic::TexCoord, PulseForge::VertexFormat::Float2, offsetof(Vertex, TexCoord) },
				{ PulseForge::VertexSemantic::Color, PulseForge::VertexFormat::Float4, offsetof(Vertex, Color) }
			};
			PipelineDescription.ColorFormat = PulseForge::ColorTargetFormat::Swapchain;
			PipelineDescription.Rasterizer.ScissorEnabled = true;
			PipelineDescription.Blend.Enabled = true;
			PipelineDescription.DebugName = "PulseForge editor ImGui pipeline";
			auto Pipeline = Runtime.CreateGraphicsPipeline(PipelineDescription);
			if (!Pipeline)
				return std::unexpected("Could not create editor graphics pipeline: " + Pipeline.error().Message);
			m_Pipeline = std::move(*Pipeline);

			ImGuiIO& IO = ImGui::GetIO();
			IO.BackendRendererName = "PulseForgeEditorRenderer";
			IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
			unsigned char* Pixels = nullptr;
			int Width = 0;
			int Height = 0;
			int BytesPerPixel = 0;
			IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height, &BytesPerPixel);
			if (!Pixels || Width <= 0 || Height <= 0 || BytesPerPixel != 4)
				return std::unexpected("Dear ImGui did not provide a valid RGBA font atlas");

			const uint64_t PixelCount = static_cast<uint64_t>(Width) * static_cast<uint64_t>(Height);
			if (PixelCount > (std::numeric_limits<size_t>::max)() / 4)
				return std::unexpected("Dear ImGui font atlas dimensions overflow the upload size");

			PulseForge::TextureDesc FontDescription;
			FontDescription.Width = static_cast<uint32_t>(Width);
			FontDescription.Height = static_cast<uint32_t>(Height);
			FontDescription.Format = PulseForge::TextureFormat::RGBA8_UNorm;
			FontDescription.Usage = PulseForge::TextureUsage::ShaderResource;
			FontDescription.DebugName = "PulseForge editor ImGui font atlas";
			const size_t PixelBytes = static_cast<size_t>(PixelCount) * 4;
			auto Font = Runtime.CreateTexture(
				FontDescription,
				std::span<const std::byte>(reinterpret_cast<const std::byte*>(Pixels), PixelBytes));
			if (!Font)
				return std::unexpected("Could not upload editor font atlas: " + Font.error().Message);
			m_FontTexture = std::move(*Font);

			auto FontTextureID = RegisterTexture(*m_FontTexture);
			if (!FontTextureID)
				return std::unexpected("Could not bind editor font atlas: " + FontTextureID.error());
			m_FontTextureID = *FontTextureID;
			IO.Fonts->SetTexID(static_cast<ImTextureID>(m_FontTextureID));
			m_Initialized = true;
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Editor ImGui renderer initialization failed: ") + Exception.what());
		}
	}

	std::expected<uint64_t, std::string> EditorImGuiRenderer::RegisterTexture(const PulseForge::Texture& Texture)
	{
		if (!m_Runtime || !m_BindingLayout || !m_Sampler)
			return std::unexpected("Editor texture renderer is not initialized");
		if (m_NextTextureID == (std::numeric_limits<uint64_t>::max)())
			return std::unexpected("Editor texture identifier space is exhausted");

		PulseForge::BindingSetDesc Description;
		Description.Layout = m_BindingLayout;
		Description.Textures.push_back({ 0, std::cref(Texture) });
		Description.Samplers.push_back({ 0, std::cref(*m_Sampler) });
		auto Binding = m_Runtime->CreateBindingSet(Description);
		if (!Binding)
			return std::unexpected("Could not create editor texture binding: " + Binding.error().Message);

		const uint64_t TextureID = m_NextTextureID++;
		m_TextureBindings.emplace(TextureID, std::move(*Binding));
		return TextureID;
	}

	void EditorImGuiRenderer::UnregisterTexture(uint64_t TextureID) noexcept
	{
		if (TextureID != m_FontTextureID)
			m_TextureBindings.erase(TextureID);
	}

	std::expected<void, std::string> EditorImGuiRenderer::EnsureBuffers(size_t VertexBytes, size_t IndexBytes)
	{
		const auto Ensure = [this](
			PulseForge::BufferHandle& Buffer,
			uint64_t& Capacity,
			uint64_t Required,
			PulseForge::BufferUsage Usage,
			const char* Name) -> std::expected<void, std::string>
		{
			if (Required == 0 || Required <= Capacity)
				return {};

			uint64_t NewCapacity = (std::max)(uint64_t{ 4096 }, Capacity);
			while (NewCapacity < Required)
			{
				if (NewCapacity > (std::numeric_limits<uint64_t>::max)() / 2)
				{
					NewCapacity = Required;
					break;
				}
				NewCapacity *= 2;
			}

			PulseForge::BufferDesc Description;
			Description.ByteSize = NewCapacity;
			Description.Usage = Usage;
			Description.IsDynamic = true;
			Description.DebugName = Name;
			auto Created = m_Runtime->CreateBuffer(Description);
			if (!Created)
				return std::unexpected(std::string("Could not grow editor ") + Name + ": " + Created.error().Message);

			Buffer = std::move(*Created);
			Capacity = NewCapacity;
			return {};
		};

		if (auto Result = Ensure(
			m_VertexBuffer,
			m_VertexBufferCapacity,
			static_cast<uint64_t>(VertexBytes),
			PulseForge::BufferUsage::Vertex,
			"ImGui dynamic vertices"); !Result)
			return Result;

		return Ensure(
			m_IndexBuffer,
			m_IndexBufferCapacity,
			static_cast<uint64_t>(IndexBytes),
			PulseForge::BufferUsage::Index,
			"ImGui dynamic indices");
	}

	std::expected<void, std::string> EditorImGuiRenderer::RenderDrawData()
	{
		if (!m_Initialized || !m_Runtime || !m_Pipeline)
			return std::unexpected("Editor ImGui renderer is not initialized");

		const ImDrawData* DrawData = ImGui::GetDrawData();
		if (!DrawData || DrawData->CmdLists.empty() || DrawData->DisplaySize.x <= 0.0f || DrawData->DisplaySize.y <= 0.0f)
			return {};

		const float ScaleX = DrawData->FramebufferScale.x;
		const float ScaleY = DrawData->FramebufferScale.y;
		const float FramebufferWidth = DrawData->DisplaySize.x * ScaleX;
		const float FramebufferHeight = DrawData->DisplaySize.y * ScaleY;
		if (!std::isfinite(FramebufferWidth) || !std::isfinite(FramebufferHeight) ||
			FramebufferWidth <= 0.0f || FramebufferHeight <= 0.0f ||
			FramebufferWidth > static_cast<float>((std::numeric_limits<int>::max)()) ||
			FramebufferHeight > static_cast<float>((std::numeric_limits<int>::max)()))
		{
			return std::unexpected("Dear ImGui draw data has an invalid framebuffer extent");
		}

		const uint32_t TargetWidth = static_cast<uint32_t>(std::ceil(FramebufferWidth));
		const uint32_t TargetHeight = static_cast<uint32_t>(std::ceil(FramebufferHeight));
		m_Vertices.clear();
		m_Indices.clear();
		m_Batches.clear();
		if (DrawData->TotalVtxCount > 0)
			m_Vertices.reserve(static_cast<size_t>(DrawData->TotalVtxCount));

		try
		{
			for (const ImDrawList* List : DrawData->CmdLists)
			{
				if (!List)
					continue;
				const uint64_t ListVertexBase = m_Vertices.size();
				for (const ImDrawVert& Source : List->VtxBuffer)
				{
					Vertex& Destination = m_Vertices.emplace_back();
					Destination.Position[0] = 2.0f * (Source.pos.x - DrawData->DisplayPos.x) / DrawData->DisplaySize.x - 1.0f;
					Destination.Position[1] = 2.0f * (Source.pos.y - DrawData->DisplayPos.y) / DrawData->DisplaySize.y - 1.0f;
					Destination.TexCoord[0] = Source.uv.x;
					Destination.TexCoord[1] = Source.uv.y;
					Destination.Color[0] = ColorChannel(Source.col, IM_COL32_R_SHIFT);
					Destination.Color[1] = ColorChannel(Source.col, IM_COL32_G_SHIFT);
					Destination.Color[2] = ColorChannel(Source.col, IM_COL32_B_SHIFT);
					Destination.Color[3] = ColorChannel(Source.col, IM_COL32_A_SHIFT);
				}

				for (const ImDrawCmd& Command : List->CmdBuffer)
				{
					if (Command.UserCallback)
					{
						if (Command.UserCallback != ImDrawCallback_ResetRenderState)
							return std::unexpected("Custom Dear ImGui draw callbacks are not supported by the editor renderer");
						continue;
					}
					if (Command.ElemCount == 0)
						continue;

					const uint64_t IndexEnd = static_cast<uint64_t>(Command.IdxOffset) + Command.ElemCount;
					if (IndexEnd > static_cast<uint64_t>(List->IdxBuffer.Size))
						return std::unexpected("Dear ImGui draw command references indices outside its draw list");

					const ImVec2 ClipMin(
						(Command.ClipRect.x - DrawData->DisplayPos.x) * ScaleX,
						(Command.ClipRect.y - DrawData->DisplayPos.y) * ScaleY);
					const ImVec2 ClipMax(
						(Command.ClipRect.z - DrawData->DisplayPos.x) * ScaleX,
						(Command.ClipRect.w - DrawData->DisplayPos.y) * ScaleY);
					if (!std::isfinite(ClipMin.x) || !std::isfinite(ClipMin.y) ||
						!std::isfinite(ClipMax.x) || !std::isfinite(ClipMax.y))
						return std::unexpected("Dear ImGui draw command contains a non-finite clip rectangle");

					const float MinX = std::clamp(std::floor(ClipMin.x), 0.0f, static_cast<float>(TargetWidth));
					const float MinY = std::clamp(std::floor(ClipMin.y), 0.0f, static_cast<float>(TargetHeight));
					const float MaxX = std::clamp(std::ceil(ClipMax.x), 0.0f, static_cast<float>(TargetWidth));
					const float MaxY = std::clamp(std::ceil(ClipMax.y), 0.0f, static_cast<float>(TargetHeight));
					if (MaxX <= MinX || MaxY <= MinY)
						continue;

					const uint64_t TextureID = Command.GetTexID();
					if (!m_TextureBindings.contains(TextureID))
						return std::unexpected("Dear ImGui draw command references an unregistered editor texture");

					if (m_Indices.size() > (std::numeric_limits<uint32_t>::max)() - Command.ElemCount)
						return std::unexpected("Dear ImGui draw indices exceed the engine's 32-bit draw range");
					const uint32_t FirstIndex = static_cast<uint32_t>(m_Indices.size());
					for (uint32_t LocalIndex = 0; LocalIndex < Command.ElemCount; ++LocalIndex)
					{
						const uint64_t ListIndex =
							static_cast<uint64_t>(Command.VtxOffset) +
							static_cast<uint64_t>(List->IdxBuffer[Command.IdxOffset + LocalIndex]);
						const uint64_t GlobalIndex = ListVertexBase + ListIndex;
						if (ListIndex >= static_cast<uint64_t>(List->VtxBuffer.Size) ||
							GlobalIndex > (std::numeric_limits<uint32_t>::max)())
						{
							return std::unexpected("Dear ImGui draw command references a vertex outside its draw list");
						}
						m_Indices.push_back(static_cast<uint32_t>(GlobalIndex));
					}

					DrawBatch& Batch = m_Batches.emplace_back();
					Batch.FirstIndex = FirstIndex;
					Batch.IndexCount = Command.ElemCount;
					Batch.TextureID = TextureID;
					Batch.Scissor = {
						static_cast<uint32_t>(MinX),
						static_cast<uint32_t>(MinY),
						static_cast<uint32_t>(MaxX - MinX),
						static_cast<uint32_t>(MaxY - MinY)
					};
				}
			}
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(std::string("Could not assemble Dear ImGui draw data: ") + Exception.what());
		}

		if (m_Indices.empty())
			return {};

		const size_t VertexBytes = m_Vertices.size() * sizeof(Vertex);
		const size_t IndexBytes = m_Indices.size() * sizeof(uint32_t);
		if (auto Result = EnsureBuffers(VertexBytes, IndexBytes); !Result)
			return Result;

		const auto VertexUpload = m_Runtime->WriteBuffer(
			*m_VertexBuffer,
			0,
			std::as_bytes(std::span(m_Vertices)));
		if (!VertexUpload)
			return std::unexpected("Could not upload editor vertices: " + VertexUpload.error().Message);
		const auto IndexUpload = m_Runtime->WriteBuffer(
			*m_IndexBuffer,
			0,
			std::as_bytes(std::span(m_Indices)));
		if (!IndexUpload)
			return std::unexpected("Could not upload editor indices: " + IndexUpload.error().Message);

		for (const DrawBatch& Batch : m_Batches)
		{
			const auto Texture = m_TextureBindings.find(Batch.TextureID);
			if (Texture == m_TextureBindings.end())
				return std::unexpected("An editor texture was released before its ImGui draw command was submitted");
			const std::array<const PulseForge::BindingSet*, 1> Bindings = { Texture->second.get() };
			PulseForge::DrawIndexedArguments Arguments;
			Arguments.IndexCount = Batch.IndexCount;
			Arguments.FirstIndex = Batch.FirstIndex;
			Arguments.Scissor = Batch.Scissor;
			const PulseForge::GraphicsResult Draw = m_Runtime->DrawIndexed(
				*m_Pipeline,
				*m_VertexBuffer,
				*m_IndexBuffer,
				Arguments,
				Bindings);
			if (!Draw)
				return std::unexpected("Could not record editor UI draw: " + Draw.error().Message);
		}

		return {};
	}

	void EditorImGuiRenderer::Shutdown() noexcept
	{
		m_TextureBindings.clear();
		m_Pipeline.reset();
		m_VertexBuffer.reset();
		m_IndexBuffer.reset();
		m_FontTexture.reset();
		m_Sampler.reset();
		m_BindingLayout.reset();
		m_VertexShader.reset();
		m_FragmentShader.reset();
		m_Vertices.clear();
		m_Indices.clear();
		m_Batches.clear();
		m_VertexBufferCapacity = 0;
		m_IndexBufferCapacity = 0;
		m_FontTextureID = 0;
		m_NextTextureID = 1;
		m_Runtime = nullptr;
		m_Initialized = false;
	}
}
