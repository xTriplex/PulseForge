#include "Assets/MaterialGraphCompiler.h"

#include "Assets/AssetImportProvenance.h"
#include "Assets/DerivedDataArtifactStore.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace PulseForge
{
	std::string GetMaterialGraphCompilerToolchainFingerprint()
	{
#if defined(PF_HOST_DXC_EXECUTABLE) && defined(PF_HOST_DXC_VERSION)
		const auto FingerprintFile = [](const std::filesystem::path& Path) -> std::optional<AssetContentFingerprint>
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size == 0 || Size > 256ull * 1024ull * 1024ull) return std::nullopt;
			std::ifstream Stream(Path, std::ios::binary);
			if (!Stream) return std::nullopt;
			AssetFingerprintBuilder Builder("PulseForge.AssetInputBytes.v1");
			std::array<char, 32> LengthBuffer{};
			const auto [LengthEnd, LengthError] = std::to_chars(
				LengthBuffer.data(), LengthBuffer.data() + LengthBuffer.size(), Size);
			if (LengthError != std::errc{}) return std::nullopt;
			Builder.UpdateText("byteLength", std::string_view(LengthBuffer.data(),
				static_cast<size_t>(LengthEnd - LengthBuffer.data())));
			std::array<std::byte, 64 * 1024> Buffer{};
			uintmax_t Remaining = Size;
			while (Remaining != 0)
			{
				const auto Chunk = static_cast<std::streamsize>((std::min)(Remaining, static_cast<uintmax_t>(Buffer.size())));
				Stream.read(reinterpret_cast<char*>(Buffer.data()), Chunk);
				if (Stream.gcount() != Chunk) return std::nullopt;
				Builder.UpdateBytes("chunk", std::span<const std::byte>(Buffer.data(), static_cast<size_t>(Chunk)));
				Remaining -= static_cast<uintmax_t>(Chunk);
			}
			return Builder.Finish();
		};
		const std::filesystem::path Executable = PF_HOST_DXC_EXECUTABLE;
		AssetFingerprintBuilder Toolchain("PulseForge.DXC.Toolchain.v1");
		for (const std::filesystem::path& Name : { std::filesystem::path("dxc.exe"), std::filesystem::path("dxcompiler.dll"),
			std::filesystem::path("dxil.dll") })
		{
			const auto Path = Name == "dxc.exe" ? Executable : Executable.parent_path() / Name;
			const auto Fingerprint = FingerprintFile(Path);
			if (!Fingerprint) return {};
			Toolchain.UpdateText(Path.filename().string(), Fingerprint->Algorithm + ":" + Fingerprint->DigestHex);
		}
		const auto Fingerprint = Toolchain.Finish();
		return std::string("dxc-") + PF_HOST_DXC_VERSION + ";" + Fingerprint.Algorithm + ":" + Fingerprint.DigestHex +
			";target=vulkan1.3;abi=spirv";
#else
		return {};
