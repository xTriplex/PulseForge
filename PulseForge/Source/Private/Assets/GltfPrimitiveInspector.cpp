#include "Assets/Detail/GltfPrimitiveInspector.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/Detail/GltfSourceDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>

namespace PulseForge::Assets::Detail
{
	namespace
	{
		using Json = nlohmann::ordered_json;
		constexpr uint64_t MaximumBufferBytes = 512ull * 1024ull * 1024ull;
		constexpr size_t MaximumCollectionEntries = 1'000'000;

		GltfPrimitiveInspection Invalid(std::string Message)
		{
			return { AssetSubresourceClassification::Invalid, std::move(Message) };
		}

		GltfPrimitiveInspection Unsupported(std::string Message)
		{
			return { AssetSubresourceClassification::Unsupported, std::move(Message) };
		}

		const Json* Find(const Json& Value, std::string_view Key)
		{
			if (!Value.is_object()) return nullptr;
			const auto It = Value.find(std::string(Key));
			return It == Value.end() ? nullptr : &*It;
		}

		std::optional<uint64_t> Unsigned(const Json* Value)
		{
			if (Value == nullptr || (!Value->is_number_unsigned() && !Value->is_number_integer())) return std::nullopt;
			if (Value->is_number_integer() && Value->get<int64_t>() < 0) return std::nullopt;
			return Value->get<uint64_t>();
		}

		bool Add(uint64_t First, uint64_t Second, uint64_t& Result)
		{
			if (Second > std::numeric_limits<uint64_t>::max() - First) return false;
			Result = First + Second;
			return true;
		}

		bool Multiply(uint64_t First, uint64_t Second, uint64_t& Result)
		{
			if (First != 0 && Second > std::numeric_limits<uint64_t>::max() / First) return false;
			Result = First * Second;
			return true;
		}

		std::optional<uint64_t> ComponentSize(uint64_t ComponentType)
		{
			switch (ComponentType)
			{
			case 5120:
			case 5121: return 1;
			case 5122:
			case 5123: return 2;
			case 5125:
			case 5126: return 4;
			default: return std::nullopt;
			}
		}

		std::optional<uint64_t> TypeComponents(const Json* Value)
		{
			if (Value == nullptr || !Value->is_string()) return std::nullopt;
			const std::string Type = Value->get<std::string>();
			if (Type == "SCALAR") return 1;
			if (Type == "VEC2") return 2;
			if (Type == "VEC3") return 3;
			if (Type == "VEC4") return 4;
			if (Type == "MAT2") return 4;
			if (Type == "MAT3") return 9;
			if (Type == "MAT4") return 16;
			return std::nullopt;
		}

		int Base64Value(char Character)
		{
			if (Character >= 'A' && Character <= 'Z') return Character - 'A';
			if (Character >= 'a' && Character <= 'z') return Character - 'a' + 26;
			if (Character >= '0' && Character <= '9') return Character - '0' + 52;
			if (Character == '+') return 62;
			if (Character == '/') return 63;
			return -1;
		}

		struct BufferValidationError
		{
			bool IsUnsupported = false;
			std::string Message;
		};

