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
IncludeDir["entt"] = "PulseForge/Vendor/entt/src"
IncludeDir["enet"] = "PulseForge/Vendor/enet/include"
IncludeDir["JoltPhysics"] = "PulseForge/Vendor/JoltPhysics"
IncludeDir["tracy"] = "PulseForge/Vendor/tracy/public"

group "Dependencies"
include "PulseForge/Vendor/Glfw"
include "PulseForge/Vendor/Glad"
include "PulseForge/Vendor/imgui"
include "PulseForge/Vendor/freetype"
include "PulseForge/Vendor/nativefiledialog"
include "PulseForge/Vendor/enet"
include "PulseForge/Vendor/JoltPhysics"
include "PulseForge/Vendor/tracy"
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
	"MA_NO_WIN32_LEAN_AND_MEAN",
	"TRACY_ENABLE",
    "TRACY_NO_ETW"
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
	"%{IncludeDir.nfd}",
	"%{IncludeDir.entt}",
	"%{IncludeDir.enet}",
	"%{IncludeDir.JoltPhysics}",
	"%{IncludeDir.tracy}"
}

links
{
	"Glfw",
	"Glad",
	"imgui",
	"freetype",
	"NativeFileDialog",
	"enet",
	"JoltPhysics",
	"tracy",
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
	"%{prj.name}/Source",
	"%{prj.name}/Source/Public",
	"%{prj.name}/Source/Private"
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