#endif
	}

	namespace
	{
		MaterialGraphCompileError Failure(MaterialGraphCompileErrorCode Code, std::string Message)
		{
			return { Code, {}, std::move(Message) };
		}

		std::string FloatLiteral(float Value)
		{
			char Buffer[64]{};
			auto [End, Error] = std::to_chars(Buffer, Buffer + sizeof(Buffer), Value, std::chars_format::general, 9);
			if (Error != std::errc{}) return "0.0";
			std::string Result(Buffer, End);
			if (Result.find_first_of(".eE") == std::string::npos) Result += ".0";
			return Result;
		}

		std::wstring QuoteWindowsArgument(std::wstring_view Value)
		{
			std::wstring Result = L"\"";
			size_t Slashes = 0;
			for (wchar_t Character : Value)
			{
				if (Character == L'\\') { ++Slashes; continue; }
				if (Character == L'\"') Result.append(Slashes * 2 + 1, L'\\');
				else Result.append(Slashes, L'\\');
				Result += Character;
				Slashes = 0;
			}
			Result.append(Slashes * 2, L'\\');
			Result += L'\"';
			return Result;
		}

		struct GeneratedMaterialShader final
		{
			std::string Hlsl;
			MaterialGraphIR IR;
			std::vector<MaterialGraphCompiledArtifact::Parameter> Parameters;
		};

		std::expected<GeneratedMaterialShader, MaterialGraphCompileError> BuildSource(const MaterialGraphSource& Graph)
		{
			auto Lowered = LowerMaterialGraphToIR(Graph);
			if (!Lowered)
			{
				auto Error = Failure(MaterialGraphCompileErrorCode::InvalidGraph, "Material graph validation or lowering failed");
				Error.Diagnostics = Lowered.error().Diagnostics;
				return std::unexpected(std::move(Error));
			}
			GeneratedMaterialShader Generated;
			Generated.IR = std::move(*Lowered);
			std::map<std::string, float, std::less<>> Parameters;
			for (const auto& Expression : Generated.IR.Expressions)
				if (Expression.Op == MaterialGraphExpressionOp::FloatParameter)
				{
					Parameters.emplace(Expression.ParameterName, Expression.Value);
					Generated.Parameters.push_back({ Expression.SourceNode, Expression.ParameterName, Expression.Value });
				}
			if (!Parameters.empty())
			{
				Generated.Hlsl = "cbuffer MaterialGraphParameters : register(b0, space4) {\n";
				for (const auto& [Name, Value] : Parameters)
				{
					(void)Value;
					Generated.Hlsl += "  float g_" + Name + ";\n";
				}
				Generated.Hlsl += "};\n";
			}
		Generated.Hlsl += "float3 GraphMaterialBaseColor() {\n";
			for (size_t Index = 0; Index < Generated.IR.Expressions.size(); ++Index)
			{
				const auto& Expression = Generated.IR.Expressions[Index];
				Generated.Hlsl += Expression.Op == MaterialGraphExpressionOp::ColorConstant ? "  float3 e" : "  float e";
				Generated.Hlsl += std::to_string(Index) + " = ";
				switch (Expression.Op)
				{
				case MaterialGraphExpressionOp::FloatConstant:
					Generated.Hlsl += FloatLiteral(Expression.Value);
					break;
				case MaterialGraphExpressionOp::ColorConstant:
					Generated.Hlsl += "float3(" + FloatLiteral(Expression.Color[0]) + ", " +
						FloatLiteral(Expression.Color[1]) + ", " + FloatLiteral(Expression.Color[2]) + ")";
					break;
				case MaterialGraphExpressionOp::FloatParameter:
					Generated.Hlsl += "g_" + Expression.ParameterName;
					break;
				case MaterialGraphExpressionOp::Add:
					Generated.Hlsl += "e" + std::to_string(Expression.Left) + " + e" + std::to_string(Expression.Right);
					break;
				case MaterialGraphExpressionOp::Multiply:
					Generated.Hlsl += "e" + std::to_string(Expression.Left) + " * e" + std::to_string(Expression.Right);
					break;
				}
				Generated.Hlsl += ";\n";
			}
			const auto& Output = Generated.IR.Expressions[Generated.IR.OutputExpression];
			if (Output.Op == MaterialGraphExpressionOp::ColorConstant)
				Generated.Hlsl += "  return saturate(e" + std::to_string(Generated.IR.OutputExpression) + "); }\n";
			else
				Generated.Hlsl += "  return saturate(e" + std::to_string(Generated.IR.OutputExpression) + ").xxx; }\n";
			return Generated;
		}

		AssetContentFingerprint SemanticRecipe(
			const MaterialGraphSource& Graph, std::string_view Hlsl, std::string_view TemplateFingerprint, std::string_view Toolchain)
		{
			AssetFingerprintBuilder Builder("PulseForge.MaterialGraphRecipe.v2");
			Builder.UpdateText("semantic-hlsl", Hlsl);
			Builder.UpdateText("pbr-template", TemplateFingerprint);
			Builder.UpdateText("backend", "vulkan1.3/spirv");
			Builder.UpdateText("stage", "ps_6_0:PSMain");
		Builder.UpdateText("compiler-flags", "-spirv;-T;ps_6_0;-E;PSMain;-D;PULSEFORGE_MATERIAL_GRAPH=1;-fspv-target-env=vulkan1.3;-O3;-WX");
			Builder.UpdateText("graph-schema", std::to_string(Graph.Version));
			Builder.UpdateText("toolchain", Toolchain);
			return Builder.Finish();
		}

		uint32_t ReadSpirVWord(std::span<const std::byte> Bytes, size_t WordIndex)
		{
			uint32_t Word = 0;
			std::memcpy(&Word, Bytes.data() + WordIndex * sizeof(Word), sizeof(Word));
			return Word;
		}

		bool IsSpirV(std::span<const std::byte> Bytes, size_t MaximumBytes, bool RequiresGraphParameterBinding)
		{
			if (Bytes.size() < 20 || Bytes.size() > MaximumBytes || Bytes.size() % 4 != 0) return false;
			const size_t WordCount = Bytes.size() / sizeof(uint32_t);
			const uint32_t Version = ReadSpirVWord(Bytes, 1);
			const uint32_t IDBound = ReadSpirVWord(Bytes, 3);
			if (ReadSpirVWord(Bytes, 0) != 0x07230203 || (Version >> 16) != 1 || (Version & 0xff) > 6 ||
				IDBound == 0 || IDBound > 16 * 1024 * 1024 || ReadSpirVWord(Bytes, 4) != 0)
				return false;

			constexpr uint16_t OpMemoryModel = 14;
			constexpr uint16_t OpEntryPoint = 15;
			constexpr uint16_t OpVariable = 59;
			constexpr uint16_t OpDecorate = 71;
			constexpr uint32_t DecorationBinding = 33;
			constexpr uint32_t DecorationDescriptorSet = 34;
			constexpr uint32_t StorageClassUniform = 2;
			std::unordered_map<uint32_t, std::pair<std::optional<uint32_t>, std::optional<uint32_t>>> DecorationsByID;
			std::unordered_map<uint32_t, uint32_t> StorageClassByID;
			bool HasMemoryModel = false;
			bool HasFragmentEntryPoint = false;
			for (size_t Offset = 5; Offset < WordCount;)
			{
				const uint32_t Instruction = ReadSpirVWord(Bytes, Offset);
				const uint16_t InstructionWords = static_cast<uint16_t>(Instruction >> 16);
				const uint16_t Opcode = static_cast<uint16_t>(Instruction & 0xffff);
				if (InstructionWords == 0 || InstructionWords > WordCount - Offset) return false;
				if (Opcode == OpMemoryModel)
				{
					if (InstructionWords != 3 || HasMemoryModel) return false;
					HasMemoryModel = true;
				}
				else if (Opcode == OpEntryPoint)
				{
					if (InstructionWords < 4) return false;
					if (ReadSpirVWord(Bytes, Offset + 1) == 4) HasFragmentEntryPoint = true;
				}
				else if (Opcode == OpVariable)
				{
					if (InstructionWords < 4) return false;
					const uint32_t ResultID = ReadSpirVWord(Bytes, Offset + 2);
					const uint32_t StorageClass = ReadSpirVWord(Bytes, Offset + 3);
					if (ResultID == 0 || ResultID >= IDBound || !StorageClassByID.emplace(ResultID, StorageClass).second)
						return false;
				}
				else if (Opcode == OpDecorate)
				{
					if (InstructionWords < 3) return false;
					const uint32_t TargetID = ReadSpirVWord(Bytes, Offset + 1);
					const uint32_t Decoration = ReadSpirVWord(Bytes, Offset + 2);
					if (TargetID == 0 || TargetID >= IDBound) return false;
					if (Decoration == DecorationBinding || Decoration == DecorationDescriptorSet)
					{
						if (InstructionWords != 4) return false;
						auto& Pair = DecorationsByID[TargetID];
						auto& Value = Decoration == DecorationBinding ? Pair.first : Pair.second;
						if (Value) return false;
						Value = ReadSpirVWord(Bytes, Offset + 3);
					}
				}
				Offset += InstructionWords;
			}
			if (!HasMemoryModel || !HasFragmentEntryPoint) return false;
			if (!RequiresGraphParameterBinding) return true;
			for (const auto& [ID, Decorations] : DecorationsByID)
			{
				const auto Variable = StorageClassByID.find(ID);
				if (Variable != StorageClassByID.end() && Variable->second == StorageClassUniform &&
					Decorations.first == 0 && Decorations.second == 4)
					return true;
			}
			return false;
		}

		std::expected<std::vector<std::byte>, MaterialGraphCompileError> ReadBoundedFile(
			const std::filesystem::path& Path, size_t MaximumBytes, bool RequiresGraphParameterBinding)
		{
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error || Size == 0 || Size > MaximumBytes)
				return std::unexpected(Failure(MaterialGraphCompileErrorCode::OutputInvalid, "Compiler output is missing or exceeds the configured bound"));
			std::ifstream Stream(Path, std::ios::binary);
			if (!Stream) return std::unexpected(Failure(MaterialGraphCompileErrorCode::OutputInvalid, "Could not read compiler output"));
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			Stream.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Stream || !IsSpirV(Bytes, MaximumBytes, RequiresGraphParameterBinding))
				return std::unexpected(Failure(MaterialGraphCompileErrorCode::OutputInvalid,
					"Compiler output is not a valid fragment SPIR-V module or lacks the graph parameter binding"));
			return Bytes;
		}

		struct TempFiles final
		{
			std::filesystem::path Root;
			~TempFiles() { std::error_code Ignored; std::filesystem::remove_all(Root, Ignored); }
		};
	}

	std::expected<MaterialGraphCompiledArtifact, MaterialGraphCompileError> MaterialGraphCompiler::Compile(
		const MaterialGraphCompileRequest& Request)
	{
		if (Request.CancellationToken.stop_requested())
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::Cancelled, "Material compilation was cancelled before launch"));
		if (Request.IsGenerationCurrent && !Request.IsGenerationCurrent(Request.Generation))
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::StaleGeneration, "Material generation is stale before launch"));
		if (Request.Target != MaterialGraphCompileTarget::VulkanSpirV)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::UnsupportedTarget,
				"Material graph compilation currently supports only Vulkan SPIR-V"));
		if (Request.Timeout <= std::chrono::milliseconds::zero() || Request.Timeout > std::chrono::minutes(2) ||
			Request.MaximumOutputBytes < 20 || Request.MaximumOutputBytes > 64u * 1024u * 1024u || Request.ToolchainFingerprint.empty())
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "Compiler timeout, output bound, or toolchain fingerprint is invalid"));
		const auto Validation = ValidateMaterialGraphSource(Request.Source);
		if (!Validation.IsValid())
		{
			auto Error = Failure(MaterialGraphCompileErrorCode::InvalidGraph, "Material graph validation failed");
			Error.Diagnostics = Validation.Diagnostics;
			return std::unexpected(std::move(Error));
		}
		std::error_code PathError;
		std::filesystem::path ConfiguredExecutable = Request.CompilerExecutable;