		std::expected<uint64_t, BufferValidationError> ValidateEmbeddedBuffer(std::string_view Uri)
		{
			constexpr std::string_view Octet = "data:application/octet-stream;base64,";
			constexpr std::string_view Gltf = "data:application/gltf-buffer;base64,";
			std::string_view Encoded;
			if (Uri.starts_with(Octet)) Encoded = Uri.substr(Octet.size());
			else if (Uri.starts_with(Gltf)) Encoded = Uri.substr(Gltf.size());
			else return std::unexpected(BufferValidationError{ true, "embedded data URI is not a supported base64 buffer encoding" });
			if (Encoded.empty() || Encoded.size() % 4 != 0 || Encoded.size() > MaximumBufferBytes * 2)
				return std::unexpected(BufferValidationError{ false, "embedded buffer has an invalid or oversized base64 payload" });

			uint64_t DecodedLength = static_cast<uint64_t>(Encoded.size() / 4) * 3;
			if (Encoded.back() == '=') --DecodedLength;
			if (Encoded.size() >= 2 && Encoded[Encoded.size() - 2] == '=') --DecodedLength;
			for (size_t Offset = 0; Offset < Encoded.size(); Offset += 4)
			{
				const bool Last = Offset + 4 == Encoded.size();
				const char A = Encoded[Offset];
				const char B = Encoded[Offset + 1];
				const char C = Encoded[Offset + 2];
				const char D = Encoded[Offset + 3];
				const int VA = Base64Value(A);
				const int VB = Base64Value(B);
				const int VC = C == '=' ? 0 : Base64Value(C);
				const int VD = D == '=' ? 0 : Base64Value(D);
				const bool PadC = C == '=';
				const bool PadD = D == '=';
				if (VA < 0 || VB < 0 || VC < 0 || VD < 0 || (PadC && (!PadD || !Last || (VB & 0x0f) != 0)) ||
					(PadD && (!Last || (!PadC && (VC & 0x03) != 0))) || (!Last && (PadC || PadD)))
					return std::unexpected(BufferValidationError{ false, "embedded buffer contains malformed base64 data" });
			}
			if (DecodedLength > MaximumBufferBytes)
				return std::unexpected(BufferValidationError{ false, "decoded embedded buffer exceeds the supported limit" });
			return DecodedLength;
		}

		std::expected<void, BufferValidationError> ValidateBuffers(
			const Json& Document,
			const std::filesystem::path& SourcePath,
			const std::filesystem::path& ProjectRoot,
			std::optional<uint64_t> GlbBinaryByteLength,
			std::vector<uint64_t>& DeclaredLengths)
		{
			const Json* Buffers = Find(Document, "buffers");
			if (Buffers == nullptr || !Buffers->is_array() || Buffers->empty() || Buffers->size() > MaximumCollectionEntries)
				return std::unexpected(BufferValidationError{ false, "supported static mesh documents require a bounded, non-empty buffers array" });
			DeclaredLengths.reserve(Buffers->size());
			uint64_t TotalLength = 0;
			for (size_t Index = 0; Index < Buffers->size(); ++Index)
			{
				const Json& Buffer = (*Buffers)[Index];
				const auto ByteLength = Unsigned(Find(Buffer, "byteLength"));
				if (!Buffer.is_object() || !ByteLength || *ByteLength == 0 || *ByteLength > MaximumBufferBytes ||
					TotalLength > MaximumBufferBytes - *ByteLength)
					return std::unexpected(BufferValidationError{ false, "buffer byteLength is invalid or exceeds the supported limit" });
				TotalLength += *ByteLength;
				DeclaredLengths.push_back(*ByteLength);
				const Json* UriValue = Find(Buffer, "uri");
				if (UriValue == nullptr)
				{
					if (Index != 0 || !GlbBinaryByteLength || *ByteLength > *GlbBinaryByteLength ||
						*GlbBinaryByteLength - *ByteLength > 3)
						return std::unexpected(BufferValidationError{ false, "buffer without a URI has no matching supported GLB BIN chunk" });
					continue;
				}
				if (!UriValue->is_string()) return std::unexpected(BufferValidationError{ false, "buffer URI must be a string" });
				const std::string Uri = UriValue->get<std::string>();
				if (Uri.starts_with("data:"))
				{
					auto DecodedLength = ValidateEmbeddedBuffer(Uri);
					if (!DecodedLength) return std::unexpected(DecodedLength.error());
					if (*DecodedLength < *ByteLength)
						return std::unexpected(BufferValidationError{ false, "decoded embedded buffer is shorter than its declared byteLength" });
					continue;
				}
				if (HasUriScheme(Uri))
					return std::unexpected(BufferValidationError{ true, "external buffer URI uses an unsupported scheme" });
				auto Relative = DecodeGltfBufferUri(Uri);
				if (!Relative || !*Relative)
					return std::unexpected(BufferValidationError{ !Relative, Relative ? "buffer URI unexpectedly resolved as embedded" : Relative.error() });
				const std::filesystem::path Absolute = (SourcePath.parent_path() / **Relative).lexically_normal();
				const std::filesystem::path ProjectRelative = Absolute.lexically_relative(ProjectRoot);
				auto Managed = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, ProjectRelative);
				if (!Managed) return std::unexpected(BufferValidationError{ false, "external buffer is not a safe managed source: " + Managed.error().Message });
				std::error_code Error;
				const uintmax_t ActualLength = std::filesystem::file_size(*Managed, Error);
				if (Error || ActualLength < *ByteLength)
					return std::unexpected(BufferValidationError{ false, "external buffer is missing or shorter than its declared byteLength" });
			}
			return {};
		}

