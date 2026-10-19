#include "Core/Log.h"
#include "Window/Platform/GLFW/GLFWWindow.h"

#include <GLFW/glfw3.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	bool CreateAndCloseVulkanWindow(const char* Title)
	{
		try
		{
			PulseForge::GLFWWindow Window(PulseForge::WindowProps(
				Title, 320, 240, PulseForge::EWindowStyle::Default, PulseForge::EWindowClientAPI::Vulkan));
			if (!PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
				throw std::runtime_error("GLFW lost its Vulkan loader while the window runtime was active");
			if (glfwVulkanSupported() != GLFW_TRUE)
				throw std::runtime_error("GLFW could not query Vulkan through the injected loader");
		}
		catch (const std::exception& Error)
		{
			std::cerr << "Vulkan loader lifecycle window failed: " << Error.what() << '\n';
			return false;
		}
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "GLFW retained a Vulkan loader after its last window terminated" << '\n';
			return false;
		}
		return true;
	}
}

int main()
{
	PulseForge::Log::Init();
	const char* ExistingSdkValue = std::getenv("VULKAN_SDK");
	const std::string ExistingSdk = ExistingSdkValue ? ExistingSdkValue : "";
	const bool HadSdkValue = ExistingSdkValue != nullptr;
	if (setenv("VULKAN_SDK", "/path/that/does/not/exist", 1) != 0)
	{
		std::cerr << "Could not set the isolated loader-failure test environment" << '\n';
		return 1;
	}
	try
	{
		PulseForge::GLFWWindow InvalidLoaderWindow(PulseForge::WindowProps(
			"PulseForge Vulkan loader acquisition failure", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::Vulkan));
		std::cerr << "GLFW unexpectedly accepted a Vulkan SDK path without a loader" << '\n';
		return 1;
	}
	catch (const std::exception&)
	{
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "Failed loader acquisition left a function pointer installed" << '\n';
			return 1;
		}
	}
	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");

	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan loader startup")) return 1;

	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan loader repeated initialization")) return 1;
	{
		PulseForge::GLFWWindow OpenGLWindow(PulseForge::WindowProps(
			"PulseForge OpenGL runtime", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::OpenGL));
		try
		{
			PulseForge::GLFWWindow MismatchedVulkanWindow(PulseForge::WindowProps(
				"PulseForge Vulkan cannot join OpenGL runtime", 320, 240, PulseForge::EWindowStyle::Default,
				PulseForge::EWindowClientAPI::Vulkan));
			std::cerr << "Vulkan unexpectedly joined a GLFW runtime initialized without its loader" << '\n';
			return 1;
		}
		catch (const std::exception&)
		{
			if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
			{
				std::cerr << "Rejected Vulkan window left a loader installed into the OpenGL runtime" << '\n';
				return 1;
			}
		}
	}
	if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
	{
		std::cerr << "OpenGL runtime teardown retained a Vulkan loader" << '\n';
		return 1;
	}
	if (setenv("VULKAN_SDK", "/path/that/does/not/exist", 1) != 0) return 1;
	try
	{
		PulseForge::GLFWWindow OpenGLWindow(PulseForge::WindowProps(
			"PulseForge OpenGL does not require a Vulkan SDK", 320, 240, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::OpenGL));
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
			throw std::runtime_error("OpenGL startup unexpectedly acquired a Vulkan loader");
	}
	catch (const std::exception& Error)
	{
		std::cerr << "OpenGL startup unexpectedly required Vulkan: " << Error.what() << '\n';
		return 1;
	}
	if (HadSdkValue)
		setenv("VULKAN_SDK", ExistingSdk.c_str(), 1);
	else
		unsetenv("VULKAN_SDK");
	std::cout << "MACOS_VULKAN_LOADER_LIFECYCLE=PASS; acquisition failure, startup, API mismatch, termination and reinitialization\n";
	return 0;
}