#if defined(PF_HOST_DXC_EXECUTABLE)
		if (ConfiguredExecutable.empty()) ConfiguredExecutable = PF_HOST_DXC_EXECUTABLE;
#endif
		auto Generated = BuildSource(Request.Source);
		if (!Generated) return std::unexpected(Generated.error());
#if defined(PF_MATERIAL_GRAPH_PBR_TEMPLATE)
		std::error_code TemplateError;
		const std::filesystem::path TemplatePath = std::filesystem::weakly_canonical(PF_MATERIAL_GRAPH_PBR_TEMPLATE, TemplateError);
		if (TemplateError || !TemplatePath.is_absolute() || !std::filesystem::is_regular_file(TemplatePath, TemplateError))
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "PBR material shader template is unavailable"));
		const uintmax_t TemplateSize = std::filesystem::file_size(TemplatePath, TemplateError);
		if (TemplateError || TemplateSize == 0 || TemplateSize > 1024 * 1024)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "PBR material shader template is empty or exceeds its one MiB bound"));
		std::ifstream TemplateStream(TemplatePath, std::ios::binary);
		std::string TemplateBytes(static_cast<size_t>(TemplateSize), '\0');
		TemplateStream.read(TemplateBytes.data(), static_cast<std::streamsize>(TemplateBytes.size()));
		if (!TemplateStream || TemplateBytes.find("PULSEFORGE_MATERIAL_GRAPH") == std::string::npos ||
			TemplateBytes.find("GraphMaterialBaseColor()") == std::string::npos)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest,
				"PBR material shader template is unreadable or lacks the graph material hook"));
		AssetFingerprintBuilder TemplateFingerprintBuilder("PulseForge.MaterialGraphPbrTemplate.v1");
		TemplateFingerprintBuilder.UpdateBytes("template", std::as_bytes(std::span(TemplateBytes.data(), TemplateBytes.size())));
		const AssetContentFingerprint TemplateFingerprint = TemplateFingerprintBuilder.Finish();
		const AssetContentFingerprint Recipe = SemanticRecipe(Request.Source, Generated->Hlsl,
			TemplateFingerprint.Algorithm + ":" + TemplateFingerprint.DigestHex, Request.ToolchainFingerprint);