		std::expected<uint64_t, std::string> ValidateAccessor(
			const Json& Document, std::span<const uint64_t> BufferLengths, uint64_t AccessorIndex, bool Position);

		GltfPrimitiveInspection InspectParsedPrimitive(const Json& Document,
			std::span<const uint64_t> BufferLengths, uint32_t MeshIndex, uint32_t PrimitiveIndex)
		{
			const Json* Meshes = Find(Document, "meshes");
			if (Meshes == nullptr || !Meshes->is_array() || MeshIndex >= Meshes->size())
				return Invalid("mesh index is outside the meshes array");
			const Json& Mesh = (*Meshes)[MeshIndex];
			const Json* Primitives = Find(Mesh, "primitives");
			if (!Mesh.is_object() || Primitives == nullptr || !Primitives->is_array() || PrimitiveIndex >= Primitives->size())
				return Invalid("primitive index is outside the mesh primitives array");
			const Json& Primitive = (*Primitives)[PrimitiveIndex];
			if (!Primitive.is_object()) return Invalid("primitive entry must be an object");
			if (const Json* Extensions = Find(Primitive, "extensions"); Extensions != nullptr &&
				Find(*Extensions, "KHR_draco_mesh_compression") != nullptr)
				return Unsupported("KHR_draco_mesh_compression primitives are not supported");
			if (const Json* Targets = Find(Primitive, "targets"); Targets != nullptr)
			{
				if (!Targets->is_array()) return Invalid("primitive targets must be an array");
				if (!Targets->empty()) return Unsupported("morph target geometry is not supported");
			}
			if (const Json* Mode = Find(Primitive, "mode"); Mode != nullptr)
			{
				const auto ModeValue = Unsigned(Mode);
				if (!ModeValue) return Invalid("primitive mode must be a non-negative integer");
				if (*ModeValue != 4) return Unsupported("only triangle-list primitives are supported");
			}
			const Json* Attributes = Find(Primitive, "attributes");
			if (Attributes == nullptr || !Attributes->is_object()) return Invalid("primitive requires an attributes object");
			const auto PositionIndex = Unsigned(Find(*Attributes, "POSITION"));
			if (!PositionIndex) return Invalid("primitive requires a valid POSITION accessor index");
			const auto PositionCount = ValidateAccessor(Document, BufferLengths, *PositionIndex, true);
			if (!PositionCount) return Invalid(PositionCount.error());
			if (const Json* Indices = Find(Primitive, "indices"); Indices != nullptr)
			{
				const auto Index = Unsigned(Indices);
				if (!Index) return Invalid("primitive indices must identify a valid accessor");
				const auto IndexCount = ValidateAccessor(Document, BufferLengths, *Index, false);
				if (!IndexCount) return Invalid(IndexCount.error());
			}
			return { AssetSubresourceClassification::SupportedCandidate, {} };
		}

		std::expected<Json, GltfPrimitiveInspection> ParseAndValidateDocument(std::string_view JsonText,
			const std::filesystem::path& SourcePath, const std::filesystem::path& ProjectRoot,
			std::optional<uint64_t> GlbBinaryByteLength, std::vector<uint64_t>& BufferLengths)
		{
			Json Document;
			try
			{
				Document = Json::parse(JsonText.begin(), JsonText.end());
			}
			catch (const nlohmann::json::exception& Exception)
			{
				return std::unexpected(Invalid(std::string("glTF primitive structure is malformed: ") + Exception.what()));
			}
			if (!Document.is_object()) return std::unexpected(Invalid("glTF document root must be an object"));
			const Json* Asset = Find(Document, "asset");
			if (Asset == nullptr || !Asset->is_object()) return std::unexpected(Invalid("glTF asset must be an object"));
			const Json* Version = Find(*Asset, "version");
			if (Version == nullptr || !Version->is_string() || Version->get<std::string>() != "2.0")
				return std::unexpected(Invalid("glTF asset.version must be 2.0"));
			if (const Json* Required = Find(Document, "extensionsRequired"); Required != nullptr)
			{
				if (!Required->is_array() || Required->size() > MaximumCollectionEntries)
					return std::unexpected(Invalid("glTF extensionsRequired must be a bounded array"));
				for (const Json& Extension : *Required)
					if (!Extension.is_string()) return std::unexpected(Invalid("glTF required extension entries must be strings"));
				if (!Required->empty())
					return std::unexpected(Unsupported("static mesh importer does not support required glTF extension '" +
						Required->front().get<std::string>() + "'"));
			}
			auto Buffers = ValidateBuffers(Document, SourcePath, ProjectRoot, GlbBinaryByteLength, BufferLengths);
			if (!Buffers)
				return std::unexpected(Buffers.error().IsUnsupported ? Unsupported(Buffers.error().Message) : Invalid(Buffers.error().Message));
			return Document;
		}

