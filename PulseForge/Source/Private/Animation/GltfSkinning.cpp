#include "Animation/GltfSkinning.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>

namespace PulseForge
{
	namespace
	{
		struct PreparedJointTransform final
		{
			const std::array<float, 16>* Matrix = nullptr;
			std::array<double, 9> LinearMatrix{};
			std::array<double, 9> NormalMatrix{};
			double OrientationSign = 1.0;
		};

		[[nodiscard]] GltfSkinningError MakeError(GltfSkinningErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		[[nodiscard]] bool IsFiniteVertex(const GltfMeshVertex& Vertex)
		{
			return std::all_of(std::begin(Vertex.Position), std::end(Vertex.Position), [](float Value) { return std::isfinite(Value); }) &&
				std::all_of(std::begin(Vertex.Color), std::end(Vertex.Color), [](float Value) { return std::isfinite(Value); }) &&
				std::all_of(std::begin(Vertex.TexCoord), std::end(Vertex.TexCoord), [](float Value) { return std::isfinite(Value); }) &&
				std::all_of(std::begin(Vertex.Normal), std::end(Vertex.Normal), [](float Value) { return std::isfinite(Value); }) &&
				std::all_of(Vertex.Tangent.begin(), Vertex.Tangent.end(), [](float Value) { return std::isfinite(Value); });
		}

		[[nodiscard]] bool PrepareJointTransform(
			const std::array<float, 16>& Matrix,
			PreparedJointTransform& Output)
		{
			if (!std::all_of(Matrix.begin(), Matrix.end(), [](float Value) { return std::isfinite(Value); }) ||
				std::abs(Matrix[3]) > 1.0e-5f || std::abs(Matrix[7]) > 1.0e-5f ||
				std::abs(Matrix[11]) > 1.0e-5f || std::abs(Matrix[15] - 1.0f) > 1.0e-5f)
				return false;

			const double A = Matrix[0], B = Matrix[4], C = Matrix[8];
			const double D = Matrix[1], E = Matrix[5], F = Matrix[9];
			const double G = Matrix[2], H = Matrix[6], I = Matrix[10];
			const std::array<double, 9> Cofactors = {
				E * I - F * H, F * G - D * I, D * H - E * G,
				C * H - B * I, A * I - C * G, B * G - A * H,
				B * F - C * E, C * D - A * F, A * E - B * D
			};
			const double Determinant = A * Cofactors[0] + B * Cofactors[1] + C * Cofactors[2];
			if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.0e-10)
				return false;
			for (size_t Index = 0; Index < Cofactors.size(); ++Index)
				Output.NormalMatrix[Index] = Cofactors[Index] / Determinant;
			Output.LinearMatrix = { A, B, C, D, E, F, G, H, I };
			Output.OrientationSign = Determinant < 0.0 ? -1.0 : 1.0;
			Output.Matrix = &Matrix;
			return true;
		}

		[[nodiscard]] std::array<double, 3> TransformPoint(
			const std::array<float, 16>& Matrix,
			const float Point[3])
		{
			return {
				static_cast<double>(Matrix[0]) * Point[0] + static_cast<double>(Matrix[4]) * Point[1] +
					static_cast<double>(Matrix[8]) * Point[2] + Matrix[12],
				static_cast<double>(Matrix[1]) * Point[0] + static_cast<double>(Matrix[5]) * Point[1] +
					static_cast<double>(Matrix[9]) * Point[2] + Matrix[13],
				static_cast<double>(Matrix[2]) * Point[0] + static_cast<double>(Matrix[6]) * Point[1] +
					static_cast<double>(Matrix[10]) * Point[2] + Matrix[14]
			};
		}

		[[nodiscard]] std::array<double, 3> TransformDirection(
			const std::array<double, 9>& Matrix,
			const float Direction[3])
		{
			return {
				Matrix[0] * Direction[0] + Matrix[1] * Direction[1] + Matrix[2] * Direction[2],
				Matrix[3] * Direction[0] + Matrix[4] * Direction[1] + Matrix[5] * Direction[2],
				Matrix[6] * Direction[0] + Matrix[7] * Direction[1] + Matrix[8] * Direction[2]
			};
		}

