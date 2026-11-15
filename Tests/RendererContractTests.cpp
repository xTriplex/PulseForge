#include "TestGraphicsSupport.h"

#include "Renderer/RenderDevice.h"

#include <algorithm>

namespace
{
	using namespace PulseForgeTestSupport;

	class RecordingBuffer final : public PulseForge::Buffer
	{
	public:
		RecordingBuffer(PulseForge::BufferDesc Description, std::span<const std::byte> InitialData)
			: m_Description(std::move(Description)), m_Data(static_cast<size_t>(m_Description.ByteSize))
		{
			std::copy(InitialData.begin(), InitialData.end(), m_Data.begin());
		}

		const PulseForge::BufferDesc& GetDescription() const noexcept override { return m_Description; }
		std::vector<std::byte>& Data() noexcept { return m_Data; }

	private:
		PulseForge::BufferDesc m_Description;
		std::vector<std::byte> m_Data;
	};

	class MeshCreationTestDevice final : public PulseForge::RenderDevice
	{
	public:
		std::pair<unsigned int, unsigned int> GetOutputFramebufferSize() const noexcept override { return {}; }
		PulseForge::OutputColorEncoding GetOutputColorEncoding() const noexcept override
		{
			return PulseForge::OutputColorEncoding::SrgbAttachment;
		}
		PulseForge::RenderTargetCreateResult CreateRenderTarget(const PulseForge::RenderTargetDesc&) override
		{
			return std::unexpected(PulseForge::RenderTargetError{ PulseForge::RenderTargetErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::DepthCubemapCreateResult CreateDepthCubemap(uint32_t, const std::string&) override
		{
			return std::unexpected(PulseForge::DepthCubemapError{ PulseForge::DepthCubemapErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult BeginRenderTarget(const PulseForge::RenderTarget&, const PulseForge::RenderTargetClearValue&) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult EndRenderTarget() override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::BufferCreateResult CreateBuffer(
			const PulseForge::BufferDesc& Description,
			std::span<const std::byte> InitialData) override
		{
			if (auto Valid = PulseForge::ValidateBufferDescription(Description, InitialData.size()); !Valid)
				return std::unexpected(Valid.error());
			CreatedBufferDescriptions.push_back(Description);
			return PulseForge::BufferHandle(new RecordingBuffer(Description, InitialData));
		}
		PulseForge::BufferUpdateResult WriteBuffer(
			const PulseForge::Buffer& Target,
			uint64_t DestinationOffset,
			std::span<const std::byte> Data) override
		{
			const auto* Recorded = dynamic_cast<const RecordingBuffer*>(&Target);
			if (!Recorded)
				return std::unexpected(PulseForge::BufferUpdateError{
					PulseForge::BufferUpdateErrorCode::BackendFailure, "test buffer type mismatch" });
			if (auto Valid = PulseForge::ValidateBufferUpdate(Target.GetDescription(), DestinationOffset, Data.size()); !Valid)
				return std::unexpected(Valid.error());
			auto& Destination = const_cast<RecordingBuffer*>(Recorded)->Data();
			std::copy(Data.begin(), Data.end(), Destination.begin() + static_cast<ptrdiff_t>(DestinationOffset));
			LastWriteBytes.assign(Data.begin(), Data.end());
			++WriteCount;
			return {};
		}
		PulseForge::TextureCreateResult CreateTexture(
			const PulseForge::TextureDesc&, std::span<const PulseForge::TextureSubresourceData>) override
		{
			return std::unexpected(PulseForge::TextureError{ PulseForge::TextureErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::SamplerCreateResult CreateSampler(const PulseForge::SamplerDesc&) override
		{
			return std::unexpected(PulseForge::TextureError{ PulseForge::TextureErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::BindingLayoutCreateResult CreateBindingLayout(const PulseForge::BindingLayoutDesc&) override
		{
			return std::unexpected(PulseForge::BindingError{ PulseForge::BindingErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::BindingSetCreateResult CreateBindingSet(const PulseForge::BindingSetDesc&) override
		{
			return std::unexpected(PulseForge::BindingError{ PulseForge::BindingErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::ShaderCreateResult CreateShader(const PulseForge::ShaderDesc&, std::span<const std::byte>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsPipelineCreateResult CreateGraphicsPipeline(const PulseForge::GraphicsPipelineDesc&) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult Draw(const PulseForge::GraphicsPipeline&, const PulseForge::Buffer&,
			const PulseForge::DrawArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult Draw(const PulseForge::GraphicsPipeline&, const PulseForge::Mesh&,
			const PulseForge::DrawArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult DrawIndexed(const PulseForge::GraphicsPipeline&, const PulseForge::Buffer&,
			const PulseForge::Buffer&, const PulseForge::DrawIndexedArguments&,
			std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}
		PulseForge::GraphicsResult DrawIndexed(const PulseForge::GraphicsPipeline&, const PulseForge::Mesh&,
			const PulseForge::DrawIndexedArguments&, std::span<const PulseForge::BindingSet* const>) override
		{
			return std::unexpected(PulseForge::GraphicsError{ PulseForge::GraphicsErrorCode::UnsupportedFeature, "test stub" });
		}

		std::vector<PulseForge::BufferDesc> CreatedBufferDescriptions;
		std::vector<std::byte> LastWriteBytes;
		size_t WriteCount = 0;
	};

	void TestDynamicMeshCreationAndUpdates(TestRunner& Tests)
	{
		using namespace PulseForge;
		MeshCreationTestDevice Runtime;
		const std::array<float, 9> InitialVertices = { 0, 0, 0, 1, 0, 0, 0, 1, 0 };
		const std::array<uint32_t, 3> Indices = { 0, 1, 2 };
		MeshDesc Description;
		Description.VertexLayout.Stride = sizeof(float) * 3;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 } };
		Description.VertexData = std::as_bytes(std::span(InitialVertices));
		Description.Indices = Indices;
		Description.DebugName = "dynamic mesh contract fixture";

		auto Immutable = Runtime.CreateMesh(Description);
		PF_CHECK(Tests, Immutable.has_value());
		PF_CHECK(Tests, Runtime.CreatedBufferDescriptions.size() == 2);
		if (Runtime.CreatedBufferDescriptions.size() == 2)
		{
			PF_CHECK(Tests, !Runtime.CreatedBufferDescriptions[0].IsDynamic);
			PF_CHECK(Tests, !Runtime.CreatedBufferDescriptions[1].IsDynamic);
		}

		auto Dynamic = Runtime.CreateDynamicMesh(Description);
		PF_CHECK(Tests, Dynamic.has_value());
		PF_CHECK(Tests, Runtime.CreatedBufferDescriptions.size() == 4);
		if (Runtime.CreatedBufferDescriptions.size() == 4)
		{
			PF_CHECK(Tests, Runtime.CreatedBufferDescriptions[2].Usage == BufferUsage::Vertex &&
				Runtime.CreatedBufferDescriptions[2].IsDynamic);
			PF_CHECK(Tests, Runtime.CreatedBufferDescriptions[3].Usage == BufferUsage::Index &&
				!Runtime.CreatedBufferDescriptions[3].IsDynamic);
		}
		if (!Dynamic || !Immutable) return;

		const std::array<float, 9> UpdatedVertices = { 0, 0.5f, 0, 1, 0.5f, 0, 0, 1.5f, 0 };
		const auto Updated = Runtime.UpdateDynamicMeshVertices(**Dynamic, Description.VertexLayout,
			std::as_bytes(std::span(UpdatedVertices)));
		PF_CHECK(Tests, Updated.has_value());
		PF_CHECK(Tests, Runtime.WriteCount == 1);
		PF_CHECK(Tests, Runtime.LastWriteBytes == std::vector<std::byte>(
			std::as_bytes(std::span(UpdatedVertices)).begin(), std::as_bytes(std::span(UpdatedVertices)).end()));
		const auto WrongSize = Runtime.UpdateDynamicMeshVertices(**Dynamic, Description.VertexLayout,
			std::as_bytes(std::span(UpdatedVertices).first(8)));
		PF_CHECK(Tests, !WrongSize && WrongSize.error().Code == BufferUpdateErrorCode::InvalidUpdateSize);
		VertexLayoutDesc WrongLayout = Description.VertexLayout;
		WrongLayout.Stride = sizeof(float) * 4;
		const auto LayoutMismatch = Runtime.UpdateDynamicMeshVertices(**Dynamic, WrongLayout,
			std::as_bytes(std::span(UpdatedVertices)));
		PF_CHECK(Tests, !LayoutMismatch && LayoutMismatch.error().Code == BufferUpdateErrorCode::InvalidUpdateSize);
		const auto ImmutableUpdate = Runtime.UpdateDynamicMeshVertices(**Immutable, Description.VertexLayout,
			std::as_bytes(std::span(UpdatedVertices)));
		PF_CHECK(Tests, !ImmutableUpdate && ImmutableUpdate.error().Code == BufferUpdateErrorCode::UnsupportedUsage);
		PF_CHECK(Tests, Runtime.WriteCount == 1);

		auto InvalidDescription = Description;
		InvalidDescription.VertexLayout.Stride = 0;
		const auto InvalidLayoutMesh = Runtime.CreateDynamicMesh(InvalidDescription);
		PF_CHECK(Tests, !InvalidLayoutMesh && InvalidLayoutMesh.error().Code == MeshErrorCode::InvalidVertexLayout);
		PF_CHECK(Tests, Runtime.CreatedBufferDescriptions.size() == 4);
	}

	void TestBufferDescriptionValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BufferDesc ValidDescription{ 48, BufferUsage::Vertex, "Test vertex buffer" };
		PF_CHECK(Tests, ValidateBufferDescription(ValidDescription, 48).has_value());
		PF_CHECK(Tests, ValidateBufferDescription(ValidDescription, 0).has_value());

		auto DynamicVertexDescription = ValidDescription;
		DynamicVertexDescription.IsDynamic = true;
		PF_CHECK(Tests, ValidateBufferDescription(DynamicVertexDescription, 48).has_value());

		auto ZeroSize = ValidDescription;
		ZeroSize.ByteSize = 0;
		auto ZeroSizeResult = ValidateBufferDescription(ZeroSize, 0);
		PF_CHECK(Tests, !ZeroSizeResult.has_value());
		PF_CHECK(Tests, ZeroSizeResult.error().Code == BufferCreateErrorCode::InvalidByteSize);

		auto TooMuchData = ValidateBufferDescription(ValidDescription, 49);
		PF_CHECK(Tests, !TooMuchData.has_value());
		PF_CHECK(Tests, TooMuchData.error().Code == BufferCreateErrorCode::InitialDataTooLarge);

		auto UnsupportedUsage = ValidDescription;
		UnsupportedUsage.Usage = static_cast<BufferUsage>(0xff);
		auto UnsupportedUsageResult = ValidateBufferDescription(UnsupportedUsage, 0);
		PF_CHECK(Tests, !UnsupportedUsageResult.has_value());
		PF_CHECK(Tests, UnsupportedUsageResult.error().Code == BufferCreateErrorCode::UnsupportedUsage);

		BufferDesc ConstantDescription{ 16, BufferUsage::Constant, "Test constant buffer" };
		PF_CHECK(Tests, ValidateBufferDescription(ConstantDescription, 16).has_value());
		ConstantDescription.IsDynamic = true;
		PF_CHECK(Tests, !ValidateBufferDescription(ConstantDescription, 0).has_value());
		ConstantDescription.IsDynamic = false;
		ConstantDescription.ByteSize = 17;
		PF_CHECK(Tests, !ValidateBufferDescription(ConstantDescription, 0).has_value());
	}

	void TestShaderDescriptionAndSpirVValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		ShaderDesc Description;
		Description.Stage = ShaderStage::Vertex;
		Description.EntryPoint = "VSMain";
		std::array<std::byte, 20> ValidBytecode{};
		ValidBytecode[0] = std::byte{ 0x03 };
		ValidBytecode[1] = std::byte{ 0x02 };
		ValidBytecode[2] = std::byte{ 0x23 };
		ValidBytecode[3] = std::byte{ 0x07 };

		PF_CHECK(Tests, ValidateShaderBytecode(Description, ValidBytecode).has_value());
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, {}).has_value());

		std::array<std::byte, 21> MisalignedBytecode{};
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, MisalignedBytecode).has_value());

		auto InvalidMagic = ValidBytecode;
		InvalidMagic[0] = std::byte{ 0 };
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, InvalidMagic).has_value());

		auto InvalidStage = Description;
		InvalidStage.Stage = static_cast<ShaderStage>(0xff);
		PF_CHECK(Tests, !ValidateShaderBytecode(InvalidStage, ValidBytecode).has_value());

		auto MissingEntryPoint = Description;
		MissingEntryPoint.EntryPoint.clear();
		PF_CHECK(Tests, !ValidateShaderBytecode(MissingEntryPoint, ValidBytecode).has_value());
	}

	void TestVertexLayoutValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		VertexLayoutDesc Layout;
		Layout.Stride = sizeof(float) * 6;
		Layout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 } };
		PF_CHECK(Tests, ValidateVertexLayout(Layout).has_value());
		auto NormalLayout = Layout;
		NormalLayout.Stride = sizeof(float) * 9;
		NormalLayout.Attributes.push_back({ VertexSemantic::Normal, VertexFormat::Float3, sizeof(float) * 6 });
		PF_CHECK(Tests, ValidateVertexLayout(NormalLayout).has_value());

		auto ZeroStride = Layout;
		ZeroStride.Stride = 0;
		PF_CHECK(Tests, !ValidateVertexLayout(ZeroStride).has_value());

		auto NoAttributes = Layout;
		NoAttributes.Attributes.clear();
		PF_CHECK(Tests, !ValidateVertexLayout(NoAttributes).has_value());

		auto DuplicateSemantic = Layout;
		DuplicateSemantic.Attributes[1].Semantic = VertexSemantic::Position;
		PF_CHECK(Tests, !ValidateVertexLayout(DuplicateSemantic).has_value());

		auto AttributeOutsideStride = Layout;
		AttributeOutsideStride.Attributes[1].Offset = Layout.Stride - sizeof(float) * 2;
		PF_CHECK(Tests, !ValidateVertexLayout(AttributeOutsideStride).has_value());

		auto InvalidFormat = Layout;
		InvalidFormat.Attributes[0].Format = static_cast<VertexFormat>(0xff);
		PF_CHECK(Tests, !ValidateVertexLayout(InvalidFormat).has_value());

		auto InvalidSemantic = Layout;
		InvalidSemantic.Attributes[0].Semantic = static_cast<VertexSemantic>(0xff);
		PF_CHECK(Tests, !ValidateVertexLayout(InvalidSemantic).has_value());
	}

	void TestMeshDescriptionAndDrawValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::array<float, 9> Vertices = { 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, -0.5f, -0.5f, 0.0f };
		const std::array<uint32_t, 3> Indices = { 0, 1, 2 };
		MeshDesc Description;
		Description.VertexLayout.Stride = sizeof(float) * 3;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 } };
		Description.VertexData = std::as_bytes(std::span(Vertices));
		Description.Indices = Indices;
		Description.DebugName = "Test indexed mesh";

		const MeshValidationResult ValidMesh = ValidateMeshDescription(Description);
		PF_CHECK(Tests, ValidMesh.has_value());
		PF_CHECK(Tests, ValidMesh && ValidMesh->VertexCount == 3 && ValidMesh->IndexCount == 3);
		const uint32_t ValidatedIndexCount = ValidMesh ? ValidMesh->IndexCount : 0;

		auto EmptyVertices = Description;
		EmptyVertices.VertexData = {};
		const auto EmptyVertexResult = ValidateMeshDescription(EmptyVertices);
		PF_CHECK(Tests, !EmptyVertexResult.has_value());
		PF_CHECK(Tests, EmptyVertexResult.error().Code == MeshErrorCode::InvalidVertexData);

		auto InvalidLayout = Description;
		InvalidLayout.VertexLayout.Stride = 0;
		PF_CHECK(Tests, !ValidateMeshDescription(InvalidLayout).has_value());

		std::array<std::byte, sizeof(Vertices) + 1> TrailingVertexByte{};
		auto MisalignedVertexData = Description;
		MisalignedVertexData.VertexData = TrailingVertexByte;
		PF_CHECK(Tests, !ValidateMeshDescription(MisalignedVertexData).has_value());

		const std::array<uint32_t, 3> OutOfRangeIndices = { 0, 1, 3 };
		auto InvalidIndices = Description;
		InvalidIndices.Indices = OutOfRangeIndices;
		const auto InvalidIndexResult = ValidateMeshDescription(InvalidIndices);
		PF_CHECK(Tests, !InvalidIndexResult.has_value());
		PF_CHECK(Tests, InvalidIndexResult.error().Code == MeshErrorCode::InvalidIndexData);

		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		Pipeline.VertexLayout = Description.VertexLayout;
		const DrawIndexedArguments IndexedTriangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateIndexedDrawArguments(IndexedTriangle, Pipeline, Description.VertexLayout, ValidatedIndexCount).has_value());

		auto IndexRangePastEnd = IndexedTriangle;
		IndexRangePastEnd.FirstIndex = 2;
		PF_CHECK(
			Tests, !ValidateIndexedDrawArguments(IndexRangePastEnd, Pipeline, Description.VertexLayout, ValidatedIndexCount).has_value());
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(IndexedTriangle, Pipeline, Description.VertexLayout, 0).has_value());

		auto ZeroIndexedInstances = IndexedTriangle;
		ZeroIndexedInstances.InstanceCount = 0;
		PF_CHECK(Tests,
			!ValidateIndexedDrawArguments(ZeroIndexedInstances, Pipeline, Description.VertexLayout, ValidatedIndexCount).has_value());

		auto MismatchedPipeline = Pipeline;
		MismatchedPipeline.VertexLayout.Attributes[0].Offset = sizeof(float);
		PF_CHECK(Tests,
			!ValidateIndexedDrawArguments(IndexedTriangle, MismatchedPipeline, Description.VertexLayout, ValidatedIndexCount).has_value());

		const BufferDesc VertexBuffer{ Description.VertexData.size(), BufferUsage::Vertex, "Test mesh vertices" };
		const DrawArguments Triangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateMeshDrawArguments(Triangle, Pipeline, Description.VertexLayout, VertexBuffer).has_value());

		VertexLayoutDesc FullMeshLayout;
		FullMeshLayout.Stride = sizeof(float) * 6;
		FullMeshLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 } };
		VertexLayoutDesc PositionOnlyPipelineLayout;
		PositionOnlyPipelineLayout.Stride = FullMeshLayout.Stride;
		PositionOnlyPipelineLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 } };
		auto PositionOnlyPipeline = Pipeline;
		PositionOnlyPipeline.VertexLayout = PositionOnlyPipelineLayout;
		PF_CHECK(
			Tests, ValidateIndexedDrawArguments(IndexedTriangle, PositionOnlyPipeline, FullMeshLayout, ValidatedIndexCount).has_value());
		PositionOnlyPipeline.VertexLayout.Attributes[0].Format = VertexFormat::Float2;
		PF_CHECK(
			Tests, !ValidateIndexedDrawArguments(IndexedTriangle, PositionOnlyPipeline, FullMeshLayout, ValidatedIndexCount).has_value());

		auto NonIndexedDescription = Description;
		NonIndexedDescription.Indices = {};
		const auto NonIndexedMesh = ValidateMeshDescription(NonIndexedDescription);
		PF_CHECK(Tests, NonIndexedMesh && NonIndexedMesh->VertexCount == 3 && NonIndexedMesh->IndexCount == 0);
	}
} // namespace

void RunRendererContractTests(TestRunner& Tests)
{
	TestBufferDescriptionValidation(Tests);
	TestDynamicMeshCreationAndUpdates(Tests);
	TestShaderDescriptionAndSpirVValidation(Tests);
	TestVertexLayoutValidation(Tests);
	TestMeshDescriptionAndDrawValidation(Tests);
}
