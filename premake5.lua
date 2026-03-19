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
IncludeDir["Glfw"]  = "PulseForge/Vendor/Glfw/include"
IncludeDir["Glad"]  = "PulseForge/Vendor/Glad/include"
IncludeDir["imgui"] = "PulseForge/Vendor/imgui"
IncludeDir["glm"]   = "PulseForge/Vendor/glm"

group "Dependencies"
include "PulseForge/Vendor/Glfw"
include "PulseForge/Vendor/Glad"
include "PulseForge/Vendor/imgui"
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
	"IMGUI_IMPL_OPENGL_LOADER_GLAD"
}

includedirs
{
	"%{prj.name}/Source",
	"%{prj.name}/Vendor/spdlog/include",
	"%{IncludeDir.Glfw}",
	"%{IncludeDir.Glad}",
	"%{IncludeDir.imgui}",
	"%{IncludeDir.glm}"
}

links
{
	"Glfw",
	"Glad",
	"imgui",
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
	"%{IncludeDir.glm}"
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