		[[nodiscard]] bool Normalize(std::array<double, 3>& Vector)
		{
			const double LengthSquared = Vector[0] * Vector[0] + Vector[1] * Vector[1] + Vector[2] * Vector[2];
			if (!std::isfinite(LengthSquared) || LengthSquared <= 1.0e-20) return false;
			const double InverseLength = 1.0 / std::sqrt(LengthSquared);
			for (double& Component : Vector) Component *= InverseLength;
			return true;
		}

		[[nodiscard]] bool FitsFloat(double Value)
		{
			return std::isfinite(Value) && std::abs(Value) <= std::numeric_limits<float>::max();
		}

		[[nodiscard]] bool IsAffineMatrix(const std::array<float, 16>& Matrix)
		{
			return std::all_of(Matrix.begin(), Matrix.end(), [](float Value) { return std::isfinite(Value); }) &&
				std::abs(Matrix[3]) <= 1.0e-5f && std::abs(Matrix[7]) <= 1.0e-5f &&
				std::abs(Matrix[11]) <= 1.0e-5f && std::abs(Matrix[15] - 1.0f) <= 1.0e-5f;
		}

		[[nodiscard]] std::array<float, 16> MultiplyMatrices(
			const std::array<float, 16>& First,
			const std::array<float, 16>& Second)
		{
			std::array<float, 16> Result{};
			for (size_t Column = 0; Column < 4; ++Column)
				for (size_t Row = 0; Row < 4; ++Row)
				{
					double Value = 0.0;
					for (size_t Inner = 0; Inner < 4; ++Inner)
						Value += static_cast<double>(First[Inner * 4 + Row]) * Second[Column * 4 + Inner];
					if (!FitsFloat(Value)) return {};
					Result[Column * 4 + Row] = static_cast<float>(Value);
				}
			return Result;
		}

		[[nodiscard]] bool InvertMatrix(const std::array<float, 16>& Matrix, std::array<float, 16>& Inverse)
		{
			double Rows[4][8]{};
			for (size_t Row = 0; Row < 4; ++Row)
				for (size_t Column = 0; Column < 4; ++Column)
				{
					Rows[Row][Column] = Matrix[Column * 4 + Row];
					Rows[Row][Column + 4] = Row == Column ? 1.0 : 0.0;
				}

			for (size_t Pivot = 0; Pivot < 4; ++Pivot)
			{
				size_t BestRow = Pivot;
				for (size_t Row = Pivot + 1; Row < 4; ++Row)
					if (std::abs(Rows[Row][Pivot]) > std::abs(Rows[BestRow][Pivot])) BestRow = Row;
				if (!std::isfinite(Rows[BestRow][Pivot]) || std::abs(Rows[BestRow][Pivot]) <= 1.0e-12) return false;
				if (BestRow != Pivot)
					for (size_t Column = 0; Column < 8; ++Column)
						std::swap(Rows[Pivot][Column], Rows[BestRow][Column]);

				const double Divisor = Rows[Pivot][Pivot];
				for (double& Value : Rows[Pivot]) Value /= Divisor;
				for (size_t Row = 0; Row < 4; ++Row)
				{
					if (Row == Pivot) continue;
					const double Factor = Rows[Row][Pivot];
					for (size_t Column = 0; Column < 8; ++Column)
						Rows[Row][Column] -= Factor * Rows[Pivot][Column];
				}
			}

			for (size_t Row = 0; Row < 4; ++Row)
				for (size_t Column = 0; Column < 4; ++Column)
				{
					const double Value = Rows[Row][Column + 4];
					if (!FitsFloat(Value)) return false;
					Inverse[Column * 4 + Row] = static_cast<float>(Value);
				}
			return IsAffineMatrix(Inverse);
		}
	}