#else
		return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest,
			"Graph material compilation requires the configured PBR shader template"));
#endif
		const DerivedDataArtifactKey ArtifactKey{ "material-graph-spirv", Recipe, 1 };
		std::vector<UUID> SourceNodes;
		SourceNodes.reserve(Generated->IR.Expressions.size());
		for (const auto& Expression : Generated->IR.Expressions) SourceNodes.push_back(Expression.SourceNode);
		if (Request.ArtifactStore)
		{
			auto Cached = Request.ArtifactStore->Read(ArtifactKey);
			if (Cached.Status == DerivedDataArtifactReadStatus::Hit &&
				IsSpirV(Cached.Payload, Request.MaximumOutputBytes, !Generated->Parameters.empty()))
				return MaterialGraphCompiledArtifact{ std::move(Cached.Payload), Generated->Hlsl, Recipe.DigestHex, {},
					std::move(SourceNodes), std::move(Generated->Parameters), Request.Generation };
		}
		const auto Executable = std::filesystem::weakly_canonical(ConfiguredExecutable, PathError);
		if (PathError || !Executable.is_absolute() || !std::filesystem::is_regular_file(Executable, PathError))
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerUnavailable, "Configured DXC executable is unavailable"));
		TempFiles Temporary{ std::filesystem::temp_directory_path() / ("PulseForgeMaterialGraph-" + UUID().ToString()) };
		if (!std::filesystem::create_directory(Temporary.Root, PathError) || PathError)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "Could not create isolated compiler workspace"));
		const auto IncludePath = Temporary.Root / "MaterialGraph.generated.hlsli";
		const auto OutputPath = Temporary.Root / "MaterialGraph.spv";
		const auto LogPath = Temporary.Root / "Compiler.log";
		{
			std::ofstream Stream(IncludePath, std::ios::binary | std::ios::trunc);
			Stream.write(Generated->Hlsl.data(), static_cast<std::streamsize>(Generated->Hlsl.size()));
			if (!Stream) return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "Could not write generated material graph include"));
		}
		int ExitCode = -1;
		const auto Deadline = std::chrono::steady_clock::now() + Request.Timeout;
