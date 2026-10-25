#pragma once

#include "Assets/AssetImportProvenance.h"

#include <cstdint>
#include <string>

namespace PulseForge
{
	// Describes inspection progress, not successful import. Only a persisted success receipt can establish that
	// processing completed.
	enum class AssetSubresourceClassification : uint8_t
	{
		DiscoveredLocator,
		StructurallyInspected,
		SupportedCandidate,
		Imported,
		Invalid,
		Unsupported
	};

	struct AssetDependencyInputReceipt
	{
		std::string ProjectRelativePath;
		bool Exists = true;
		uint64_t ByteLength = 0;
		AssetContentFingerprint Content;
		friend bool operator==(const AssetDependencyInputReceipt&, const AssetDependencyInputReceipt&) = default;
	};
} // namespace PulseForge
