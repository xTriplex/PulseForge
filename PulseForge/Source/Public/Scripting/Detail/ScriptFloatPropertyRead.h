#pragma once

#include "Scene/PropertyMetadata.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <utility>

namespace PulseForge::ScriptRuntimeDetail
{
	enum class ScriptFloatPropertyReadState : uint8_t
	{
		Value,
		Missing,
		Failed
	};

	struct ScriptFloatPropertyReadResult
	{
		ScriptFloatPropertyReadState State = ScriptFloatPropertyReadState::Missing;
		float Value = 0.0f;
		std::array<char, 512> Failure{};
	};

	template <typename ReadOperation>
	[[nodiscard]] ScriptFloatPropertyReadResult CaptureScriptFloatPropertyRead(ReadOperation&& Operation) noexcept
	{
		ScriptFloatPropertyReadResult Result;
		const auto CopyFailure = [&Result](std::string_view Message) noexcept
		{
			const size_t Length = (std::min)(Message.size(), Result.Failure.size() - 1);
			std::copy_n(Message.data(), Length, Result.Failure.data());
			Result.Failure[Length] = '\0';
			Result.State = ScriptFloatPropertyReadState::Failed;
		};

		try
		{
			{
				auto Property = std::forward<ReadOperation>(Operation)();
				if (Property)
				{
					const float* Value = std::get_if<float>(&*Property);
					if (Value)
					{
						Result.Value = *Value;
						Result.State = ScriptFloatPropertyReadState::Value;
					}
					else CopyFailure("Script float property lookup returned a non-float value");
				}
				else if (Property.error().Code == PropertyAccessErrorCode::UnknownProperty)
					Result.State = ScriptFloatPropertyReadState::Missing;
				else CopyFailure(Property.error().Message);
			}
		}
		catch (const std::exception& Exception)
		{
			CopyFailure(Exception.what());
		}
		catch (...)
		{
			CopyFailure("Script float property lookup failed");
		}
		return Result;
	}
} // namespace PulseForge::ScriptRuntimeDetail
