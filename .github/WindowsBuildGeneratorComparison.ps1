$ErrorActionPreference = 'Stop'

$Workspace = $env:GITHUB_WORKSPACE
$RunnerTemp = $env:RUNNER_TEMP
$DependencyRoot = Join-Path $Workspace 'Build/CI/Dependencies'
$TargetSet = @(
	'PulseForgeTests', 'PulseForgeEditor', 'PulseForgeGame', 'PulseForgeCook',
	'PulseForgeExport', 'PulseForgeCookedGame', 'PulseForgeCookedPlayerTemplate'
)
$Results = [System.Collections.Generic.List[object]]::new()

function ConvertTo-CMakePath
{
	param([Parameter(Mandatory)][string]$Path)
	return $Path.Replace('\', '/')
}

function Import-MsvcEnvironment
{
	$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
	$VsInstallation = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
	if ($LASTEXITCODE -ne 0 -or -not $VsInstallation) { throw 'Could not locate the installed MSVC x64 toolchain.' }
	$VsDevCmd = Join-Path $VsInstallation 'Common7/Tools/VsDevCmd.bat'
	$EnvironmentScript = Join-Path $RunnerTemp 'pulseforge-msvc-environment.cmd'
	$ScriptContents = "@call `"$VsDevCmd`" -no_logo -arch=amd64 -host_arch=amd64 >nul`r`n@if errorlevel 1 exit /b 1`r`n@set`r`n"
	[System.IO.File]::WriteAllText($EnvironmentScript, $ScriptContents, [System.Text.Encoding]::ASCII)
	$EnvironmentLines = & $env:ComSpec /d /c $EnvironmentScript
	if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd failed while preparing the MSVC environment for Ninja.' }
	foreach ($Line in $EnvironmentLines)
	{
		$Equals = $Line.IndexOf('=')
		if ($Equals -gt 0)
		{
			[System.Environment]::SetEnvironmentVariable($Line.Substring(0, $Equals), $Line.Substring($Equals + 1), 'Process')
		}
	}
	Remove-Item -LiteralPath $EnvironmentScript -Force
}

function Invoke-ObservedProcess
{
	param(
		[Parameter(Mandatory)][string]$Name,
		[Parameter(Mandatory)][string]$Executable,
		[Parameter(Mandatory)][string[]]$Arguments
	)

	$SafeName = $Name -replace '[^A-Za-z0-9_-]', '-'
	$Stdout = Join-Path $RunnerTemp "$SafeName.stdout.log"
	$Stderr = Join-Path $RunnerTemp "$SafeName.stderr.log"
	Remove-Item -LiteralPath $Stdout, $Stderr -Force -ErrorAction SilentlyContinue
	$Watch = [System.Diagnostics.Stopwatch]::StartNew()
	$Process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $Workspace `
		-RedirectStandardOutput $Stdout -RedirectStandardError $Stderr -PassThru -NoNewWindow
	$MaximumCompilerProcesses = 0
	$MaximumCompilerWorkingSet = 0L
	$MaximumMsbuildProcesses = 0
	$NextProgress = 15
	while (-not $Process.WaitForExit(1000))
	{
		$Compilers = @(Get-Process -Name cl -ErrorAction SilentlyContinue)
		$Msbuild = @(Get-Process -Name MSBuild -ErrorAction SilentlyContinue)
		$MaximumCompilerProcesses = [Math]::Max($MaximumCompilerProcesses, $Compilers.Count)
		$MaximumMsbuildProcesses = [Math]::Max($MaximumMsbuildProcesses, $Msbuild.Count)
		$CompilerWorkingSet = [long](($Compilers | Measure-Object -Property WorkingSet64 -Sum).Sum)
		$MaximumCompilerWorkingSet = [Math]::Max($MaximumCompilerWorkingSet, $CompilerWorkingSet)
		if ($Watch.Elapsed.TotalSeconds -ge $NextProgress)
		{
			Write-Output "BUILD_PROFILE_PROGRESS; phase=$Name; elapsed_seconds=$([Math]::Round($Watch.Elapsed.TotalSeconds, 1)); cl_processes=$($Compilers.Count); msbuild_processes=$($Msbuild.Count)"
			$NextProgress += 15
		}
	}
	$Process.WaitForExit()
	$Watch.Stop()
	$StdoutLines = if (Test-Path -LiteralPath $Stdout) { [System.IO.File]::ReadAllLines($Stdout) } else { @() }
	$StderrLines = if (Test-Path -LiteralPath $Stderr) { [System.IO.File]::ReadAllLines($Stderr) } else { @() }
	$AllLines = @($StdoutLines) + @($StderrLines)
	$CompilerCommandCount = @($AllLines | Where-Object { $_ -match '(?i)\bcl\.exe\b' }).Count
	Write-Output "BUILD_PROFILE_RESULT; phase=$Name; exit_code=$($Process.ExitCode); elapsed_seconds=$([Math]::Round($Watch.Elapsed.TotalSeconds, 2)); compiler_command_lines=$CompilerCommandCount; max_cl_processes=$MaximumCompilerProcesses; max_msbuild_processes=$MaximumMsbuildProcesses; max_cl_working_set_mib=$([Math]::Round($MaximumCompilerWorkingSet / 1MB, 1)); stdout_log=$Stdout; stderr_log=$Stderr"
	if ($Process.ExitCode -ne 0)
	{
		Write-Output "---- $Name stdout tail ----"
		$StdoutLines | Select-Object -Last 120
		Write-Output "---- $Name stderr tail ----"
		$StderrLines | Select-Object -Last 120
		throw "$Name failed with exit code $($Process.ExitCode)."
	}
	$Results.Add([pscustomobject]@{
		Phase = $Name
		Seconds = [Math]::Round($Watch.Elapsed.TotalSeconds, 2)
		CompilerCommandLines = $CompilerCommandCount
		MaxClProcesses = $MaximumCompilerProcesses
		MaxMsbuildProcesses = $MaximumMsbuildProcesses
		MaxClWorkingSetMiB = [Math]::Round($MaximumCompilerWorkingSet / 1MB, 1)
		ExitCode = $Process.ExitCode
	})
}

Write-Output "BUILD_PROFILE_RUNNER; image_os=$env:ImageOS; image_version=$env:ImageVersion; workspace=$Workspace"
$Processor = Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1
$Computer = Get-CimInstance -ClassName Win32_ComputerSystem
$OperatingSystem = Get-CimInstance -ClassName Win32_OperatingSystem
Write-Output "BUILD_PROFILE_HOST; logical_processors=$env:NUMBER_OF_PROCESSORS; cim_logical_processors=$($Processor.NumberOfLogicalProcessors); physical_memory_gib=$([Math]::Round($Computer.TotalPhysicalMemory / 1GB, 1)); available_memory_mib=$([Math]::Round($OperatingSystem.FreePhysicalMemory / 1024, 0)); requested_parallelism=cmake --parallel (native default); compiled_cache=disabled"
cmake --version
ctest --version
ninja --version
Import-MsvcEnvironment
where.exe ninja
where.exe cl
where.exe msbuild
Write-Output "BUILD_PROFILE_MSVC; tools_version=$env:VCToolsVersion; windows_sdk=$env:WindowsSdkDir; include_entries=$(($env:INCLUDE -split ';').Count)"
& cl.exe /Bv 2>&1 | Select-Object -First 12
if ($LASTEXITCODE -ne 0) { throw 'MSVC compiler identity query failed.' }

$CommonConfigureOptions = @(
	'-DBUILD_TESTING=ON',
	'-DPULSEFORGE_BUILD_EDITOR=ON',
	'-DPULSEFORGE_BUILD_GPU_VALIDATION_TESTS=OFF',
	'-DFETCHCONTENT_FULLY_DISCONNECTED=OFF'
)
$BuildRoot = Join-Path $Workspace 'Build/CI/windows-build-profile'
$VsBuild = Join-Path $BuildRoot 'vs'
$NinjaBuild = Join-Path $BuildRoot 'ninja-multi'
$VsConfigureOptions = $CommonConfigureOptions + "-DFETCHCONTENT_BASE_DIR=$(ConvertTo-CMakePath $DependencyRoot)"
$NinjaDependencyRoot = Join-Path $BuildRoot 'dependencies-ninja'
$NinjaConfigureOptions = $CommonConfigureOptions + "-DFETCHCONTENT_BASE_DIR=$(ConvertTo-CMakePath $NinjaDependencyRoot)"
if (Test-Path -LiteralPath (Join-Path $DependencyRoot 'pulseforgenvrhi-src/CMakeLists.txt'))
{
	$NvrhiSource = ConvertTo-CMakePath (Join-Path $DependencyRoot 'pulseforgenvrhi-src')
	$NinjaConfigureOptions += "-DFETCHCONTENT_SOURCE_DIR_PULSEFORGENVRHI=$NvrhiSource"
}
if (Test-Path -LiteralPath (Join-Path $DependencyRoot 'pulseforgelua-src/src/lua.h'))
{
	$LuaSource = ConvertTo-CMakePath (Join-Path $DependencyRoot 'pulseforgelua-src')
	$NinjaConfigureOptions += "-DFETCHCONTENT_SOURCE_DIR_PULSEFORGELUA=$LuaSource"
}
if (Test-Path -LiteralPath (Join-Path $DependencyRoot 'vulkan_headers-src/include/vulkan/vulkan.h'))
{
	$VulkanHeadersSource = ConvertTo-CMakePath (Join-Path $DependencyRoot 'vulkan_headers-src')
	$NinjaConfigureOptions += "-DNVRHI_VULKAN_HEADERS_FETCH_DIR=$VulkanHeadersSource"
}
Write-Output "BUILD_PROFILE_DEPENDENCY_SOURCES; nvrhi=$(Test-Path (Join-Path $DependencyRoot 'pulseforgenvrhi-src/CMakeLists.txt')); lua=$(Test-Path (Join-Path $DependencyRoot 'pulseforgelua-src/src/lua.h')); vulkan_headers=$(Test-Path (Join-Path $DependencyRoot 'vulkan_headers-src/include/vulkan/vulkan.h')); vs_fetchcontent_base=$DependencyRoot; ninja_fetchcontent_base=$NinjaDependencyRoot"
Invoke-ObservedProcess 'vs-configure' 'cmake' (@('-S', (ConvertTo-CMakePath $Workspace), '-B', (ConvertTo-CMakePath $VsBuild), '-G', '"Visual Studio 17 2022"', '-A', 'x64') + $VsConfigureOptions)
Invoke-ObservedProcess 'ninja-configure' 'cmake' (@('-S', (ConvertTo-CMakePath $Workspace), '-B', (ConvertTo-CMakePath $NinjaBuild), '-G', '"Ninja Multi-Config"') + $NinjaConfigureOptions)

function Get-BuildArguments
{
	param([string]$BuildDirectory, [bool]$UseMsbuild)
	$Arguments = @('--build', $BuildDirectory, '--config', 'Debug', '--parallel', '--verbose', '--target') + $TargetSet + @('--')
	if ($UseMsbuild) { $Arguments += '/clp:PerformanceSummary;Summary' }
	else { $Arguments += @('-d', 'stats') }
	return $Arguments
}

$VsBuildArguments = Get-BuildArguments $VsBuild $true
$NinjaBuildArguments = Get-BuildArguments $NinjaBuild $false
Invoke-ObservedProcess 'vs-clean-debug' 'cmake' $VsBuildArguments
Invoke-ObservedProcess 'vs-noop-debug' 'cmake' $VsBuildArguments

$Probe = Join-Path $Workspace 'PulseForge/Source/Private/Assets/AssetExportService.cpp'
$ProbeBytes = [System.IO.File]::ReadAllBytes($Probe)
try
{
	$Suffix = [System.Text.Encoding]::ASCII.GetBytes("`r`n// Temporary affected-source build profile probe.`r`n")
	[System.IO.File]::WriteAllBytes($Probe, [byte[]]($ProbeBytes + $Suffix))
	Invoke-ObservedProcess 'vs-affected-source-debug' 'cmake' $VsBuildArguments
}
finally
{
	[System.IO.File]::WriteAllBytes($Probe, $ProbeBytes)
}

Invoke-ObservedProcess 'ninja-clean-debug' 'cmake' $NinjaBuildArguments
Invoke-ObservedProcess 'ninja-noop-debug' 'cmake' $NinjaBuildArguments
$ProbeBytes = [System.IO.File]::ReadAllBytes($Probe)
try
{
	$Suffix = [System.Text.Encoding]::ASCII.GetBytes("`r`n// Temporary affected-source build profile probe.`r`n")
	[System.IO.File]::WriteAllBytes($Probe, [byte[]]($ProbeBytes + $Suffix))
	Invoke-ObservedProcess 'ninja-affected-source-debug' 'cmake' $NinjaBuildArguments
}
finally
{
	[System.IO.File]::WriteAllBytes($Probe, $ProbeBytes)
}

$VsClean = ($Results | Where-Object Phase -eq 'vs-clean-debug').Seconds
$NinjaClean = ($Results | Where-Object Phase -eq 'ninja-clean-debug').Seconds
if ($NinjaClean -lt ($VsClean * 0.85))
{
	Write-Output "BUILD_PROFILE_REPEAT; Ninja clean Debug is at least 15% faster; repeating clean builds in reverse order to reduce ordering bias."
	$VsRepeat = Join-Path $BuildRoot 'vs-repeat'
	$NinjaRepeat = Join-Path $BuildRoot 'ninja-multi-repeat'
	Invoke-ObservedProcess 'ninja-repeat-configure' 'cmake' (@('-S', (ConvertTo-CMakePath $Workspace), '-B', (ConvertTo-CMakePath $NinjaRepeat), '-G', '"Ninja Multi-Config"') + $NinjaConfigureOptions)
	Invoke-ObservedProcess 'vs-repeat-configure' 'cmake' (@('-S', (ConvertTo-CMakePath $Workspace), '-B', (ConvertTo-CMakePath $VsRepeat), '-G', '"Visual Studio 17 2022"', '-A', 'x64') + $VsConfigureOptions)
	Invoke-ObservedProcess 'ninja-repeat-clean-debug' 'cmake' (Get-BuildArguments $NinjaRepeat $false)
	Invoke-ObservedProcess 'vs-repeat-clean-debug' 'cmake' (Get-BuildArguments $VsRepeat $true)
}

$Results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $RunnerTemp 'windows-build-generator-comparison.json') -Encoding utf8
$Results | Format-Table -AutoSize | Out-String | Write-Output
Write-Output 'BUILD_PROFILE_COMPLETE=PASS; Debug functional target set built under MSVC with Visual Studio and Ninja Multi-Config; CTest and packaging acceptance remain on the normal matrix.'