#if defined(_WIN32)
		SECURITY_ATTRIBUTES LogSecurity{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE Log = CreateFileW(LogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &LogSecurity, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY, nullptr);
		if (Log == INVALID_HANDLE_VALUE) return std::unexpected(Failure(MaterialGraphCompileErrorCode::InvalidRequest, "Could not create compiler diagnostics file"));
		STARTUPINFOW Startup{};
		Startup.cb = sizeof(Startup);
		Startup.dwFlags = STARTF_USESTDHANDLES;
		Startup.hStdOutput = Log;
		Startup.hStdError = Log;
		Startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		std::wstring CommandLine = QuoteWindowsArgument(Executable.wstring());
		for (const auto& Argument : { L"-spirv", L"-T", L"ps_6_0", L"-E", L"PSMain", L"-D", L"PULSEFORGE_MATERIAL_GRAPH=1",
			L"-I", Temporary.Root.c_str(), L"-fspv-target-env=vulkan1.3", L"-O3", L"-WX", L"-Fo" })
			CommandLine += L" " + QuoteWindowsArgument(Argument);
		CommandLine += L" " + QuoteWindowsArgument(OutputPath.wstring()) + L" " + QuoteWindowsArgument(TemplatePath.wstring());
		PROCESS_INFORMATION Process{};
		const BOOL Started = CreateProcessW(Executable.c_str(), CommandLine.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, Temporary.Root.c_str(), &Startup, &Process);
		CloseHandle(Log);
		if (!Started) return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerUnavailable, "Could not start configured DXC"));
		bool TimedOut = false;
		bool Cancelled = false;
		bool Stale = false;
		while (WaitForSingleObject(Process.hProcess, 20) == WAIT_TIMEOUT)
		{
			std::error_code SizeError;
			const auto LogSize = std::filesystem::file_size(LogPath, SizeError);
			const bool LogOverBudget = !SizeError && LogSize > 1024 * 1024;
			SizeError.clear();
			const auto OutputSize = std::filesystem::file_size(OutputPath, SizeError);
			const bool OutputOverBudget = !SizeError && OutputSize > Request.MaximumOutputBytes;
			if (LogOverBudget || OutputOverBudget ||
				Request.CancellationToken.stop_requested() || std::chrono::steady_clock::now() >= Deadline ||
				(Request.IsGenerationCurrent && !Request.IsGenerationCurrent(Request.Generation)))
			{
				Cancelled = Request.CancellationToken.stop_requested();
				Stale = !Cancelled && Request.IsGenerationCurrent && !Request.IsGenerationCurrent(Request.Generation);
				TimedOut = !Cancelled && !Stale && std::chrono::steady_clock::now() >= Deadline;
				TerminateProcess(Process.hProcess, 1);
				WaitForSingleObject(Process.hProcess, INFINITE);
				break;
			}
		}
		DWORD Exit = 1;
		GetExitCodeProcess(Process.hProcess, &Exit);
		ExitCode = static_cast<int>(Exit);
		CloseHandle(Process.hThread);
		CloseHandle(Process.hProcess);
		if (Cancelled) return std::unexpected(Failure(MaterialGraphCompileErrorCode::Cancelled, "DXC process was cancelled and terminated"));
		if (Stale) return std::unexpected(Failure(MaterialGraphCompileErrorCode::StaleGeneration, "DXC result belongs to a stale material generation"));
		if (TimedOut) return std::unexpected(Failure(MaterialGraphCompileErrorCode::TimedOut, "DXC exceeded its time budget and was terminated"));
