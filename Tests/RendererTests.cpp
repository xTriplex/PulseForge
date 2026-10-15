#include "TestEngineSupport.h"
#include "TestGraphicsSupport.h"

namespace
{
	using namespace PulseForgeTestSupport;
	void TestGraphicsPipelineAndDrawValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		PF_CHECK(Tests, Pipeline.VertexShader != nullptr && Pipeline.FragmentShader != nullptr);

		auto MissingFragmentShader = Pipeline;
		MissingFragmentShader.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(MissingFragmentShader).has_value());

		auto WrongShaderStage = Pipeline;
		WrongShaderStage.VertexShader = std::make_shared<TestShader>(ShaderStage::Fragment);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(WrongShaderStage).has_value());

		auto UnsupportedTopology = Pipeline;
		UnsupportedTopology.Topology = static_cast<PrimitiveTopology>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTopology).has_value());

		auto UnsupportedTarget = Pipeline;
		UnsupportedTarget.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTarget).has_value());

		auto UnormTarget = Pipeline;
		UnormTarget.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(UnormTarget).has_value());
		auto SrgbTarget = Pipeline;
		SrgbTarget.ColorFormat = ColorTargetFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(SrgbTarget).has_value());
		auto HalfFloatPipeline = Pipeline;
		HalfFloatPipeline.ColorFormat = ColorTargetFormat::RGBA16_Float;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = true;
		HalfFloatPipeline.Depth.TestEnabled = true;
		HalfFloatPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());

		auto DepthTestedPipeline = Pipeline;
		DepthTestedPipeline.Depth.TestEnabled = true;
		DepthTestedPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthTestedPipeline).has_value());

		auto InvalidDepthComparison = DepthTestedPipeline;
		InvalidDepthComparison.Depth.Compare = static_cast<DepthCompareOperation>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(InvalidDepthComparison).has_value());

		auto DepthWriteWithoutTest = Pipeline;
		DepthWriteWithoutTest.Depth.WriteEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthWriteWithoutTest).has_value());

		auto DepthOnlyPipeline = Pipeline;
		DepthOnlyPipeline.FragmentShader.reset();
		DepthOnlyPipeline.ColorFormat = ColorTargetFormat::None;
		DepthOnlyPipeline.Depth.TestEnabled = true;
		DepthOnlyPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthOnlyPipeline).has_value());
		auto DepthOnlyWithColor = DepthOnlyPipeline;
		DepthOnlyWithColor.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithColor).has_value());
		auto DepthOnlyWithoutTesting = DepthOnlyPipeline;
		DepthOnlyWithoutTesting.Depth.TestEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutTesting).has_value());
		auto DepthOnlyWithoutWrites = DepthOnlyPipeline;
		DepthOnlyWithoutWrites.Depth.WriteEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutWrites).has_value());
		auto ColorPipelineWithoutFragment = Pipeline;
		ColorPipelineWithoutFragment.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorPipelineWithoutFragment).has_value());
		auto DepthOnlyBlending = DepthOnlyPipeline;
		DepthOnlyBlending.Blend.Enabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyBlending).has_value());
		auto ColorOnlyPipeline = Pipeline;
		ColorOnlyPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(ColorOnlyPipeline).has_value());
		auto ColorOnlyWithDepthTest = ColorOnlyPipeline;
		ColorOnlyWithDepthTest.Depth.TestEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorOnlyWithDepthTest).has_value());
		auto NonFiniteBias = DepthOnlyPipeline;
		NonFiniteBias.Rasterizer.SlopeScaledDepthBias = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(NonFiniteBias).has_value());

		BufferDesc VertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Test triangle vertices" };
		DrawArguments Triangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateDrawArguments(Triangle, Pipeline, VertexBuffer).has_value());

		auto ZeroVertices = Triangle;
		ZeroVertices.VertexCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroVertices, Pipeline, VertexBuffer).has_value());

		auto BeyondBuffer = Triangle;
		BeyondBuffer.VertexCount = 4;
		PF_CHECK(Tests, !ValidateDrawArguments(BeyondBuffer, Pipeline, VertexBuffer).has_value());

		auto IndexBuffer = VertexBuffer;
		IndexBuffer.Usage = BufferUsage::Index;
		PF_CHECK(Tests, !ValidateDrawArguments(Triangle, Pipeline, IndexBuffer).has_value());

		auto ZeroInstances = Triangle;
		ZeroInstances.InstanceCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroInstances, Pipeline, VertexBuffer).has_value());

		BufferDesc IndexedVertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Raw indexed draw vertices" };
		BufferDesc RawIndexBuffer{ sizeof(uint32_t) * 6, BufferUsage::Index, "Raw indexed draw indices" };
		const DrawIndexedArguments RawIndexedTriangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateIndexedBufferDrawArguments(RawIndexedTriangle, Pipeline, IndexedVertexBuffer, RawIndexBuffer).has_value());

		auto IndexRangePastEnd = RawIndexedTriangle;
		IndexRangePastEnd.FirstIndex = 4;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(IndexRangePastEnd, Pipeline, IndexedVertexBuffer, RawIndexBuffer).has_value());

		auto WrongRawIndexBuffer = RawIndexBuffer;
		WrongRawIndexBuffer.Usage = BufferUsage::Vertex;
		PF_CHECK(
			Tests, !ValidateIndexedBufferDrawArguments(RawIndexedTriangle, Pipeline, IndexedVertexBuffer, WrongRawIndexBuffer).has_value());

		auto MisalignedIndexBuffer = RawIndexBuffer;
		++MisalignedIndexBuffer.ByteSize;
		PF_CHECK(Tests,
			!ValidateIndexedBufferDrawArguments(RawIndexedTriangle, Pipeline, IndexedVertexBuffer, MisalignedIndexBuffer).has_value());

		auto ScissoredPipeline = Pipeline;
		ScissoredPipeline.Rasterizer.ScissorEnabled = true;
		auto ScissoredDraw = RawIndexedTriangle;
		ScissoredDraw.Scissor = ScissorRect{ 10, 12, 64, 48 };
		PF_CHECK(
			Tests, ValidateIndexedBufferDrawArguments(ScissoredDraw, ScissoredPipeline, IndexedVertexBuffer, RawIndexBuffer).has_value());

		auto MissingScissorState = Pipeline;
		PF_CHECK(Tests,
			!ValidateIndexedBufferDrawArguments(ScissoredDraw, MissingScissorState, IndexedVertexBuffer, RawIndexBuffer).has_value());

		auto EmptyScissor = ScissoredDraw;
		EmptyScissor.Scissor->Width = 0;
		PF_CHECK(
			Tests, !ValidateIndexedBufferDrawArguments(EmptyScissor, ScissoredPipeline, IndexedVertexBuffer, RawIndexBuffer).has_value());
	}

	void TestConstantBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "Test fragment constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateSlot = LayoutDescription;
		DuplicateSlot.Items.push_back({ BindingResourceType::ConstantBuffer, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateSlot).has_value());

		auto InvalidVisibility = LayoutDescription;
		InvalidVisibility.Visibility = static_cast<ShaderVisibility>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(InvalidVisibility).has_value());

		auto SharedVisibility = LayoutDescription;
		SharedVisibility.Visibility = ShaderVisibility::AllGraphics;
		PF_CHECK(Tests, ValidateBindingLayout(SharedVisibility).has_value());

		auto UnsupportedType = LayoutDescription;
		UnsupportedType.Items[0].Type = static_cast<BindingResourceType>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(UnsupportedType).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });
		TestBuffer VertexBuffer(BufferDesc{ 48, BufferUsage::Vertex, "Test vertices" });
		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto MissingBuffer = SetDescription;
		MissingBuffer.Buffers.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingBuffer).has_value());

		auto WrongBufferType = SetDescription;
		WrongBufferType.Buffers[0].Resource = std::cref(static_cast<const Buffer&>(VertexBuffer));
		PF_CHECK(Tests, !ValidateBindingSet(WrongBufferType).has_value());

		auto WrongSlot = SetDescription;
		WrongSlot.Buffers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSlot).has_value());

		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		Pipeline.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		auto DuplicateRegisterSpace = Pipeline;
		DuplicateRegisterSpace.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DuplicateRegisterSpace).has_value());

		TestBindingSet CreatedTestBindingSet(Layout);
		const std::array<const BindingSet*, 1> ValidSets = { &CreatedTestBindingSet };
		PF_CHECK(Tests, ValidateDrawBindingSets(Pipeline, ValidSets).has_value());
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, {}).has_value());

		BindingLayoutHandle DifferentLayout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBindingSet MismatchedSet(DifferentLayout);
		const std::array<const BindingSet*, 1> MismatchedSets = { &MismatchedSet };
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, MismatchedSets).has_value());
	}

	void TestEnvironmentLightingProcessing(TestRunner& Tests)
	{
		using namespace PulseForge;
		const glm::vec2 PositiveX = EquirectangularUvFromDirection({ 1.0f, 0.0f, 0.0f });
		const glm::vec2 NegativeX = EquirectangularUvFromDirection({ -1.0f, 0.0f, 0.0f });
		const glm::vec2 PositiveZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, 1.0f });
		const glm::vec2 NegativeZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, -1.0f });
		const glm::vec2 PositiveY = EquirectangularUvFromDirection({ 0.0f, 1.0f, 0.0f });
		const glm::vec2 NegativeY = EquirectangularUvFromDirection({ 0.0f, -1.0f, 0.0f });
		PF_CHECK(Tests, std::abs(PositiveX.x - 0.5f) < 0.0001f && std::abs(PositiveX.y - 0.5f) < 0.0001f);
		PF_CHECK(Tests, NegativeX.x < 0.0001f || NegativeX.x > 0.9999f);
		PF_CHECK(Tests, std::abs(PositiveZ.x - 0.75f) < 0.0001f);
		PF_CHECK(Tests, std::abs(NegativeZ.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, std::abs(PositiveY.y) < 0.0001f && std::abs(NegativeY.y - 1.0f) < 0.0001f);

		constexpr std::array<glm::vec3, 6> ExpectedFaceCenters = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f) };
		for (uint32_t Face = 0; Face < ExpectedFaceCenters.size(); ++Face)
		{
			const glm::vec3 Direction = CubeFaceTexelDirection(static_cast<CubeFace>(Face), 0.5f, 0.5f);
			PF_CHECK(Tests, glm::dot(Direction, ExpectedFaceCenters[Face]) > 0.9999f);
		}

		ImportedFloatImage ConstantEnvironment;
		ConstantEnvironment.Width = 8;
		ConstantEnvironment.Height = 4;
		ConstantEnvironment.RGBA32FPixels.resize(8 * 4 * 4);
		for (size_t Pixel = 0; Pixel < 8 * 4; ++Pixel)
		{
			ConstantEnvironment.RGBA32FPixels[Pixel * 4] = 2.0f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 1] = 0.5f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 2] = 0.25f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 3] = 1.0f;
		}
		EnvironmentProcessingDesc Settings;
		Settings.EnvironmentFaceSize = 8;
		Settings.IrradianceFaceSize = 4;
		Settings.SpecularFaceSize = 8;
		Settings.BrdfLutSize = 8;
		Settings.IrradianceSamples = 32;
		Settings.PrefilterSamples = 32;
		Settings.BrdfSamples = 32;
		auto Processed = ProcessEnvironmentImage(ConstantEnvironment, Settings);
		PF_CHECK(Tests, Processed.has_value());
		if (Processed)
		{
			PF_CHECK(Tests, Processed->Environment.Size == 8 && Processed->DiffuseIrradiance.Size == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.size() == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.front().Size == 8 && Processed->PrefilteredSpecular.back().Size == 1);
			PF_CHECK(Tests, Processed->BrdfIntegrationLut.Width == 8 && Processed->BrdfIntegrationLut.Height == 8);
			for (const float Channel : Processed->Environment.Faces[0])
				PF_CHECK(Tests, std::isfinite(Channel));
			for (const float Channel : Processed->BrdfIntegrationLut.RGBA32FPixels)
				PF_CHECK(Tests, std::isfinite(Channel));
			const std::vector<float>& IrradiancePositiveX = Processed->DiffuseIrradiance.Faces[0];
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[0] - 2.0f * std::numbers::pi_v<float>) < 0.05f);
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[1] - 0.5f * std::numbers::pi_v<float>) < 0.05f);
			for (const FloatCubeLevel& Level : Processed->PrefilteredSpecular)
			{
				PF_CHECK(Tests, std::abs(Level.Faces[0][0] - 2.0f) < 0.01f);
				PF_CHECK(Tests, std::abs(Level.Faces[0][1] - 0.5f) < 0.01f);
			}
		}
		std::stop_source CancelSource;
		CancelSource.request_stop();
		const auto CancelledProcessing = ProcessEnvironmentImage(ConstantEnvironment, Settings, CancelSource.get_token());
		PF_CHECK(Tests, !CancelledProcessing && CancelledProcessing.error().Code == EnvironmentProcessingErrorCode::Cancelled);
		auto InvalidSize = Settings;
		InvalidSize.EnvironmentFaceSize = 7;
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, InvalidSize).has_value());
		ConstantEnvironment.RGBA32FPixels[0] = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, Settings).has_value());
	}

	void TestTextureAndSamplerValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		TextureDesc Description;
		Description.Width = 2;
		Description.Height = 2;
		Description.Format = TextureFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateTextureUpload(Description, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 15).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 17).has_value());
		PF_CHECK(Tests, Description.Dimension == TextureDimension::Texture2D && Description.MipLevels == 1);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_UNorm) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_Srgb) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA16_Float) == 8);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA32_Float) == 16);
		PF_CHECK(Tests, !GetTextureFormatBytesPerPixel(TextureFormat::Depth32Float).has_value());

		TextureDesc HalfFloatDescription;
		HalfFloatDescription.Format = TextureFormat::RGBA16_Float;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 8).value() == 8);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 7).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 9).has_value());
		HalfFloatDescription.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 48).value() == 48);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 47).has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 128).value() == 128);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 127).has_value());

		const std::array<std::byte, 72> HalfFloatUploadBytes{};
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		const TextureSubresourceData HalfFloatSubresource{ 0, 0, HalfFloatUploadBytes, 24 };
		PF_CHECK(Tests,
			ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>(&HalfFloatSubresource, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatPitch{ 0, 0, HalfFloatUploadBytes, 15 };
		PF_CHECK(Tests,
			!ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>(&InvalidHalfFloatPitch, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatSize{ 0, 0, std::span<const std::byte>(HalfFloatUploadBytes).first(71), 24 };
		PF_CHECK(Tests,
			!ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>(&InvalidHalfFloatSize, 1)).has_value());
		const std::array<std::byte, 73> OversizedHalfFloatUploadBytes{};
		const TextureSubresourceData OversizedHalfFloatSubresource{ 0, 0, OversizedHalfFloatUploadBytes, 24 };
		PF_CHECK(
			Tests, !ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>(&OversizedHalfFloatSubresource, 1))
					   .has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		HalfFloatDescription.MipLevels = 3;
		const std::array<std::byte, 168> HalfFloatMips{};
		const std::array<TextureSubresourceData, 3> HalfFloatMipSubresources = { TextureSubresourceData{
																					 0, 0, std::span(HalfFloatMips).first(128), 0 },
			TextureSubresourceData{ 1, 0, std::span(HalfFloatMips).subspan(128, 32), 0 },
			TextureSubresourceData{ 2, 0, std::span(HalfFloatMips).last(8), 0 } };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatMipSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, std::span(HalfFloatMipSubresources).first(2)).has_value());
		HalfFloatDescription.MipLevels = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.MipLevels = 1;
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource;
		HalfFloatDescription.Dimension = TextureDimension::TextureCube;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		std::array<std::byte, 48> HalfFloatCubeBytes{};
		std::array<TextureSubresourceData, 6> HalfFloatCube{};
		for (uint32_t Face = 0; Face < HalfFloatCube.size(); ++Face)
			HalfFloatCube[Face] = { 0, Face, std::span(HalfFloatCubeBytes).subspan(Face * 8, 8), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());

		TextureDesc CubeDescription;
		CubeDescription.Width = 1;
		CubeDescription.Height = 1;
		CubeDescription.Format = TextureFormat::RGBA32_Float;
		CubeDescription.Dimension = TextureDimension::TextureCube;
		std::array<std::byte, 96> CubeUploadBytes{};
		std::array<TextureSubresourceData, 6> CubeSubresources{};
		for (uint32_t Face = 0; Face < CubeSubresources.size(); ++Face)
			CubeSubresources[Face] = { 0, Face, std::span(CubeUploadBytes).subspan(Face * 16, 16), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(CubeDescription, CubeSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, std::span(CubeSubresources).first(5)).has_value());
		auto DuplicateFaceUploads = CubeSubresources;
		DuplicateFaceUploads[5].ArraySlice = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, DuplicateFaceUploads).has_value());
		auto InvalidFaceUpload = CubeSubresources;
		InvalidFaceUpload[0].ArraySlice = 6;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidFaceUpload).has_value());
		auto InvalidCubePitch = CubeSubresources;
		InvalidCubePitch[0].RowPitch = 15;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidCubePitch).has_value());
		auto RectangularCube = CubeDescription;
		RectangularCube.Height = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(RectangularCube, CubeSubresources).has_value());
		auto ExcessMips = CubeDescription;
		ExcessMips.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(ExcessMips, CubeSubresources).has_value());

		TextureDesc MippedFloat2D = CubeDescription;
		MippedFloat2D.Dimension = TextureDimension::Texture2D;
		MippedFloat2D.Width = 2;
		MippedFloat2D.Height = 2;
		MippedFloat2D.MipLevels = 2;
		const std::array<std::byte, 80> MippedUploadBytes{};
		const std::array<TextureSubresourceData, 2> MippedSubresources = { TextureSubresourceData{
																			   0, 0, std::span(MippedUploadBytes).first(64), 0 },
			TextureSubresourceData{ 1, 0, std::span(MippedUploadBytes).last(16), 0 } };
		PF_CHECK(Tests, ValidateTextureUpload(MippedFloat2D, MippedSubresources).has_value());
		auto MissingMipUpload = MippedSubresources;
		MissingMipUpload[1].MipLevel = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedFloat2D, MissingMipUpload).has_value());

		auto ZeroWidth = Description;
		ZeroWidth.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroWidth, 0).has_value());

		auto UnsupportedFormat = Description;
		UnsupportedFormat.Format = static_cast<TextureFormat>(0xff);
		PF_CHECK(Tests, !ValidateTextureUpload(UnsupportedFormat, 16).has_value());

		auto OverflowDimensions = Description;
		OverflowDimensions.Width = UINT32_MAX;
		OverflowDimensions.Height = UINT32_MAX;
		PF_CHECK(Tests, !ValidateTextureUpload(OverflowDimensions, 0).has_value());
		auto HalfFloatOverflowDimensions = OverflowDimensions;
		HalfFloatOverflowDimensions.Format = TextureFormat::RGBA16_Float;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatOverflowDimensions, 0).has_value());

		TextureDesc DepthDescription;
		DepthDescription.Width = 128;
		DepthDescription.Height = 64;
		DepthDescription.Format = TextureFormat::Depth32Float;
		DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(DepthDescription, 0).value() == 0);

		auto ZeroDepthHeight = DepthDescription;
		ZeroDepthHeight.Height = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroDepthHeight, 0).has_value());

		auto DepthAsShaderResource = DepthDescription;
		DepthAsShaderResource.Usage = TextureUsage::ShaderResource;
		PF_CHECK(Tests, !ValidateTextureUpload(DepthAsShaderResource, 0).has_value());

		PF_CHECK(Tests, !ValidateTextureUpload(DepthDescription, sizeof(float)).has_value());

		auto ColorAsDepthAttachment = Description;
		ColorAsDepthAttachment.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(ColorAsDepthAttachment, 16).has_value());

		auto RenderableColor = Description;
		RenderableColor.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 0).value() == 0);
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(RenderableColor, 15).has_value());

		auto AttachmentOnly = Description;
		AttachmentOnly.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(AttachmentOnly, 0).value() == 0);

		auto MissingShaderUpload = Description;
		PF_CHECK(Tests, !ValidateTextureUpload(MissingShaderUpload, 0).has_value());

		auto UnknownUsage = Description;
		UnknownUsage.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(UnknownUsage, 16).has_value());

		auto SampleableDepth = DepthDescription;
		SampleableDepth.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepth, 0).has_value());
		auto SampleableDepthCube = SampleableDepth;
		SampleableDepthCube.Dimension = TextureDimension::TextureCube;
		SampleableDepthCube.Width = 512;
		SampleableDepthCube.Height = 512;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto NonSquareDepthCube = SampleableDepthCube;
		NonSquareDepthCube.Width = 256;
		PF_CHECK(Tests, !ValidateTextureUpload(NonSquareDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto MippedDepthCube = SampleableDepthCube;
		MippedDepthCube.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto InvalidDepthUsage = DepthDescription;
		InvalidDepthUsage.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(InvalidDepthUsage, 0).has_value());

		RenderTargetDesc TargetDescription;
		TargetDescription.Width = 800;
		TargetDescription.Height = 600;
		PF_CHECK(Tests, ValidateRenderTargetDescription(TargetDescription).has_value());
		PF_CHECK(Tests,
			MatchesRenderTargetConfiguration(TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests,
			!MatchesRenderTargetConfiguration(TargetDescription, 0, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
							TargetDescription, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests,
			!MatchesRenderTargetConfiguration(TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::None));
		auto HalfFloatColorTarget = TargetDescription;
		HalfFloatColorTarget.ColorFormat = ColorTargetFormat::RGBA16_Float;
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests,
			MatchesRenderTargetConfiguration(HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::None));
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::Attachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
							HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		auto DepthOnlyTarget = TargetDescription;
		DepthOnlyTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(DepthOnlyTarget).has_value());
		PF_CHECK(Tests, DepthOnlyTarget.DepthMode == DepthAttachmentMode::Attachment);
		auto SampleableDepthOnlyTarget = DepthOnlyTarget;
		SampleableDepthOnlyTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(SampleableDepthOnlyTarget).has_value());
		auto ColorOnlyTarget = TargetDescription;
		ColorOnlyTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorOnlyTarget).has_value());
		auto ColorSampleableDepthTarget = TargetDescription;
		ColorSampleableDepthTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorSampleableDepthTarget).has_value());
		auto NoAttachmentsTarget = ColorOnlyTarget;
		NoAttachmentsTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(NoAttachmentsTarget).has_value());
		auto InvalidDepthMode = TargetDescription;
		InvalidDepthMode.DepthMode = static_cast<DepthAttachmentMode>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(InvalidDepthMode).has_value());
		auto ZeroTargetWidth = TargetDescription;
		ZeroTargetWidth.Width = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetWidth).has_value());
		auto ZeroTargetHeight = TargetDescription;
		ZeroTargetHeight.Height = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetHeight).has_value());
		auto SwapchainTarget = TargetDescription;
		SwapchainTarget.ColorFormat = ColorTargetFormat::Swapchain;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(SwapchainTarget).has_value());
		auto UnknownTargetFormat = TargetDescription;
		UnknownTargetFormat.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(UnknownTargetFormat).has_value());

		RenderTargetClearValue ClearValue;
		PF_CHECK(Tests, ValidateRenderTargetClearValue(ClearValue).has_value());
		auto InvalidClearDepth = ClearValue;
		InvalidClearDepth.Depth = 1.1f;
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(InvalidClearDepth).has_value());
		auto NonFiniteClearColor = ClearValue;
		NonFiniteClearColor.Color[2] = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(NonFiniteClearColor).has_value());

		SamplerDesc SamplerDescription;
		PF_CHECK(Tests, ValidateSamplerDescription(SamplerDescription).has_value());
		auto UnsupportedFilter = SamplerDescription;
		UnsupportedFilter.Minification = static_cast<SamplerFilter>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedFilter).has_value());
		auto UnsupportedAddressMode = SamplerDescription;
		UnsupportedAddressMode.AddressV = static_cast<SamplerAddressMode>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedAddressMode).has_value());
	}

	void TestTextureSamplerAndBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::Texture2D, 0 }, { BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "Test texture/sampler/constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateTextureSlot = LayoutDescription;
		DuplicateTextureSlot.Items.push_back({ BindingResourceType::Texture2D, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateTextureSlot).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TextureDesc TextureDescription;
		TextureDescription.Width = 1;
		TextureDescription.Height = 1;
		TextureDescription.DebugName = "Test texture";
		TestTexture FakeTexture(TextureDescription);
		TextureDesc DepthTextureDescription;
		DepthTextureDescription.Width = 1;
		DepthTextureDescription.Height = 1;
		DepthTextureDescription.Format = TextureFormat::Depth32Float;
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment;
		TestTexture FakeDepthTexture(DepthTextureDescription);
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		TestTexture FakeSampleableDepthTexture(DepthTextureDescription);
		TestSampler Sampler;
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });

		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Textures.push_back({ 0, std::cref(static_cast<const PulseForge::Texture&>(FakeTexture)) });
		SetDescription.Samplers.push_back({ 0, std::cref(static_cast<const PulseForge::Sampler&>(Sampler)) });
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto CubeLayoutDescription = LayoutDescription;
		CubeLayoutDescription.Items[0].Type = BindingResourceType::TextureCube;
		BindingLayoutHandle CubeLayout = std::make_shared<TestBindingLayout>(CubeLayoutDescription);
		TextureDesc CubeTextureDescription = TextureDescription;
		CubeTextureDescription.Dimension = TextureDimension::TextureCube;
		TestTexture FakeCubeTexture(CubeTextureDescription);
		auto CubeBindingDescription = SetDescription;
		CubeBindingDescription.Layout = CubeLayout;
		CubeBindingDescription.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		CubeBindingDescription.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, ValidateBindingSet(CubeBindingDescription).has_value());
		auto WrongCubeDimension = CubeBindingDescription;
		WrongCubeDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(WrongCubeDimension).has_value());
		auto Wrong2DDimension = SetDescription;
		Wrong2DDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(Wrong2DDimension).has_value());

		auto DepthTextureBinding = SetDescription;
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeDepthTexture));
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		PF_CHECK(Tests, ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		DepthTextureBinding.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());

		auto MissingTexture = SetDescription;
		MissingTexture.Textures.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingTexture).has_value());

		auto WrongSamplerSlot = SetDescription;
		WrongSamplerSlot.Samplers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSamplerSlot).has_value());

		auto DuplicateSamplerSlot = SetDescription;
		DuplicateSamplerSlot.Samplers.push_back(DuplicateSamplerSlot.Samplers[0]);
		PF_CHECK(Tests, !ValidateBindingSet(DuplicateSamplerSlot).has_value());
	}

} // namespace

void RunRendererTests(TestRunner& Tests)
{
	TestEnvironmentLightingProcessing(Tests);
	TestGraphicsPipelineAndDrawValidation(Tests);
	TestConstantBufferBindingValidation(Tests);
	TestTextureAndSamplerValidation(Tests);
	TestTextureSamplerAndBufferBindingValidation(Tests);
}
