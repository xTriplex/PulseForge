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
		"vertex buffer updates are explicitly unsupported");
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

	return Tests.Finish();
}
