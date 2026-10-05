# PulseForge

PulseForge is a C++23 game engine under active development. Windows is the supported platform today, and CMake is the canonical build system. The sample uses the Vulkan/NVRHI renderer by default; OpenGL remains available as a transitional fallback.

## Requirements

- Windows 10 or later
- Visual Studio 2022 with the Desktop development with C++ workload and a Windows SDK
- CMake 3.24 or newer and Git with submodule support
- A Vulkan 1.3-capable driver/runtime to run the default sample
- Network access on the first configure to fetch the pinned NVRHI, Vulkan-Headers, and DXC dependencies

The Vulkan SDK is not required to build or run PulseForge. Khronos validation layers are optional and used for Debug validation when installed. PulseForge currently has no Linux or macOS build target.

The project has been built with Visual Studio Community 2022 17.14.37, MSVC 14.44, Windows SDK 10.0.26100.0, and CMake 4.0.0.

## Getting Started

Initialize the repository's submodules:

```powershell
git submodule update --init --recursive
```

Configure from the repository root using an x64 Visual Studio developer environment:

```bat
cmake -S . -B Build/CMake/VS2022-x64 -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
```

CMake fetches the pinned NVRHI/Vulkan-Headers sources and Microsoft DXC. DXC compiles the sample's HLSL shaders to SPIR-V at build time; the generated shader files are copied beside the sample executable. DXC is a build-time tool and is not needed to run a built game.

## Building

Build and test Debug, Release, or Dist with the matching configuration:

```bat
cmake --build Build/CMake/VS2022-x64 --config Debug --parallel 1
ctest --test-dir Build/CMake/VS2022-x64 -C Debug --output-on-failure
cmake --build Build/CMake/VS2022-x64 --config Release --parallel 1
ctest --test-dir Build/CMake/VS2022-x64 -C Release --output-on-failure
cmake --build Build/CMake/VS2022-x64 --config Dist --parallel 1
ctest --test-dir Build/CMake/VS2022-x64 -C Dist --output-on-failure
```

`Dist` is an optimized build configuration, not a game packaging or export pipeline.

## Running

`PulseForgeGame` uses Vulkan by default. Run it from its output directory so it can find the generated `Shaders` folder. To build the sample with the OpenGL fallback, configure with:

```bat
cmake -S . -B Build/CMake/VS2022-x64-OpenGL -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DPULSEFORGE_SAMPLE_RENDERER=OpenGL
```

The OpenGL fallback supports the current bootstrap and ImGui path; the newer shader, binding, and geometry APIs are Vulkan-only for now. Vulkan presentation currently uses FIFO (VSync); runtime VSync controls are not exposed.

## Tests

`PulseForgeTests` is the first-party test target and runs through CTest. It covers core lifecycle, events and input, Vulkan selection helpers, and deterministic validation of renderer descriptions and draw/binding arguments. GPU and window behavior is exercised by running the sample.
