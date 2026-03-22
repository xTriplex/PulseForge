workspace "PulseForge"
architecture "x64"
startproject "PulseForgeGame"

configurations
{
	"Debug",
	"Release",
	"Dist"
}

outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

-- Include directories
IncludeDir = {}
IncludeDir["spdlog"] = "PulseForge/Vendor/spdlog/include"
IncludeDir["Glfw"]  = "PulseForge/Vendor/Glfw/include"
IncludeDir["Glad"]  = "PulseForge/Vendor/Glad/include"
IncludeDir["imgui"] = "PulseForge/Vendor/imgui"
IncludeDir["glm"]   = "PulseForge/Vendor/glm"
IncludeDir["miniaudio"] = "PulseForge/Vendor/miniaudio"
IncludeDir["stb"] = "PulseForge/Vendor/stb"
IncludeDir["freetype"] = "PulseForge/Vendor/freetype/include"
IncludeDir["json"] = "PulseForge/Vendor/json/single_include"
IncludeDir["nfd"] = "PulseForge/Vendor/nativefiledialog/src/include"

group "Dependencies"
include "PulseForge/Vendor/Glfw"
include "PulseForge/Vendor/Glad"
include "PulseForge/Vendor/imgui"
include "PulseForge/Vendor/freetype"
include "PulseForge/Vendor/nativefiledialog"
group ""

-- =========================
-- Engine
-- =========================
project "PulseForge"
location "PulseForge"
kind "StaticLib"
language "C++"
cppdialect "C++23"
staticruntime "on"

targetdir ("Bin/" .. outputdir .. "/%{prj.name}")
objdir ("Int/" .. outputdir .. "/%{prj.name}")

pchheader "pfpch.h"
pchsource "%{prj.name}/Source/pfpch.cpp"

files
{
	"%{prj.name}/Source/**.h",
	"%{prj.name}/Source/**.cpp"
}

defines
{
	"_CRT_SECURE_NO_WARNINGS",
	"IMGUI_IMPL_OPENGL_LOADER_GLAD",
	"MA_NO_WIN32_LEAN_AND_MEAN"
}

includedirs
{
	"%{prj.name}/Source/Public",
	"%{prj.name}/Source/Private",
	"%{prj.name}/Source",
	"%{IncludeDir.spdlog}",
	"%{IncludeDir.Glfw}",
	"%{IncludeDir.Glad}",
	"%{IncludeDir.imgui}",
	"%{IncludeDir.glm}",
	"%{IncludeDir.miniaudio}",
	"%{IncludeDir.stb}",
	"%{IncludeDir.freetype}",
	"%{IncludeDir.json}",
	"%{IncludeDir.nfd}"
}

links
{
	"Glfw",
	"Glad",
	"imgui",
	"freetype",
	"NativeFileDialog",
	"opengl32.lib"
}

flags
{
	"MultiProcessorCompile"
}

filter "system:windows"
	systemversion "latest"

	defines
	{
		"PF_PLATFORM_WINDOWS",
		"GLFW_INCLUDE_NONE"
	}

filter "configurations:Debug"
	defines "PF_DEBUG"
	runtime "Debug"
	symbols "on"

filter "configurations:Release"
	defines "PF_RELEASE"
	runtime "Release"
	optimize "on"

filter "configurations:Dist"
	defines "PF_DIST"
	runtime "Release"
	optimize "on"

filter { "system:windows", "configurations:*" }
	buildoptions "/utf-8"

filter {}

-- =========================
-- Game
-- =========================
project "PulseForgeGame"
location "PulseForgeGame"
kind "ConsoleApp"
language "C++"
cppdialect "C++23"
staticruntime "on"

targetdir ("Bin/" .. outputdir .. "/%{prj.name}")
objdir ("Int/" .. outputdir .. "/%{prj.name}")

files
{
	"%{prj.name}/Source/**.h",
	"%{prj.name}/Source/**.cpp"
}

includedirs
{
	"PulseForge/Source",
	"PulseForge/Source/Public",
	"PulseForge/Source/Private",
	"%{prj.name}/Source"
}

links
{
	"PulseForge"
}

flags
{
	"MultiProcessorCompile"
}

filter "system:windows"
	systemversion "latest"

	defines
	{
		"PF_PLATFORM_WINDOWS"
	}

filter "configurations:Debug"
	defines "PF_DEBUG"
	runtime "Debug"
	symbols "on"

filter "configurations:Release"
	defines "PF_RELEASE"
	runtime "Release"
	optimize "on"

filter "configurations:Dist"
	defines "PF_DIST"
	runtime "Release"
	optimize "on"

filter { "system:windows", "configurations:*" }
	buildoptions "/utf-8"

filter {}
