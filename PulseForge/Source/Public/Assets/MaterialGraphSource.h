#pragma once

#include "Assets/AssetID.h"
#include "Core/Core.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	enum class MaterialGraphNodeKind : uint8_t
	{
		FloatConstant,
		ColorConstant,
		FloatParameter,
		Add,
		Multiply,
		Reroute,
		UV0,
		BaseColorTextureSample,
		PbrOutput
	};

	struct MaterialGraphNode final
	{
		UUID ID;
		MaterialGraphNodeKind Kind = MaterialGraphNodeKind::FloatConstant;
		float Value = 0.0f;
		std::string ParameterName;
		double X = 0.0;
		double Y = 0.0;
		std::array<float, 3> Color{ 1.0f, 1.0f, 1.0f };
		friend bool operator==(const MaterialGraphNode&, const MaterialGraphNode&) = default;
	};

	struct MaterialGraphConnection final
	{
		UUID ID;
		UUID OutputNode;
		UUID InputNode;
		uint8_t InputSlot = 0;
		friend bool operator==(const MaterialGraphConnection&, const MaterialGraphConnection&) = default;
	};

	struct MaterialGraphSource final
	{
		static constexpr uint32_t CurrentVersion = 3;
		uint32_t Version = CurrentVersion;
		UUID GraphID;
		std::vector<MaterialGraphNode> Nodes;
		std::vector<MaterialGraphConnection> Connections;
		friend bool operator==(const MaterialGraphSource&, const MaterialGraphSource&) = default;
	};

	struct MaterialGraphDiagnostic final
	{
		UUID Node;
		UUID Pin;
		std::string Message;
	};

	struct MaterialGraphValidation final
	{
		std::vector<MaterialGraphDiagnostic> Diagnostics;
		[[nodiscard]] bool IsValid() const noexcept { return Diagnostics.empty(); }
	};

	enum class MaterialGraphExpressionOp : uint8_t
	{
		FloatConstant,
		ColorConstant,
		FloatParameter,
		Add,
		Multiply,
		UV0,
		BaseColorTextureSample
	};

	struct MaterialGraphExpression final
	{
		UUID SourceNode;
		MaterialGraphExpressionOp Op = MaterialGraphExpressionOp::FloatConstant;
		uint32_t Left = 0;
		uint32_t Right = 0;
		float Value = 0.0f;
		std::array<float, 3> Color{ 1.0f, 1.0f, 1.0f };
		std::string ParameterName;
	};

	struct MaterialGraphIR final
	{
		uint32_t Version = 1;
		UUID SourceGraph;
		UUID OutputNode;
		uint32_t OutputExpression = 0;
		std::vector<MaterialGraphExpression> Expressions;
	};

	[[nodiscard]] PULSEFORGE_API MaterialGraphValidation ValidateMaterialGraphSource(const MaterialGraphSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<MaterialGraphIR, MaterialGraphValidation> LowerMaterialGraphToIR(
		const MaterialGraphSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<std::string, std::string> SerializeMaterialGraphSource(
		const MaterialGraphSource& Source);
	[[nodiscard]] PULSEFORGE_API std::expected<MaterialGraphSource, std::string> DeserializeMaterialGraphSource(
		std::string_view Bytes);
}