#else
		posix_spawn_file_actions_t Actions;
		if (posix_spawn_file_actions_init(&Actions) != 0)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerUnavailable, "Could not initialize DXC process redirection"));
		struct ActionsGuard final { posix_spawn_file_actions_t* Actions; ~ActionsGuard() { posix_spawn_file_actions_destroy(Actions); } } ActionsScope{ &Actions };
		if (posix_spawn_file_actions_addopen(&Actions, STDOUT_FILENO, LogPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600) != 0 ||
			posix_spawn_file_actions_adddup2(&Actions, STDOUT_FILENO, STDERR_FILENO) != 0)
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerUnavailable, "Could not redirect DXC diagnostics"));
		std::vector<std::string> ArgumentStorage{
			Executable.string(), "-spirv", "-T", "ps_6_0", "-E", "PSMain", "-D", "PULSEFORGE_MATERIAL_GRAPH=1",
			"-I", Temporary.Root.string(), "-fspv-target-env=vulkan1.3", "-O3", "-WX", "-Fo", OutputPath.string(),
			TemplatePath.string()
		};
		std::vector<char*> Arguments;
		Arguments.reserve(ArgumentStorage.size() + 1);
		for (auto& Argument : ArgumentStorage) Arguments.push_back(Argument.data());
		Arguments.push_back(nullptr);
		pid_t Child = -1;
		const int SpawnError = posix_spawn(&Child, Executable.c_str(), &Actions, nullptr, Arguments.data(), environ);
		if (SpawnError != 0) return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerUnavailable, "Could not start configured DXC"));
		bool TimedOut = false;
		bool Cancelled = false;
		bool Stale = false;
		bool OverBudget = false;
		int Status = 0;
		for (;;)
		{
			const pid_t Waited = waitpid(Child, &Status, WNOHANG);
			if (Waited == Child) break;
			if (Waited < 0 && errno != EINTR)
				return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerFailed, "Could not observe DXC process completion"));
			std::error_code SizeError;
			const uintmax_t LogSize = std::filesystem::file_size(LogPath, SizeError);
			if (!SizeError && LogSize > 1024 * 1024) OverBudget = true;
			SizeError.clear();
			const uintmax_t OutputSize = std::filesystem::file_size(OutputPath, SizeError);
			if (!SizeError && OutputSize > Request.MaximumOutputBytes) OverBudget = true;
			Cancelled = Request.CancellationToken.stop_requested();
			Stale = !Cancelled && Request.IsGenerationCurrent && !Request.IsGenerationCurrent(Request.Generation);
			TimedOut = !Cancelled && !Stale && std::chrono::steady_clock::now() >= Deadline;
			if (Cancelled || Stale || TimedOut || OverBudget)
			{
				kill(Child, SIGKILL);
				while (waitpid(Child, &Status, 0) < 0 && errno == EINTR) {}
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (Cancelled) return std::unexpected(Failure(MaterialGraphCompileErrorCode::Cancelled, "DXC process was cancelled and terminated"));
		if (Stale) return std::unexpected(Failure(MaterialGraphCompileErrorCode::StaleGeneration, "DXC result belongs to a stale material generation"));
		if (TimedOut) return std::unexpected(Failure(MaterialGraphCompileErrorCode::TimedOut, "DXC exceeded its time budget and was terminated"));
		if (OverBudget) return std::unexpected(Failure(MaterialGraphCompileErrorCode::OutputInvalid, "DXC exceeded a diagnostics or SPIR-V output bound and was terminated"));
		if (!WIFEXITED(Status)) return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerFailed, "DXC terminated abnormally"));
		ExitCode = WEXITSTATUS(Status);
