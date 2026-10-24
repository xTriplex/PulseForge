#pragma once

#include "Core/Core.h"
#include "Core/Log.h"
#include "Core/Application.h"

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(PF_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#if defined(PF_PLATFORM_WINDOWS) || defined(PF_PLATFORM_LINUX) || defined(PF_PLATFORM_MACOS)

namespace PulseForge
{
	std::unique_ptr<Application> CreateApplication(int ArgumentCount, char** Arguments);

	inline int RunClient(int ArgumentCount, char** Arguments)
	{
		PulseForge::Log::Init();
		PF_CORE_INFO("Engine Logger Initialized!");

		try
		{
			auto App = PulseForge::CreateApplication(ArgumentCount, Arguments);
			if (!App) throw std::runtime_error("Client application factory returned null");

			PF_INFO("Client Application Created!");
			App->Run();
			return EXIT_SUCCESS;
		}
		catch (const std::exception& Exception)
		{
			PF_CORE_CRITICAL("Application startup/runtime failure: {0}", Exception.what());
			return EXIT_FAILURE;
		}
	}
} // namespace PulseForge

#endif

#if defined(PF_PLATFORM_WINDOWS)
int wmain(int ArgumentCount, wchar_t** WideArguments)
{
	std::vector<std::string> Utf8Arguments;
	std::vector<char*> Arguments;
	Utf8Arguments.reserve(static_cast<size_t>(ArgumentCount));
	Arguments.reserve(static_cast<size_t>(ArgumentCount));
	for (int Index = 0; Index < ArgumentCount; ++Index)
	{
		const int ByteCount = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArguments[Index], -1, nullptr, 0, nullptr, nullptr);
		if (ByteCount <= 0) return EXIT_FAILURE;
		std::string Encoded(static_cast<size_t>(ByteCount), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, WideArguments[Index], -1, Encoded.data(), ByteCount, nullptr, nullptr) <= 0)
			return EXIT_FAILURE;
		Encoded.pop_back();
		Utf8Arguments.push_back(std::move(Encoded));
	}
	for (std::string& Argument : Utf8Arguments)
		Arguments.push_back(Argument.data());
	return PulseForge::RunClient(ArgumentCount, Arguments.data());
}
#else
int main(int ArgumentCount, char** Arguments)
{
	return PulseForge::RunClient(ArgumentCount, Arguments);
}
#endif
