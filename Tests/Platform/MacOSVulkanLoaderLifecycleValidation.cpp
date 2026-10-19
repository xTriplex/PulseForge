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

	try
	{
		PulseForge::GLFWWindow InvalidWindow(PulseForge::WindowProps(
			"PulseForge Vulkan loader failed-window cleanup", 0, 0, PulseForge::EWindowStyle::Default,
			PulseForge::EWindowClientAPI::Vulkan));
		std::cerr << "GLFW unexpectedly created a zero-sized validation window" << '\n';
		return 1;
	}
	catch (const std::exception&)
	{
		if (PulseForge::GLFWWindow::GetVulkanLoaderProcAddress())
		{
			std::cerr << "Failed window construction left the Vulkan loader retained" << '\n';
			return 1;
		}
	}

	if (!CreateAndCloseVulkanWindow("PulseForge Vulkan loader repeated initialization")) return 1;
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
	std::cout << "MACOS_VULKAN_LOADER_LIFECYCLE=PASS; startup, failed-window cleanup, termination and reinitialization\n";
	return 0;
}