#endif
		std::ifstream LogStream(LogPath, std::ios::binary);
		std::string Diagnostics((std::istreambuf_iterator<char>(LogStream)), std::istreambuf_iterator<char>());
		if (Diagnostics.size() > 1024 * 1024) Diagnostics.resize(1024 * 1024);
		if (ExitCode != 0) return std::unexpected(Failure(MaterialGraphCompileErrorCode::CompilerFailed, "DXC failed: " + Diagnostics));
		if (Request.CancellationToken.stop_requested())
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::Cancelled, "DXC completed after cancellation"));
		if (Request.IsGenerationCurrent && !Request.IsGenerationCurrent(Request.Generation))
			return std::unexpected(Failure(MaterialGraphCompileErrorCode::StaleGeneration, "DXC completed for a stale material generation"));
		auto SpirV = ReadBoundedFile(OutputPath, Request.MaximumOutputBytes, !Generated->Parameters.empty());
		if (!SpirV) return std::unexpected(SpirV.error());
		if (Request.ArtifactStore) (void)Request.ArtifactStore->Write(ArtifactKey, *SpirV);
		return MaterialGraphCompiledArtifact{ std::move(*SpirV), Generated->Hlsl, Recipe.DigestHex, std::move(Diagnostics),
			std::move(SourceNodes), std::move(Generated->Parameters), Request.Generation };
	}
}