		std::expected<uint64_t, std::string> ValidateAccessor(
			const Json& Document,
			std::span<const uint64_t> BufferLengths,
			uint64_t AccessorIndex,
			bool Position)
		{
			const Json* Accessors = Find(Document, "accessors");
			const Json* Views = Find(Document, "bufferViews");
			if (Accessors == nullptr || !Accessors->is_array() || Accessors->size() > MaximumCollectionEntries ||
				AccessorIndex >= Accessors->size())
				return std::unexpected("primitive references a missing or invalid accessor index");
			const Json& Accessor = (*Accessors)[static_cast<size_t>(AccessorIndex)];
			if (!Accessor.is_object()) return std::unexpected("accessor entry must be an object");
			if (Find(Accessor, "sparse") != nullptr) return std::unexpected("sparse accessors are not supported by the static mesh importer");
			const auto ViewIndex = Unsigned(Find(Accessor, "bufferView"));
			const auto Count = Unsigned(Find(Accessor, "count"));
			const auto ComponentType = Unsigned(Find(Accessor, "componentType"));
			const auto Components = TypeComponents(Find(Accessor, "type"));
			if (!ViewIndex || !Count || *Count == 0 || !ComponentType || !Components || Views == nullptr || !Views->is_array() ||
				Views->size() > MaximumCollectionEntries || *ViewIndex >= Views->size())
				return std::unexpected("accessor has an invalid bufferView, count, componentType, or type");
			const Json* Normalized = Find(Accessor, "normalized");
			if (Normalized != nullptr && !Normalized->is_boolean())
				return std::unexpected("accessor normalized must be a boolean");
			if (Position && (*ComponentType != 5126 || *Components != 3 ||
				(Normalized != nullptr && Normalized->get<bool>())))
				return std::unexpected("POSITION must be a non-normalized float VEC3 accessor");
			if (Position && *Count > 10'000'000)
				return std::unexpected("POSITION accessor exceeds the importer vertex limit");
			if (!Position && (*Components != 1 || (*ComponentType != 5121 && *ComponentType != 5123 && *ComponentType != 5125)))
				return std::unexpected("indices must use an unsigned scalar accessor");
			if (!Position && *Count > 30'000'000)
				return std::unexpected("indices accessor exceeds the importer index limit");
			if (!Position && Normalized != nullptr && Normalized->get<bool>())
				return std::unexpected("indices accessor must not be normalized");
			const Json& View = (*Views)[static_cast<size_t>(*ViewIndex)];
			if (!View.is_object()) return std::unexpected("bufferView entry must be an object");
			const auto BufferIndex = Unsigned(Find(View, "buffer"));
			const auto ViewLength = Unsigned(Find(View, "byteLength"));
			const Json* ViewOffsetValue = Find(View, "byteOffset");
			const auto ParsedViewOffset = Unsigned(ViewOffsetValue);
			if (ViewOffsetValue != nullptr && !ParsedViewOffset)
				return std::unexpected("bufferView byteOffset must be a non-negative integer");
			const uint64_t ViewOffset = ParsedViewOffset.value_or(0);
			if (!BufferIndex || !ViewLength || *ViewLength == 0 || *BufferIndex >= BufferLengths.size())
				return std::unexpected("bufferView has an invalid buffer index or byteLength");
			uint64_t ViewEnd = 0;
			if (!Add(ViewOffset, *ViewLength, ViewEnd) || ViewEnd > BufferLengths[static_cast<size_t>(*BufferIndex)])
				return std::unexpected("bufferView byte range exceeds its declared buffer bounds");
			const auto ComponentBytes = ComponentSize(*ComponentType);
			if (!ComponentBytes) return std::unexpected("accessor componentType is unsupported");
			uint64_t ElementBytes = 0;
			if (!Multiply(*ComponentBytes, *Components, ElementBytes)) return std::unexpected("accessor element size overflows");
			const Json* AccessorOffsetValue = Find(Accessor, "byteOffset");
			const auto ParsedAccessorOffset = Unsigned(AccessorOffsetValue);
			if (AccessorOffsetValue != nullptr && !ParsedAccessorOffset)
				return std::unexpected("accessor byteOffset must be a non-negative integer");
			const uint64_t AccessorOffset = ParsedAccessorOffset.value_or(0);
			const Json* StrideValue = Find(View, "byteStride");
			const auto ParsedStride = Unsigned(StrideValue);
			if (StrideValue != nullptr && !ParsedStride)
				return std::unexpected("bufferView byteStride must be a non-negative integer");
			const uint64_t Stride = ParsedStride.value_or(ElementBytes);
			if (Stride < ElementBytes || (Find(View, "byteStride") != nullptr && (Stride < 4 || Stride > 252 || Stride % 4 != 0)))
				return std::unexpected("bufferView byteStride is outside the supported range");
			uint64_t AbsoluteOffset = 0;
			if (!Add(ViewOffset, AccessorOffset, AbsoluteOffset) || AccessorOffset % *ComponentBytes != 0 ||
				AbsoluteOffset % *ComponentBytes != 0)
				return std::unexpected("accessor byteOffset is not component-aligned");
			uint64_t Tail = 0;
			uint64_t End = 0;
			if (!Multiply(*Count - 1, Stride, Tail) || !Add(AccessorOffset, Tail, End) || !Add(End, ElementBytes, End) || End > *ViewLength)
				return std::unexpected("accessor byte range exceeds its bufferView bounds");
			return *Count;
		}
	} // namespace

	std::vector<GltfPrimitiveInspection> InspectStaticPrimitives(std::string_view JsonText,
		const std::filesystem::path& SourcePath, const std::filesystem::path& ProjectRoot,
		std::span<const GltfPrimitiveLocator> Locators, std::optional<uint64_t> GlbBinaryByteLength)
	{
		std::vector<GltfPrimitiveInspection> Results;
		Results.reserve(Locators.size());
		try
		{
			std::vector<uint64_t> BufferLengths;
			auto Document = ParseAndValidateDocument(JsonText, SourcePath, ProjectRoot, GlbBinaryByteLength, BufferLengths);
			if (!Document)
			{
				std::fill_n(std::back_inserter(Results), Locators.size(), Document.error());
				return Results;
			}
			for (const GltfPrimitiveLocator& Locator : Locators)
				Results.push_back(InspectParsedPrimitive(*Document, BufferLengths, Locator.MeshIndex, Locator.PrimitiveIndex));
		}
		catch (const std::exception& Exception)
		{
			std::fill_n(std::back_inserter(Results), Locators.size(), Invalid(std::string("glTF primitive inspection failed: ") + Exception.what()));
		}
		return Results;
	}

	GltfPrimitiveInspection InspectStaticPrimitive(std::string_view JsonText, const std::filesystem::path& SourcePath,
		const std::filesystem::path& ProjectRoot, uint32_t MeshIndex, uint32_t PrimitiveIndex,
		std::optional<uint64_t> GlbBinaryByteLength)
	{
		const std::array<GltfPrimitiveLocator, 1> Locator{ GltfPrimitiveLocator{ MeshIndex, PrimitiveIndex } };
		auto Results = InspectStaticPrimitives(JsonText, SourcePath, ProjectRoot, Locator, GlbBinaryByteLength);
		return Results.empty() ? Invalid("glTF primitive inspection produced no result") : std::move(Results.front());
	}
} // namespace PulseForge::Assets::Detail