	std::expected<std::vector<std::array<float, 16>>, GltfSkinningError> BuildGltfJointPalette(
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const std::array<float, 16>> LocalNodeTransforms,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled, "Joint palette construction was cancelled."));
		if (Mesh.SkeletonNodes.empty() || Mesh.SkeletonNodes.size() > MaximumGltfSkinPaletteJoints ||
			LocalNodeTransforms.size() != Mesh.SkeletonNodes.size() || Mesh.MeshNodeIndex >= Mesh.SkeletonNodes.size() ||
			Mesh.JointNodeIndices.empty() || Mesh.JointNodeIndices.size() != Mesh.InverseBindMatrices.size() ||
			Mesh.JointNodeIndices.size() > MaximumGltfSkinPaletteJoints)
			return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
				"Skeleton transforms, mesh node, joints, and inverse bind matrices are inconsistent or out of bounds."));

		try
		{
			std::vector<std::array<float, 16>> GlobalTransforms(LocalNodeTransforms.size());
			for (size_t Index = 0; Index < LocalNodeTransforms.size(); ++Index)
			{
				if ((Index & 0x3fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled,
						"Joint palette construction was cancelled."));
				const int32_t ParentIndex = Mesh.SkeletonNodes[Index].ParentNodeIndex;
				if (!IsAffineMatrix(LocalNodeTransforms[Index]) || ParentIndex < -1 ||
					(ParentIndex >= 0 && static_cast<size_t>(ParentIndex) >= Index))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
						"Skeleton node transforms must be finite affine matrices in parent-before-child order."));
				GlobalTransforms[Index] = ParentIndex < 0 ? LocalNodeTransforms[Index] :
					MultiplyMatrices(GlobalTransforms[static_cast<size_t>(ParentIndex)], LocalNodeTransforms[Index]);
				if (!IsAffineMatrix(GlobalTransforms[Index]))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
						"Composed skeleton transform is non-finite or outside the affine float range."));
			}

			std::array<float, 16> InverseMeshTransform{};
			if (!InvertMatrix(GlobalTransforms[Mesh.MeshNodeIndex], InverseMeshTransform))
				return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidPalette,
					"Mesh node transform is singular and cannot define a skinning coordinate space."));

			std::vector<std::array<float, 16>> Palette;
			Palette.reserve(Mesh.JointNodeIndices.size());
			for (size_t Joint = 0; Joint < Mesh.JointNodeIndices.size(); ++Joint)
			{
				if ((Joint & 0x3fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled,
						"Joint palette construction was cancelled."));
				const uint16_t NodeIndex = Mesh.JointNodeIndices[Joint];
				if (NodeIndex >= Mesh.SkeletonNodes.size() || !Mesh.SkeletonNodes[NodeIndex].IsJoint ||
					!IsAffineMatrix(Mesh.InverseBindMatrices[Joint]))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
						"Skin joint map or inverse bind transform is invalid."));
				const auto WorldJointBind = MultiplyMatrices(GlobalTransforms[NodeIndex], Mesh.InverseBindMatrices[Joint]);
				const auto MeshLocalPalette = MultiplyMatrices(InverseMeshTransform, WorldJointBind);
				if (!IsAffineMatrix(MeshLocalPalette))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidPalette,
						"Composed mesh-local joint transform is invalid."));
				Palette.push_back(MeshLocalPalette);
			}
			return Palette;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(GltfSkinningErrorCode::ResourceLimitExceeded,
				"Insufficient memory while building the skin joint palette."));
		}
	}

	std::expected<std::vector<GltfMeshVertex>, GltfSkinningError> SkinGltfMeshVertices(
		const ImportedGltfSkeletalMesh& Mesh,
		std::span<const std::array<float, 16>> JointPalette,
		std::stop_token CancellationToken)
	{
		if (CancellationToken.stop_requested())
			return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled, "CPU skinning was cancelled."));
		if (Mesh.Geometry.Vertices.empty() || Mesh.Geometry.Vertices.size() > MaximumGltfSkinnedVertices ||
			Mesh.VertexInfluences.size() != Mesh.Geometry.Vertices.size() || Mesh.JointNodeIndices.empty() ||
			Mesh.JointNodeIndices.size() != JointPalette.size())
			return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
				"Skeletal mesh geometry, vertex influences, and palette sizes are inconsistent or out of bounds."));
		if (JointPalette.empty() || JointPalette.size() > MaximumGltfSkinPaletteJoints)
			return std::unexpected(MakeError(GltfSkinningErrorCode::ResourceLimitExceeded,
				"Joint palette is empty or exceeds the supported 4096-joint limit."));
		for (const uint32_t Index : Mesh.Geometry.Indices)
			if (Index >= Mesh.Geometry.Vertices.size())
				return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
					"Skeletal mesh index data references a missing vertex."));
		for (const uint16_t NodeIndex : Mesh.JointNodeIndices)
			if (NodeIndex >= Mesh.SkeletonNodes.size() || !Mesh.SkeletonNodes[NodeIndex].IsJoint)
				return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidMesh,
					"Skeletal mesh skin palette map references a missing or non-joint skeleton node."));

		try
		{
			std::vector<PreparedJointTransform> PreparedPalette(JointPalette.size());
			for (size_t Joint = 0; Joint < JointPalette.size(); ++Joint)
			{
				if ((Joint & 0x3fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled, "CPU skinning was cancelled."));
				if (!PrepareJointTransform(JointPalette[Joint], PreparedPalette[Joint]))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidPalette,
						"Joint palette contains a non-finite, non-affine, or singular matrix."));
			}

			std::vector<GltfMeshVertex> Result = Mesh.Geometry.Vertices;
			for (size_t VertexIndex = 0; VertexIndex < Result.size(); ++VertexIndex)
			{
				if ((VertexIndex & 0x7fu) == 0 && CancellationToken.stop_requested())
					return std::unexpected(MakeError(GltfSkinningErrorCode::Cancelled, "CPU skinning was cancelled."));
				const GltfMeshVertex& Source = Mesh.Geometry.Vertices[VertexIndex];
				const GltfVertexInfluences& Influence = Mesh.VertexInfluences[VertexIndex];
				if (!IsFiniteVertex(Source))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidVertex,
						"Skeletal mesh contains a non-finite vertex attribute."));

				double WeightSum = 0.0;
				std::array<double, 3> Position{};
				std::array<double, 3> Normal{};
				std::array<double, 3> Tangent{};
				double Orientation = 0.0;
				for (size_t Slot = 0; Slot < Influence.Weights.size(); ++Slot)
				{
					const float Weight = Influence.Weights[Slot];
					if (!std::isfinite(Weight) || Weight < 0.0f)
						return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidInfluence,
							"Vertex influence weight must be finite and nonnegative."));
					if (Weight == 0.0f) continue;
					const uint16_t JointIndex = Influence.JointIndices[Slot];
					if (JointIndex >= PreparedPalette.size())
						return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidInfluence,
							"Vertex influence references a joint outside the supplied skin palette."));
					const PreparedJointTransform& Joint = PreparedPalette[JointIndex];
					const auto TransformedPosition = TransformPoint(*Joint.Matrix, Source.Position);
					const auto TransformedNormal = TransformDirection(Joint.NormalMatrix, Source.Normal);
					const auto TransformedTangent = TransformDirection(Joint.LinearMatrix, Source.Tangent.data());
					for (size_t Component = 0; Component < 3; ++Component)
					{
						Position[Component] += Weight * TransformedPosition[Component];
						Normal[Component] += Weight * TransformedNormal[Component];
						Tangent[Component] += Weight * TransformedTangent[Component];
					}
					Orientation += Weight * Joint.OrientationSign;
					WeightSum += Weight;
				}
				if (!std::isfinite(WeightSum) || std::abs(WeightSum - 1.0) > 1.0e-4 || !Normalize(Normal) ||
					!std::isfinite(Orientation) || std::abs(Orientation) <= 1.0e-8)
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidInfluence,
						"Vertex influences must be normalized and produce nonzero normal and tangent directions."));
				const double NormalTangentDot = Normal[0] * Tangent[0] + Normal[1] * Tangent[1] + Normal[2] * Tangent[2];
			for (size_t Component = 0; Component < 3; ++Component)
				Tangent[Component] -= NormalTangentDot * Normal[Component];
			if (!Normalize(Tangent))
				return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidVertex,
					"Skinned vertex tangent is degenerate after orthogonalization."));
				if (!FitsFloat(Position[0]) || !FitsFloat(Position[1]) || !FitsFloat(Position[2]))
					return std::unexpected(MakeError(GltfSkinningErrorCode::InvalidVertex,
						"Skinned vertex position exceeds the supported float range."));

				for (size_t Component = 0; Component < 3; ++Component)
				{
					Result[VertexIndex].Position[Component] = static_cast<float>(Position[Component]);
					Result[VertexIndex].Normal[Component] = static_cast<float>(Normal[Component]);
					Result[VertexIndex].Tangent[Component] = static_cast<float>(Tangent[Component]);
				}
				Result[VertexIndex].Tangent[3] = Source.Tangent[3] * (Orientation < 0.0 ? -1.0f : 1.0f);
			}
			return Result;
		}
		catch (const std::bad_alloc&)
		{
			return std::unexpected(MakeError(GltfSkinningErrorCode::ResourceLimitExceeded,
				"Insufficient memory while preparing CPU-skinned vertices."));
		}
	}
}
