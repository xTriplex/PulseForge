#include "Renderer/Buffer.h"

#include <iostream>
#include <string_view>

namespace
{
	class TestRunner
	{
	public:
		void Check(bool Passed, std::string_view Expression)
		{
			++m_CheckCount;
			if (Passed)
				return;

			++m_FailureCount;
			std::cerr << "Buffer update check failed: " << Expression << '\n';
		}

		int Finish() const
		{
			if (m_FailureCount == 0)
				std::cout << "Passed " << m_CheckCount << " buffer update checks.\n";
			else
				std::cerr << m_FailureCount << " of " << m_CheckCount << " buffer update checks failed.\n";

			return m_FailureCount == 0 ? 0 : 1;
		}

	private:
		int m_CheckCount = 0;
		int m_FailureCount = 0;
	};
}

int main()
{
	TestRunner Tests;
	const PulseForge::BufferDesc ConstantBuffer{ 64, PulseForge::BufferUsage::Constant, "test constants" };
	const PulseForge::BufferDesc VertexBuffer{ 64, PulseForge::BufferUsage::Vertex, "test vertices" };
	const PulseForge::BufferDesc IndexBuffer{ 64, PulseForge::BufferUsage::Index, "test indices" };
	const PulseForge::BufferDesc DynamicVertexBuffer{ 64, PulseForge::BufferUsage::Vertex, "dynamic vertices", true };
	const PulseForge::BufferDesc DynamicIndexBuffer{ 64, PulseForge::BufferUsage::Index, "dynamic indices", true };
	const auto HasError = [](const PulseForge::BufferDesc& Description,
		uint64_t Offset,
		size_t Size,
		PulseForge::BufferUpdateErrorCode Code)
	{
		const auto Result = PulseForge::ValidateBufferUpdate(Description, Offset, Size);
		return !Result && Result.error().Code == Code;
	};

	Tests.Check(PulseForge::ValidateBufferUpdate(ConstantBuffer, 0, 64).has_value(), "full constant buffer update is valid");
	Tests.Check(PulseForge::ValidateBufferUpdate(ConstantBuffer, 16, 16).has_value(), "aligned constant buffer subrange is valid");
	Tests.Check(
		HasError(VertexBuffer, 0, 16, PulseForge::BufferUpdateErrorCode::UnsupportedUsage),
		"immutable vertex buffer updates are rejected");
	Tests.Check(
		HasError(IndexBuffer, 0, 4, PulseForge::BufferUpdateErrorCode::UnsupportedUsage),
		"immutable index buffer updates are rejected");
	Tests.Check(
		PulseForge::ValidateBufferDescription(DynamicIndexBuffer, 0).has_value(),
		"dynamic index buffer descriptions are valid");
	Tests.Check(
		PulseForge::ValidateBufferUpdate(DynamicVertexBuffer, 0, 16).has_value(),
		"dynamic vertex buffer updates are valid");
	Tests.Check(
		PulseForge::ValidateBufferUpdate(DynamicVertexBuffer, 16, 32).has_value(),
		"dynamic vertex buffer subrange updates are valid");
	Tests.Check(
		PulseForge::ValidateBufferUpdate(DynamicIndexBuffer, 0, 12).has_value(),
		"dynamic index buffer updates are valid");
	Tests.Check(
		HasError(DynamicVertexBuffer, 0, 0, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"empty dynamic geometry updates are rejected");
	Tests.Check(
		HasError(DynamicVertexBuffer, 2, 4, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"unaligned dynamic geometry update offsets are rejected");
	Tests.Check(
		HasError(DynamicVertexBuffer, 0, 6, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"non-four-byte dynamic geometry update sizes are rejected");
	Tests.Check(
		HasError(DynamicIndexBuffer, 64, 4, PulseForge::BufferUpdateErrorCode::OutOfBounds),
		"dynamic index updates extending beyond the buffer are rejected");
	Tests.Check(
		HasError(DynamicIndexBuffer, 80, 4, PulseForge::BufferUpdateErrorCode::OutOfBounds),
		"dynamic index update offsets beyond the buffer are rejected");
	Tests.Check(
		HasError(ConstantBuffer, 0, 0, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"empty updates are rejected");
	Tests.Check(
		HasError(ConstantBuffer, 2, 4, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"unaligned update offsets are rejected");
	Tests.Check(
		HasError(ConstantBuffer, 0, 6, PulseForge::BufferUpdateErrorCode::InvalidUpdateSize),
		"non-four-byte update sizes are rejected");
	Tests.Check(
		HasError(ConstantBuffer, 64, 4, PulseForge::BufferUpdateErrorCode::OutOfBounds),
		"updates extending beyond the buffer are rejected");
	Tests.Check(
		HasError(ConstantBuffer, 80, 4, PulseForge::BufferUpdateErrorCode::OutOfBounds),
		"offsets beyond the buffer are rejected");

	auto DynamicConstantBuffer = ConstantBuffer;
	DynamicConstantBuffer.IsDynamic = true;
	Tests.Check(
		!PulseForge::ValidateBufferDescription(DynamicConstantBuffer, 0).has_value(),
		"the dynamic geometry flag is rejected for constant buffers");
	Tests.Check(
		HasError(DynamicConstantBuffer, 0, 16, PulseForge::BufferUpdateErrorCode::UnsupportedUsage),
		"dynamic constant-buffer updates are rejected as an invalid usage combination");

	const PulseForge::BufferDesc LargeDynamicVertex{ 131072, PulseForge::BufferUsage::Vertex, "large dynamic vertices", true };
	Tests.Check(
		PulseForge::ValidateBufferUpdate(LargeDynamicVertex, 0, 65540).has_value(),
		"large dynamic geometry update sizes pass validation");

	return Tests.Finish();
}
