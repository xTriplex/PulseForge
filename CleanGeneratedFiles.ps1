[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:RepoRoot = $null
$script:HadCleanupFailure = $false
# Mutating operations may target only these exact repository-relative paths.
# Visual Studio build trees are cleaned by recognized generator outputs while
# preserving per-build local quality tools. The shared Build\QualityTools
# environment is outside every cleanup target and survives all cleanup actions.
# The isolated CMakeSettings Out tree is removed
# only after its exact layout, source directory, generator, and links are checked.
$script:TargetManifest = @(
	@{ Path = 'Build\CMake\VS2022-x64'; Category = 'CMake build outputs'; Kind = 'CMakeBuildDirectory'; AllowedExtensions = $null },
	@{ Path = 'Build\CMake\VS2022-x64-OpenGL'; Category = 'CMake build outputs'; Kind = 'CMakeBuildDirectory'; AllowedExtensions = $null },
	@{ Path = 'Build\NoToolsProbeBuild'; Category = 'CMake build outputs'; Kind = 'CMakeBuildDirectory'; ExpectedSourceDirectory = 'Build\NoToolsProbeSource'; PreserveReparsePointNames = @('_tools'); AllowedExtensions = $null },
	@{ Path = 'Out'; Category = 'CMake build outputs'; Kind = 'CMakeSettingsOutputTree'; AllowedExtensions = $null },
	@{ Path = 'Out\build'; Category = 'CMake build outputs'; Kind = 'CMakeSettingsBuildDirectory'; AllowedExtensions = $null },
	@{ Path = 'Out\QualityGraphProbe\build'; Category = 'CMake build outputs'; Kind = 'CMakeFixtureBuildDirectory'; ExpectedSourceDirectory = 'Out\QualityGraphProbe'; AllowedExtensions = $null },
	@{ Path = 'Build\Diagnostics'; Category = 'Diagnostics and evidence'; Kind = 'GeneratedTree'; AllowedExtensions = $null },
	@{ Path = 'Build\IntegrationEvidence'; Category = 'Diagnostics and evidence'; Kind = 'GeneratedTree'; AllowedExtensions = $null },
	@{ Path = 'Build\CMake\VS2022-x64\Build\Diagnostics'; Category = 'Diagnostics and evidence'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'Build\CMake\VS2022-x64-OpenGL\Build\Diagnostics'; Category = 'Diagnostics and evidence'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'Out\build-editor-workspace-clcompile.log'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Out\build-editor-workspace-target.log'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Build\NoToolsProbeBuild\msbuild-diagnostic.log'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Out\QualityGraphProbe\A.cpp'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Out\QualityGraphProbe\B.cpp'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Out\QualityGraphProbe\Core.cpp'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Out\QualityGraphProbe\CMakeLists.txt'; Category = 'Diagnostics and evidence'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = 'Build\ParallelBuildProbe'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'CMakeFiles'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\enet\Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\enet\Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\Glad\Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\Glad\Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\Glfw\Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\Glfw\Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\imgui\Bin'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\imgui\Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'PulseForge\Vendor\tracy\Int'; Category = 'Legacy outputs'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = '.vs\PulseForge\v17\ipch'; Category = 'Visual Studio caches'; Kind = 'RestrictedDirectory'; AllowedExtensions = @('.ipch') },
	@{ Path = '.vs\PulseForge\FileContentIndex'; Category = 'Visual Studio caches'; Kind = 'RestrictedDirectory'; AllowedExtensions = @('.vsidx') },
	@{ Path = '.vs\PulseForge\v17\Browse.VC.db'; Category = 'Visual Studio caches'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = '.vs\PulseForge\CopilotIndices\17.14.1700.50054\CodeChunks.db'; Category = 'Visual Studio caches'; Kind = 'File'; AllowedExtensions = $null },
	@{ Path = '.vs\PulseForge\CopilotIndices\17.14.1700.50054\SemanticSymbols.db'; Category = 'Visual Studio caches'; Kind = 'File'; AllowedExtensions = $null }
)

$script:ActionManifest = @{
	Vulkan = @{ Label = 'Vulkan Visual Studio build outputs'; Paths = @('Build\CMake\VS2022-x64'); Confirm = 'DELETE VULKAN BUILD OUTPUTS' }
	OpenGL = @{ Label = 'OpenGL Visual Studio build outputs'; Paths = @('Build\CMake\VS2022-x64-OpenGL'); Confirm = 'DELETE OPENGL BUILD OUTPUTS' }
	Build = @{ Label = 'Vulkan, OpenGL, CMakeSettings, and completed probe build outputs'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'CMake build outputs' } | ForEach-Object { $_.Path }); Confirm = 'DELETE CMAKE BUILD OUTPUTS' }
	Legacy = @{ Label = 'legacy generated outputs and completed build probes'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Legacy outputs' } | ForEach-Object { $_.Path }); Confirm = 'DELETE LEGACY OUTPUTS' }
	Diagnostics = @{ Label = 'diagnostics, integration evidence, and diagnostic build logs'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Diagnostics and evidence' } | ForEach-Object { $_.Path }); Confirm = 'DELETE DIAGNOSTICS AND EVIDENCE' }
	VisualStudio = @{ Label = 'selected Visual Studio regenerable caches'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Visual Studio caches' } | ForEach-Object { $_.Path }); Confirm = 'DELETE VISUAL STUDIO CACHES' }
	Comprehensive = @{ Label = 'approved CMake outputs, diagnostics/evidence, legacy outputs, and selected VS caches'; Paths = @($script:TargetManifest | ForEach-Object { $_.Path }); Confirm = 'DELETE ALL APPROVED GENERATED FILES INCLUDING OUT DEPENDENCIES' }
}

$script:PreservedCMakeBuildChildren = @('QualityTools')
$script:GeneratedCMakeBuildChildren = @('QualityReports')
$script:RegisteredWorktreeProvider = $null

function Set-CleanerRoot
{
	param([Parameter(Mandatory = $true)][string]$Path)

	$resolvedRoot = [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
	$marker = Join-Path $resolvedRoot 'CMakeLists.txt'
	if (-not (Test-Path -LiteralPath $marker -PathType Leaf))
	{
		throw "CMakeLists.txt was not found at repository root '$resolvedRoot'."
	}
	$rootItem = Get-Item -LiteralPath $resolvedRoot -Force
	if (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
	{
		throw 'Refusing a repository root that is a symbolic link or junction.'
	}
	$markerItem = Get-Item -LiteralPath $marker -Force
	if (($markerItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
	{
		throw 'Refusing a repository marker CMakeLists.txt that is a symbolic link or junction.'
	}
	$script:RepoRoot = $resolvedRoot
}

function Get-ManifestEntry
{
	param([Parameter(Mandatory = $true)][string]$RelativePath)

	$matches = @($script:TargetManifest | Where-Object { [StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $RelativePath) })
	if ($matches.Count -ne 1)
	{
		throw "Refusing path not present exactly once in the cleanup allowlist: '$RelativePath'."
	}
	return $matches[0]
}

function Get-RegisteredWorktreePaths
{
	if ($null -ne $script:RegisteredWorktreeProvider)
	{
		return @(& $script:RegisteredWorktreeProvider)
	}

	$output = @(& git -C $script:RepoRoot worktree list --porcelain 2>&1)
	if ($LASTEXITCODE -ne 0)
	{
		throw "Cannot verify registered Git worktrees; cleanup is blocked. Git reported: $($output -join ' ')"
	}

	$paths = New-Object System.Collections.ArrayList
	foreach ($line in $output)
	{
		if ([string]$line -match '^worktree (.+)$')
		{
			$path = [IO.Path]::GetFullPath($Matches[1]).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
			$null = $paths.Add($path)
		}
	}
	return @($paths.ToArray())
}

function Test-PathContains
{
	param([Parameter(Mandatory = $true)][string]$Parent, [Parameter(Mandatory = $true)][string]$Child)

	$parentPath = [IO.Path]::GetFullPath($Parent).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
	$childPath = [IO.Path]::GetFullPath($Child).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
	if ([StringComparer]::OrdinalIgnoreCase.Equals($parentPath, $childPath)) { return $true }
	$prefix = $parentPath + [IO.Path]::DirectorySeparatorChar
	return $childPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Get-OverlappingRegisteredWorktree
{
	param([Parameter(Mandatory = $true)][string]$TargetPath)

	foreach ($worktreePath in (Get-RegisteredWorktreePaths))
	{
		# The active repository root is expected to contain cleanup targets.
		if ([StringComparer]::OrdinalIgnoreCase.Equals($worktreePath, $script:RepoRoot)) { continue }
		if ((Test-PathContains -Parent $TargetPath -Child $worktreePath) -or
			(Test-PathContains -Parent $worktreePath -Child $TargetPath))
		{
			return $worktreePath
		}
	}
	return $null
}

function Test-CMakeGeneratedBuildEntry
{
	param([Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][bool]$IsDirectory)

	if ($IsDirectory)
	{
		if ($Name -in @('CMakeFiles', 'Testing', 'Debug', 'Release', 'Dist', 'MinSizeRel', 'RelWithDebInfo',
			'Editor', 'Runtime', 'Assets', 'Shaders', 'GpuValidationShaders', 'PublicHeaderChecks', 'PulseForge', 'x64', '_deps'))
		{
			return $true
		}
		return $Name -match '^[A-Za-z0-9_.+-]+\.dir$'
	}

	if ($Name -in @('CMakeCache.txt', 'cmake_install.cmake', 'CTestTestfile.cmake', 'CPackConfig.cmake',
		'CPackSourceConfig.cmake', 'DartConfiguration.tcl', 'compile_commands.json', '.ninja_deps', '.ninja_log', 'build.ninja'))
	{
		return $true
	}
	return $Name -match '^[A-Za-z0-9_.+-]+\.sln$' -or
		$Name -match '^[A-Za-z0-9_.+-]+\.vcxproj(\.filters)?$' -or
		$Name -match '^PulseForgeTargetGraph-[A-Za-z0-9_.+-]+\.txt$'
}

function Test-CMakeDependencyBuildDirectory
{
	param([Parameter(Mandatory = $true)][string]$Path)

	# FetchContent uses "-src" for source checkouts and "-build"/"-subbuild"
	# for generated build trees. Preserve the entire cache if any entry is not one
	# of the generated forms so a future dependency source is never guessed away.
	foreach ($itemPath in [IO.Directory]::EnumerateFileSystemEntries($Path))
	{
		$attributes = [IO.File]::GetAttributes($itemPath)
		if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
			($attributes -band [IO.FileAttributes]::Directory) -eq 0)
		{
			return $false
		}

		$name = [IO.Path]::GetFileName($itemPath)
		if ($name -notmatch '^[A-Za-z0-9_.+-]+-(build|subbuild)$')
		{
			return $false
		}
	}
	return $true
}

function Assert-CMakeBuildDirectory
{
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[string[]]$AllowedGenerators = @('Visual Studio 17 2022'),
		[string]$ExpectedSourceDirectory = $script:RepoRoot
	)

	$cachePath = Join-Path $Path 'CMakeCache.txt'
	if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf))
	{
		throw "CMake ownership cannot be verified because '$cachePath' is missing. The target was preserved."
	}
	$cacheItem = Get-Item -LiteralPath $cachePath -Force
	if (($cacheItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
	{
		throw "Refusing CMake metadata that is a symbolic link or junction: '$cachePath'."
	}
	$cache = [IO.File]::ReadAllText($cachePath)
	$homeMatch = [Regex]::Match($cache, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
	$generatorMatch = [Regex]::Match($cache, '(?m)^CMAKE_GENERATOR:INTERNAL=(.+)$')
	if (-not $homeMatch.Success -or -not $generatorMatch.Success)
	{
		throw "CMake source/generator metadata is incomplete in '$cachePath'. The target was preserved."
	}
		$sourceTreePath = [IO.Path]::GetFullPath($homeMatch.Groups[1].Value.Trim()).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
		$expectedSourcePath = if ([IO.Path]::IsPathRooted($ExpectedSourceDirectory)) { [IO.Path]::GetFullPath($ExpectedSourceDirectory) } else { [IO.Path]::GetFullPath((Join-Path $script:RepoRoot $ExpectedSourceDirectory)) }
		$expectedSourcePath = $expectedSourcePath.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
		if (-not [StringComparer]::OrdinalIgnoreCase.Equals($sourceTreePath, $expectedSourcePath))
	{
			throw "CMake target '$Path' belongs to source tree '$sourceTreePath', not expected source '$expectedSourcePath'. The target was preserved."
	}
	if ($AllowedGenerators -notcontains $generatorMatch.Groups[1].Value.Trim())
	{
		throw "CMake target '$Path' uses unsupported generator '$($generatorMatch.Groups[1].Value.Trim())'. Allowed generators: $($AllowedGenerators -join ', '). The target was preserved."
	}
}

function Get-CMakeSettingsOutputTreePlan
{
	param([Parameter(Mandatory = $true)][string]$Path)

	$entry = [pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = 'Out' }
	$bytes = Get-TreeInspection -Path $Path -Entry $entry
	$rootChildren = @([IO.Directory]::EnumerateFileSystemEntries($Path))
	$rootNames = @($rootChildren | ForEach-Object { [IO.Path]::GetFileName($_) })
	if ($rootNames -notcontains 'build' -or @($rootNames | Where-Object { $_ -notin @('build', 'install') }).Count -gt 0)
	{
		throw "The Out tree must contain only CMakeSettings 'build' and optional 'install' directories. The target was preserved."
	}

	$buildRoot = Join-Path $Path 'build'
	if (-not (Test-Path -LiteralPath $buildRoot -PathType Container))
	{
		throw "The Out tree has no CMakeSettings build directory. The target was preserved."
	}
	$buildDirectories = @([IO.Directory]::EnumerateFileSystemEntries($buildRoot))
	if ($buildDirectories.Count -eq 0)
	{
		throw "The Out build directory contains no configured build trees. The target was preserved."
	}

	$configurationNames = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
	foreach ($buildDirectory in $buildDirectories)
	{
		$attributes = [IO.File]::GetAttributes($buildDirectory)
		if (($attributes -band [IO.FileAttributes]::Directory) -eq 0 -or [IO.Path]::GetFileName($buildDirectory) -notmatch '^[A-Za-z0-9_.+-]+$')
		{
			throw "Unexpected item in the Out CMakeSettings build directory: '$buildDirectory'. The target was preserved."
		}
		Assert-CMakeBuildDirectory -Path $buildDirectory -AllowedGenerators @('Ninja')
		$null = $configurationNames.Add([IO.Path]::GetFileName($buildDirectory))
	}

	$installRoot = Join-Path $Path 'install'
	if (Test-Path -LiteralPath $installRoot)
	{
		foreach ($installDirectory in [IO.Directory]::EnumerateFileSystemEntries($installRoot))
		{
			$attributes = [IO.File]::GetAttributes($installDirectory)
			$name = [IO.Path]::GetFileName($installDirectory)
			if (($attributes -band [IO.FileAttributes]::Directory) -eq 0 -or -not $configurationNames.Contains($name))
			{
				throw "Unexpected item in the Out CMakeSettings install directory: '$installDirectory'. The target was preserved."
			}
		}
	}

	$generatedDirectories = @($buildDirectories)
	if (Test-Path -LiteralPath $installRoot)
	{
		$generatedDirectories += @([IO.Directory]::EnumerateFileSystemEntries($installRoot))
	}
	return [pscustomobject]@{ Bytes = [long]$bytes; GeneratedDirectories = $generatedDirectories }
}

function Get-CMakeBuildPlan
{
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[string]$ExpectedSourceDirectory = $script:RepoRoot,
		[string[]]$AllowedGenerators = @('Visual Studio 17 2022'),
		[string[]]$PreserveReparsePointNames = @()
	)

	Assert-CMakeBuildDirectory -Path $Path -AllowedGenerators $AllowedGenerators -ExpectedSourceDirectory $ExpectedSourceDirectory
	$bytes = [long]0
	$remove = New-Object System.Collections.ArrayList
	$preserve = New-Object System.Collections.ArrayList
	foreach ($itemPath in [IO.Directory]::EnumerateFileSystemEntries($Path))
	{
		$name = [IO.Path]::GetFileName($itemPath)
		if ($script:PreservedCMakeBuildChildren -contains $name)
		{
			$null = $preserve.Add($itemPath)
			continue
		}
		if ($script:GeneratedCMakeBuildChildren -contains $name)
		{
			$attributes = [IO.File]::GetAttributes($itemPath)
			if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
				($attributes -band [IO.FileAttributes]::Directory) -eq 0)
			{
				throw "Expected a regular generated report directory: '$itemPath'. The CMake target was preserved."
			}
			$itemBytes = Get-TreeInspection -Path $itemPath -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = $name })
			$bytes += [long]$itemBytes
			$null = $remove.Add([pscustomobject]@{ Path = $itemPath; Bytes = [long]$itemBytes })
			continue
		}
		$attributes = [IO.File]::GetAttributes($itemPath)
		if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
		{
			if ($PreserveReparsePointNames -contains $name)
			{
				$null = $preserve.Add($itemPath)
				continue
			}
			throw "Refusing CMake output containing a symbolic link or junction: '$itemPath'."
		}
		$isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
			if ($name -eq '_deps' -and -not (Test-CMakeDependencyBuildDirectory -Path $itemPath))
			{
				$null = $preserve.Add($itemPath)
				continue
			}
		if (-not (Test-CMakeGeneratedBuildEntry -Name $name -IsDirectory $isDirectory))
		{
			$null = $preserve.Add($itemPath)
			continue
		}

		if ($isDirectory)
		{
			$itemBytes = Get-TreeInspection -Path $itemPath -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = $name })
		}
		else
		{
			$itemBytes = (New-Object IO.FileInfo($itemPath)).Length
		}
		$bytes += $itemBytes
		$null = $remove.Add([pscustomobject]@{ Path = $itemPath; Bytes = [long]$itemBytes })
	}
		$orderedRemove = @($remove.ToArray() | Sort-Object `
			@{ Expression = { if ([IO.Path]::GetFileName($_.Path) -eq 'CMakeCache.txt') { 1 } else { 0 } } },
			@{ Expression = { $_.Path } })
		return [pscustomobject]@{ Bytes = $bytes; ItemsToRemove = $orderedRemove; PreservedItems = @($preserve.ToArray()) }
}

function Get-CMakeSettingsBuildDirectoryPlan
{
	param([Parameter(Mandatory = $true)][string]$Path)

	$buildDirectories = @([IO.Directory]::EnumerateFileSystemEntries($Path))
	if ($buildDirectories.Count -eq 0) { throw "CMakeSettings build directory '$Path' is empty; the target was preserved." }
	foreach ($buildDirectory in $buildDirectories)
	{
		$attributes = [IO.File]::GetAttributes($buildDirectory)
		if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
			($attributes -band [IO.FileAttributes]::Directory) -eq 0 -or
			[IO.Path]::GetFileName($buildDirectory) -notmatch '^[A-Za-z0-9_.+-]+$')
		{
			throw "Unexpected item in CMakeSettings build directory: '$buildDirectory'. The target was preserved."
		}
		Assert-CMakeBuildDirectory -Path $buildDirectory -AllowedGenerators @('Ninja') -ExpectedSourceDirectory $script:RepoRoot
	}

	$bytes = Get-TreeInspection -Path $Path -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = [IO.Path]::GetFileName($Path) })
	return [pscustomobject]@{ Bytes = [long]$bytes; GeneratedDirectories = $buildDirectories }
}

function Assert-NoTrackedFiles
{
	param([Parameter(Mandatory = $true)][string]$RelativePath)

	$gitPath = $RelativePath.Replace('\', '/')
	$tracked = @(& git -C $script:RepoRoot ls-files --full-name -- $gitPath 2>&1)
	if ($LASTEXITCODE -ne 0)
	{
		throw "Cannot verify tracked-file protection for '$RelativePath'; cleanup is blocked. Git reported: $($tracked -join ' ')"
	}
	$tracked = @($tracked | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
	if ($tracked.Count -gt 0)
	{
		throw "Refusing cleanup target '$RelativePath' because it contains tracked file(s): $($tracked -join ', ')."
	}
}

function Get-GeneratedTreeEntryPlan
{
	param([Parameter(Mandatory = $true)][string]$Path)

	$attributes = [IO.File]::GetAttributes($Path)
	if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
	{
		return [pscustomobject]@{ Bytes = [long]0; ItemsToRemove = @(); PreservedItems = @([pscustomobject]@{ Path = $Path; Reason = 'reparse point' }) }
	}

	if (($attributes -band [IO.FileAttributes]::Directory) -ne 0)
	{
		try
		{
			$itemBytes = Get-TreeInspection -Path $Path -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = [IO.Path]::GetFileName($Path) })
			return [pscustomobject]@{ Bytes = [long]$itemBytes; ItemsToRemove = @([pscustomobject]@{ Path = $Path; Bytes = [long]$itemBytes }); PreservedItems = @() }
		}
		catch
		{
			if ($_.Exception.Message -notmatch 'symbolic link or junction') { throw }
		}

		$remove = New-Object System.Collections.ArrayList
		$preserve = New-Object System.Collections.ArrayList
		$bytes = [long]0
		foreach ($childPath in [IO.Directory]::EnumerateFileSystemEntries($Path))
		{
			$childPlan = Get-GeneratedTreeEntryPlan -Path $childPath
			$bytes += [long]$childPlan.Bytes
			foreach ($item in $childPlan.ItemsToRemove) { $null = $remove.Add($item) }
			foreach ($item in $childPlan.PreservedItems) { $null = $preserve.Add($item) }
		}
		return [pscustomobject]@{ Bytes = $bytes; ItemsToRemove = @($remove.ToArray()); PreservedItems = @($preserve.ToArray()) }
	}

	$itemBytes = (New-Object IO.FileInfo($Path)).Length
	return [pscustomobject]@{ Bytes = [long]$itemBytes; ItemsToRemove = @([pscustomobject]@{ Path = $Path; Bytes = [long]$itemBytes }); PreservedItems = @() }
}

function Get-GeneratedTreePlan
{
	param([Parameter(Mandatory = $true)][string]$Path)

	$remove = New-Object System.Collections.ArrayList
	$preserve = New-Object System.Collections.ArrayList
	$bytes = [long]0
	foreach ($itemPath in [IO.Directory]::EnumerateFileSystemEntries($Path))
	{
		$itemPlan = Get-GeneratedTreeEntryPlan -Path $itemPath
		$bytes += [long]$itemPlan.Bytes
		foreach ($item in $itemPlan.ItemsToRemove) { $null = $remove.Add($item) }
		foreach ($item in $itemPlan.PreservedItems) { $null = $preserve.Add($item) }
	}

	return [pscustomobject]@{ Bytes = $bytes; ItemsToRemove = @($remove.ToArray()); PreservedItems = @($preserve.ToArray()) }
}

function Get-TargetFullPath
{
	param([Parameter(Mandatory = $true)][string]$RelativePath)

	$null = Get-ManifestEntry $RelativePath
	if ([IO.Path]::IsPathRooted($RelativePath) -or $RelativePath.Contains('..'))
	{
		throw "Invalid relative cleanup path '$RelativePath'."
	}
	$full = [IO.Path]::GetFullPath((Join-Path $script:RepoRoot $RelativePath))
	$expected = [IO.Path]::GetFullPath((Join-Path $script:RepoRoot (Get-ManifestEntry $RelativePath).Path))
	if (-not [StringComparer]::OrdinalIgnoreCase.Equals($full, $expected))
	{
		throw "Resolved path did not match the exact allowlist entry '$RelativePath'."
	}
	$worktreeConflict = Get-OverlappingRegisteredWorktree -TargetPath $full
	if ($worktreeConflict)
	{
		throw "Refusing cleanup target '$full' because it overlaps registered worktree '$worktreeConflict'."
	}
	Assert-NoTrackedFiles -RelativePath $RelativePath
	$prefix = $script:RepoRoot + [IO.Path]::DirectorySeparatorChar
	if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase))
	{
		throw "Resolved cleanup target escaped the repository root: '$full'."
	}
	$relativeParts = $RelativePath -split '[\\/]'
	$current = $script:RepoRoot
	foreach ($part in $relativeParts)
	{
		$current = Join-Path $current $part
		if (Test-Path -LiteralPath $current)
		{
			$component = Get-Item -LiteralPath $current -Force
			if (($component.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
			{
				throw "Refusing allowlisted path containing a symbolic link or junction component: '$current'."
			}
		}
	}
	return $full
}

function Get-TreeInspection
{
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)]$Entry
	)

	$bytes = [long]0
	$pending = New-Object 'System.Collections.Generic.Stack[string]'
	$pending.Push($Path)
	while ($pending.Count -gt 0)
	{
		$directory = $pending.Pop()
		foreach ($itemPath in [IO.Directory]::EnumerateFileSystemEntries($directory))
		{
			$attributes = [IO.File]::GetAttributes($itemPath)
			if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
			{
				throw "Refusing cleanup target containing a symbolic link or junction: '$itemPath'."
			}
			if (($attributes -band [IO.FileAttributes]::Directory) -ne 0)
			{
				$pending.Push($itemPath)
				continue
			}
			if ($Entry.Kind -eq 'RestrictedDirectory')
			{
				$extension = [IO.Path]::GetExtension($itemPath)
				if ($Entry.AllowedExtensions -notcontains $extension)
				{
					throw "Unexpected file in restricted cache target '$($Entry.Path)': '$itemPath'. No target files were removed."
				}
			}
			$bytes += (New-Object IO.FileInfo($itemPath)).Length
		}
	}
	return $bytes
}

function Get-TargetInspection
{
	param([Parameter(Mandatory = $true)][string]$RelativePath)

	$entry = Get-ManifestEntry $RelativePath
	$full = Get-TargetFullPath $RelativePath
	if (-not (Test-Path -LiteralPath $full))
	{
			return [pscustomobject]@{
				RelativePath = $entry.Path
				FullPath = $full
				Exists = $false
				Bytes = [long]0
				Entry = $entry
				GeneratedItems = @()
				PreservedItems = @()
				Error = $null
			}
	}
	$item = Get-Item -LiteralPath $full -Force
	if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
	{
		throw "Refusing a cleanup target that is a symbolic link or junction: '$full'."
	}
	if ($entry.Kind -eq 'File' -and $item.PSIsContainer)
	{
		throw "Expected an exact cache file, but found a directory: '$full'."
	}
	if ($entry.Kind -ne 'File' -and -not $item.PSIsContainer)
	{
		throw "Expected an allowlisted generated directory, but found a file: '$full'."
	}
	if ($entry.Kind -eq 'File')
	{
		$bytes = [long]$item.Length
	}
	elseif ($entry.Kind -eq 'CMakeBuildDirectory')
	{
		$expectedSource = if ($entry.ContainsKey('ExpectedSourceDirectory')) { $entry.ExpectedSourceDirectory } else { $script:RepoRoot }
		$allowedGenerators = if ($entry.ContainsKey('AllowedGenerators')) { $entry.AllowedGenerators } else { @('Visual Studio 17 2022') }
		$preserveReparseNames = if ($entry.ContainsKey('PreserveReparsePointNames')) { $entry.PreserveReparsePointNames } else { @() }
		$plan = Get-CMakeBuildPlan -Path $full -ExpectedSourceDirectory $expectedSource -AllowedGenerators $allowedGenerators -PreserveReparsePointNames $preserveReparseNames
		return [pscustomobject]@{
			RelativePath = $entry.Path
			FullPath = $full
			Exists = $true
			Bytes = [long]$plan.Bytes
			Entry = $entry
			GeneratedItems = @($plan.ItemsToRemove)
			PreservedItems = @($plan.PreservedItems)
			Error = $null
		}
	}
	elseif ($entry.Kind -eq 'CMakeSettingsOutputTree')
	{
		$plan = Get-CMakeSettingsOutputTreePlan -Path $full
		return [pscustomobject]@{
			RelativePath = $entry.Path
			FullPath = $full
			Exists = $true
			Bytes = [long]$plan.Bytes
			Entry = $entry
			GeneratedItems = @($plan.GeneratedDirectories)
			PreservedItems = @()
			Error = $null
		}
	}
	elseif ($entry.Kind -eq 'CMakeSettingsBuildDirectory')
	{
		$plan = Get-CMakeSettingsBuildDirectoryPlan -Path $full
		return [pscustomobject]@{
			RelativePath = $entry.Path
			FullPath = $full
			Exists = $true
			Bytes = [long]$plan.Bytes
			Entry = $entry
			GeneratedItems = @($plan.GeneratedDirectories)
			PreservedItems = @()
			Error = $null
		}
	}
	elseif ($entry.Kind -eq 'CMakeFixtureBuildDirectory')
	{
		Assert-CMakeBuildDirectory -Path $full -AllowedGenerators @('Ninja') -ExpectedSourceDirectory $entry.ExpectedSourceDirectory
		$bytes = Get-TreeInspection -Path $full -Entry $entry
	}
	elseif ($entry.Kind -eq 'GeneratedTree')
	{
		$plan = Get-GeneratedTreePlan -Path $full
		return [pscustomobject]@{
			RelativePath = $entry.Path
			FullPath = $full
			Exists = $true
			Bytes = [long]$plan.Bytes
			Entry = $entry
			GeneratedItems = @($plan.ItemsToRemove)
			PreservedItems = @($plan.PreservedItems)
			Error = $null
		}
	}
	else
	{
		$bytes = Get-TreeInspection -Path $full -Entry $entry
	}
	return [pscustomobject]@{
		RelativePath = $entry.Path
		FullPath = $full
		Exists = $true
		Bytes = $bytes
		Entry = $entry
		GeneratedItems = @()
		PreservedItems = @()
		Error = $null
	}
}

function Format-Bytes
{
	param([long]$Bytes)
	return ('{0:N2} GiB ({1:N0} bytes)' -f ($Bytes / 1GB), $Bytes)
}

function Get-ActiveProcessWarnings
{
	$names = @('devenv', 'Code', 'codex', 'PulseForgeEditor', 'PulseForgeGame', 'PulseForgeTests', 'PulseForgeCoreTests',
		'PulseForgeSceneTests', 'PulseForgeAssetTests', 'PulseForgeRendererTests', 'PulseForgeEditorTests',
		'PulseForgeGpuValidation', 'cmake', 'cmake-gui', 'ctest', 'ninja', 'MSBuild', 'cl', 'link', 'python', 'python3', 'VBCSCompiler', 'dotnet')
	$found = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
		($names -contains $_.ProcessName) -or ($_.ProcessName -like 'ServiceHub.*') -or ($_.ProcessName -like 'Microsoft.ServiceHub.*')
	} | Sort-Object Id -Unique)
	return $found
}

function Find-PulseForgeLauncherProcesses
{
	param([object[]]$ProcessMetadata)

	$rootPattern = [Regex]::Escape($script:RepoRoot)
	$launcherPatterns = @(
		@{ Pattern = '(?i)\bPulseForge(Editor|Game)(\.exe)?\b'; Reason = 'command line names a PulseForge executable' },
		@{ Pattern = '(?i)' + $rootPattern + '[\\/](Build|Out[\\/]build)([\\/]|\b)'; Reason = 'command line references a repository build directory' }
	)
	$results = New-Object System.Collections.ArrayList
	foreach ($process in $ProcessMetadata)
	{
		if ([string]::IsNullOrWhiteSpace($process.CommandLine)) { continue }
		foreach ($pattern in $launcherPatterns)
		{
			if ($process.CommandLine -match $pattern.Pattern)
			{
				$null = $results.Add([pscustomobject]@{ Name = $process.Name; Id = $process.ProcessId; Reason = $pattern.Reason })
				break
			}
		}
	}
	return @($results.ToArray())
}

function Get-ShellLauncherAudit
{
	$shellProcesses = @(Get-Process -Name cmd,powershell,pwsh -ErrorAction SilentlyContinue)
	$filter = "Name = 'cmd.exe' OR Name = 'powershell.exe' OR Name = 'pwsh.exe'"
	try
	{
		$metadata = @(Get-CimInstance -ClassName Win32_Process -Filter $filter -ErrorAction Stop)
		return [pscustomobject]@{ Available = $true; ShellCount = $shellProcesses.Count; Matches = @(Find-PulseForgeLauncherProcesses -ProcessMetadata $metadata) }
	}
	catch
	{
		try
		{
			$metadata = @(Get-WmiObject -Class Win32_Process -Filter $filter -ErrorAction Stop)
			return [pscustomobject]@{ Available = $true; ShellCount = $shellProcesses.Count; Matches = @(Find-PulseForgeLauncherProcesses -ProcessMetadata $metadata) }
		}
		catch
		{
			return [pscustomobject]@{ Available = $false; ShellCount = $shellProcesses.Count; Matches = @() }
		}
	}
}

function Show-SizeReport
{
	if (-not $script:RepoRoot) { throw 'Cleaner root has not been initialized.' }
	$total = [long]0
	$categoryTotals = @{}
	$rows = New-Object System.Collections.ArrayList
	Write-Host 'Allowlisted cleanup targets (missing paths are reported as not present):'
	foreach ($entry in $script:TargetManifest)
	{
		try
		{
			$inspection = Get-TargetInspection -RelativePath $entry.Path
			if ($inspection.Exists)
			{
				Write-Host ('  {0}  {1}' -f $inspection.FullPath, (Format-Bytes $inspection.Bytes))
				$null = $rows.Add([pscustomobject]@{ RelativePath = $entry.Path; FullPath = $inspection.FullPath; Exists = $true; Bytes = $inspection.Bytes })
				$total += $inspection.Bytes
				if (-not $categoryTotals.ContainsKey($entry.Category)) { $categoryTotals[$entry.Category] = [long]0 }
				$categoryTotals[$entry.Category] += $inspection.Bytes
			}
			else
			{
				Write-Host ('  {0}  not present' -f $inspection.FullPath)
				$null = $rows.Add([pscustomobject]@{ RelativePath = $entry.Path; FullPath = $inspection.FullPath; Exists = $false; Bytes = [long]0 })
			}
		}
		catch
		{
			Write-Host ('  {0}  NOT MEASURED: {1}' -f (Join-Path $script:RepoRoot $entry.Path), $_.Exception.Message) -ForegroundColor Yellow
		}
	}
	Write-Host ''
	foreach ($category in @('CMake build outputs', 'Diagnostics and evidence', 'Legacy outputs', 'Visual Studio caches'))
	{
		if ($categoryTotals.ContainsKey($category))
		{
			Write-Host ('{0} subtotal: {1}' -f $category, (Format-Bytes $categoryTotals[$category]))
		}
	}
	Write-Host ('All allowlisted targets total: {0}' -f (Format-Bytes $total))
	return [pscustomobject]@{ Entries = @($rows.ToArray()); TotalBytes = $total; CategoryTotals = $categoryTotals }
}

function Show-CleanupPreview
{
	param([Parameter(Mandatory = $true)][string]$ActionName)

	if (-not $script:ActionManifest.ContainsKey($ActionName)) { throw "Unknown cleanup action '$ActionName'." }
	$action = $script:ActionManifest[$ActionName]
	$total = [long]0
	Write-Host ("Preview: {0}" -f $action.Label)
	Write-Host '  retain Build\QualityTools (shared quality environment; not a cleanup target)'
	foreach ($relativePath in $action.Paths)
	{
		try { $inspection = Get-TargetInspection -RelativePath $relativePath }
		catch
		{
			Write-Host ('  {0}  NOT SAFE: {1}' -f (Join-Path $script:RepoRoot $relativePath), $_.Exception.Message) -ForegroundColor Yellow
			continue
		}
		if (-not $inspection.Exists)
		{
			Write-Host ('  {0}  not present' -f $inspection.FullPath)
			continue
		}
		$total += $inspection.Bytes
		Write-Host ('  {0}  {1}' -f $inspection.FullPath, (Format-Bytes $inspection.Bytes))
		if ($inspection.Entry.Kind -eq 'CMakeBuildDirectory')
		{
			foreach ($item in $inspection.GeneratedItems)
			{
				Write-Host ('    remove {0}  {1}' -f $item.Path, (Format-Bytes $item.Bytes))
			}
			foreach ($item in $inspection.PreservedItems)
			{
				Write-Host ('    preserve {0}' -f $item)
			}
		}
		elseif ($inspection.Entry.Kind -eq 'CMakeSettingsOutputTree')
		{
			Write-Host '    remove the validated Out CMakeSettings Ninja build/install tree, including any fetched dependency caches'
		}
		elseif ($inspection.Entry.Kind -eq 'CMakeSettingsBuildDirectory')
		{
			foreach ($item in $inspection.GeneratedItems) { Write-Host ('    remove {0}' -f $item) }
		}
		elseif ($inspection.Entry.Kind -eq 'GeneratedTree')
		{
			foreach ($item in $inspection.GeneratedItems) { Write-Host ('    remove {0}  {1}' -f $item.Path, (Format-Bytes $item.Bytes)) }
			foreach ($item in $inspection.PreservedItems) { Write-Host ('    preserve {0} ({1})' -f $item.Path, $item.Reason) }
		}
	}
	Write-Host ('Preview total: {0}. No files were changed.' -f (Format-Bytes $total))
	return [pscustomobject]@{ TotalBytes = $total }
}

function Resolve-MenuSelection
{
	param([string]$Selection)
	switch ($Selection)
	{
		'1' { return 'Vulkan' }
		'2' { return 'OpenGL' }
		'3' { return 'Build' }
		'4' { return 'Legacy' }
		'5' { return 'VisualStudio' }
		'6' { return 'Comprehensive' }
		'7' { return 'Preview' }
		'8' { return 'Sizes' }
		'9' { return 'Diagnostics' }
		'10' { return 'Cancel' }
		default { return $null }
	}
}

function Remove-InspectedTarget
{
	param([Parameter(Mandatory = $true)]$Inspection)

	if ($Inspection.Entry.Kind -eq 'CMakeBuildDirectory')
	{
		$current = Get-TargetInspection -RelativePath $Inspection.RelativePath
		$prefix = $current.FullPath + [IO.Path]::DirectorySeparatorChar
		foreach ($generated in $current.GeneratedItems)
		{
			$generatedPath = [IO.Path]::GetFullPath($generated.Path)
			if (-not $generatedPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase))
			{
				throw "Generated CMake entry escaped its validated build directory: '$generatedPath'."
			}
			$attributes = [IO.File]::GetAttributes($generatedPath)
			if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
			{
				throw "Refusing generated CMake entry that became a symbolic link or junction: '$generatedPath'."
			}
			if (($attributes -band [IO.FileAttributes]::Directory) -ne 0)
			{
				$null = Get-TreeInspection -Path $generatedPath -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = [IO.Path]::GetFileName($generatedPath) })
			}
			Remove-Item -LiteralPath $generatedPath -Recurse -Force -ErrorAction Stop
			if (Test-Path -LiteralPath $generatedPath) { throw "Generated CMake entry still exists after deletion: '$generatedPath'." }
		}
			if (-not (Test-Path -LiteralPath $current.FullPath -PathType Container))
			{
				throw 'CMake build directory disappeared during cleanup.'
			}
			$allowedReparseNames = if ($current.Entry.ContainsKey('PreserveReparsePointNames')) { $current.Entry.PreserveReparsePointNames } else { @() }
			foreach ($remainingPath in [IO.Directory]::EnumerateFileSystemEntries($current.FullPath))
			{
				$attributes = [IO.File]::GetAttributes($remainingPath)
				if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
				{
					if ($allowedReparseNames -contains [IO.Path]::GetFileName($remainingPath)) { continue }
					throw "A symbolic link or junction appeared during cleanup: '$remainingPath'."
				}
				$isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
				$remainingName = [IO.Path]::GetFileName($remainingPath)
				if ($remainingName -eq '_deps' -and -not (Test-CMakeDependencyBuildDirectory -Path $remainingPath))
				{
					continue
				}
				if (Test-CMakeGeneratedBuildEntry -Name $remainingName -IsDirectory $isDirectory)
				{
					throw "Generated CMake output remains after cleanup: '$remainingPath'."
				}
			}
		return
	}

	if ($Inspection.Entry.Kind -eq 'CMakeSettingsOutputTree')
	{
		$current = Get-TargetInspection -RelativePath $Inspection.RelativePath
		if (-not $current.Exists) { return }
		Remove-Item -LiteralPath $current.FullPath -Recurse -Force -ErrorAction Stop
		if (Test-Path -LiteralPath $current.FullPath) { throw 'The validated Out build tree still exists after cleanup.' }
		return
	}

	if ($Inspection.Entry.Kind -eq 'CMakeSettingsBuildDirectory' -or
		$Inspection.Entry.Kind -eq 'CMakeFixtureBuildDirectory')
	{
		$current = Get-TargetInspection -RelativePath $Inspection.RelativePath
		if (-not $current.Exists) { return }
		Remove-Item -LiteralPath $current.FullPath -Recurse -Force -ErrorAction Stop
		if (Test-Path -LiteralPath $current.FullPath) { throw 'The validated generated CMake build directory still exists after cleanup.' }
		return
	}

	if ($Inspection.Entry.Kind -eq 'GeneratedTree')
	{
		$current = Get-TargetInspection -RelativePath $Inspection.RelativePath
		if (-not $current.Exists) { return }
		foreach ($generatedItem in $current.GeneratedItems)
		{
			$attributes = [IO.File]::GetAttributes($generatedItem.Path)
			if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
			{
				throw "A reparse point appeared in a generated cleanup tree: '$($generatedItem.Path)'."
			}
			if (($attributes -band [IO.FileAttributes]::Directory) -ne 0)
			{
				$null = Get-TreeInspection -Path $generatedItem.Path -Entry ([pscustomobject]@{ Kind = 'Directory'; AllowedExtensions = $null; Path = [IO.Path]::GetFileName($generatedItem.Path) })
			}
			Remove-Item -LiteralPath $generatedItem.Path -Recurse -Force -ErrorAction Stop
			if (Test-Path -LiteralPath $generatedItem.Path) { throw "Diagnostic output remains after cleanup: '$($generatedItem.Path)'." }
		}
		return
	}

	Remove-Item -LiteralPath $Inspection.FullPath -Recurse -Force -ErrorAction Stop
	if (Test-Path -LiteralPath $Inspection.FullPath) { throw 'Target still exists after deletion.' }
}

function Invoke-CleanupAction
{
	param([Parameter(Mandatory = $true)][string]$ActionName)

	if (-not $script:ActionManifest.ContainsKey($ActionName)) { throw "Unknown cleanup action '$ActionName'." }
	$action = $script:ActionManifest[$ActionName]
	$inspections = New-Object System.Collections.ArrayList
	$invalid = New-Object System.Collections.ArrayList
	$total = [long]0
	Write-Host ''
	Write-Host ("Action: {0}" -f $action.Label)
	Write-Host 'Exact targets and inspected sizes:'
	foreach ($relativePath in $action.Paths)
	{
		try
		{
			$inspection = Get-TargetInspection -RelativePath $relativePath
			$null = $inspections.Add($inspection)
			$total += $inspection.Bytes
			$state = if ($inspection.Exists) { Format-Bytes $inspection.Bytes } else { 'not present' }
			Write-Host ('  {0}  {1}' -f $inspection.FullPath, $state)
			if ($inspection.Entry.Kind -eq 'CMakeBuildDirectory')
			{
				foreach ($item in $inspection.GeneratedItems) { Write-Host ('    remove {0}  {1}' -f $item.Path, (Format-Bytes $item.Bytes)) }
				foreach ($item in $inspection.PreservedItems) { Write-Host ('    preserve {0}' -f $item) }
			}
			elseif ($inspection.Entry.Kind -eq 'CMakeSettingsOutputTree')
			{
				Write-Host '    remove the validated CMakeSettings Ninja build/install tree, including any fetched dependency caches'
			}
		}
		catch
		{
			$full = Join-Path $script:RepoRoot $relativePath
			$null = $invalid.Add([pscustomobject]@{ RelativePath = $relativePath; FullPath = $full; Message = $_.Exception.Message })
			Write-Host ('  {0}  NOT SAFE: {1}' -f $full, $_.Exception.Message) -ForegroundColor Red
		}
	}
	Write-Host ('Inspected total: {0}' -f (Format-Bytes $total))
	if ($invalid.Count -gt 0)
	{
		Write-Warning "$($invalid.Count) target(s) failed safety inspection. They will be preserved; other safe targets may still be cleaned."
	}
	if ($inspections.Count -eq 0 -or @($inspections | Where-Object { $_.Exists }).Count -eq 0)
	{
		Write-Host 'No existing safe targets were found. Nothing to delete.'
		return [pscustomobject]@{ Succeeded = 0; Failed = $invalid.Count; Skipped = $inspections.Count }
	}

	$active = @(Get-ActiveProcessWarnings)
	if ($active.Count -gt 0)
	{
		Write-Warning 'The following processes may be using generated files. The utility will not terminate them. Close relevant applications/builds before proceeding; locked files will be reported as failures.'
		foreach ($process in $active) { Write-Host ('  {0}.exe (PID {1})' -f $process.ProcessName, $process.Id) }
	}
	$shellAudit = Get-ShellLauncherAudit
	if ($shellAudit.Matches.Count -gt 0)
	{
		Write-Warning 'Possible PulseForge launch wrapper(s) were identified from process command lines. This does not reveal their current working directories or prove they own a file lock:'
		foreach ($process in $shellAudit.Matches) { Write-Host ('  {0}.exe (PID {1}): {2}' -f $process.Name, $process.Id, $process.Reason) }
	}
	elseif (-not $shellAudit.Available)
	{
		Write-Host 'Shell command-line metadata is unavailable in this session; no shell was classified as a PulseForge launcher.'
	}
	Write-Host 'A cmd.exe, PowerShell, or terminal may hold a generated directory as its current working directory. Command-line metadata does not establish that directory; close shells you launched from a cleanup target before proceeding.'
	if ($invalid.Count -gt 0)
	{
		$skip = Read-Host 'Type CLEAN SAFE TARGETS ONLY to continue with inspected targets; anything else cancels'
		if ($skip -cne 'CLEAN SAFE TARGETS ONLY')
		{
			Write-Host 'Canceled; no files were deleted.'
			return [pscustomobject]@{ Succeeded = 0; Failed = $invalid.Count; Skipped = $inspections.Count }
		}
	}
	Write-Host 'User data outside the allowlist includes .git, CMakeSettings.json, imgui.ini, *.vcxproj.user, source/assets, CMakeUserPresets.json, Build\QualityTools (shared quality environment), per-build QualityTools, and PulseForgeGame\.pulseforge\derived.'
	if ($action.Paths -contains 'Out')
	{
		Write-Warning 'Cleaning Out removes its validated Ninja build/install outputs and any fetched dependency sources or downloads inside them. A later configure may need network access to fetch pinned dependencies again.'
	}
	if ($ActionName -eq 'VisualStudio' -or $ActionName -eq 'Comprehensive')
	{
		Write-Host 'This removes only listed IntelliSense PCH, FileContentIndex .vsidx files, Browse.VC.db, and Copilot index databases. CopilotSnapshots and Visual Studio settings are preserved.'
	}
	$confirmation = Read-Host ("Review the exact full paths above. Type '{0}' to permanently delete existing safe targets" -f $action.Confirm)
	if ($confirmation -cne $action.Confirm)
	{
		Write-Host 'Confirmation did not match; no files were deleted.'
		return [pscustomobject]@{ Succeeded = 0; Failed = $invalid.Count; Skipped = $inspections.Count }
	}

	$succeeded = 0
	$failed = $invalid.Count
	$skipped = 0
	foreach ($inspection in $inspections)
	{
		if (-not $inspection.Exists) { $skipped++; continue }
		try
		{
			# Re-inspect immediately before mutation to detect path replacement or new reparse points.
			$current = Get-TargetInspection -RelativePath $inspection.RelativePath
			if (-not $current.Exists) { $skipped++; continue }
			if (-not [StringComparer]::OrdinalIgnoreCase.Equals($current.FullPath, $inspection.FullPath))
			{
				throw 'Resolved target changed after confirmation.'
			}
			Remove-InspectedTarget -Inspection $current
			$succeeded++
			Write-Host ('Removed {0} ({1}).' -f $current.FullPath, (Format-Bytes $current.Bytes))
		}
		catch
		{
			$failed++
			Write-Error -ErrorAction Continue ("Failed to remove '{0}': {1}. The target may be partially removed; a Windows file lock or permission may be responsible. Use Resource Monitor > CPU > Associated Handles to search for the target path/name, close the owning application or shell manually, then retry. Remaining targets will still be attempted." -f $inspection.FullPath, $_.Exception.Message)
		}
	}
	Write-Host ("Cleanup result: {0} removed, {1} failed/possibly partial, {2} absent." -f $succeeded, $failed, $skipped)
	return [pscustomobject]@{ Succeeded = $succeeded; Failed = $failed; Skipped = $skipped }
}

function Invoke-CleanerMain
{
	try
	{
		Set-CleanerRoot -Path $PSScriptRoot
	}
	catch
	{
		Write-Error $_
		exit 2
	}

	Write-Host 'PulseForge generated-files maintenance'
	Write-Host ("Repository: {0}" -f $script:RepoRoot)
	Write-Host ''
	Write-Host 'Cleanup is never automatic. Only validated CMake build trees and exact legacy/cache paths are eligible.'
	Write-Host 'CMakeSettings build outputs require repository-owned Ninja caches; partial targets allow safe cleanup while preserving unrelated Out files.'
	Write-Host 'Every target is checked for tracked files, reparse points, and registered-worktree overlap before deletion.'
	Write-Host 'Cleaning Out also removes fetched dependency caches. CMakeUserPresets.json, the root CMakeSettings.json, quality tools, and quality reports are preserved.'
	Write-Host 'The legacy action is limited to explicitly allowlisted generated paths. Visual Studio cleanup preserves settings and Copilot snapshots.'
	Write-Host 'Diagnostic cleanup removes only untracked files and directories after inspecting each path; reparse links are preserved and never traversed.'
	Write-Host ''
	while ($true)
	{
		Write-Host '[1] Clean Vulkan Visual Studio CMake outputs'
		Write-Host '[2] Clean OpenGL Visual Studio CMake outputs'
		Write-Host '[3] Clean all CMake output trees (VS2022 and CMakeSettings/Ninja)'
		Write-Host '[4] Clean legacy generated outputs (root + listed vendor Bin/Int + root CMakeFiles)'
		Write-Host '[5] Clean selected Visual Studio regenerable caches'
		Write-Host '[6] Comprehensive safe cleanup (CMake outputs including Out dependencies, legacy outputs, selected VS caches)'
		Write-Host '[7] Preview comprehensive cleanup without changing files'
		Write-Host '[8] Show sizes for every supported cleanup target'
		Write-Host '[9] Clean diagnostics and integration evidence'
		Write-Host '[10] Cancel'
		$selection = Read-Host 'Select an option'
		$actionName = Resolve-MenuSelection -Selection $selection
		if ($actionName -eq 'Preview') { $null = Show-CleanupPreview -ActionName Comprehensive; continue }
		if ($actionName -eq 'Sizes') { $null = Show-SizeReport; continue }
		if ($actionName -eq 'Cancel')
		{
			Write-Host 'Cleanup canceled.'
			if ($script:HadCleanupFailure)
			{
				Write-Warning 'At least one selected cleanup had a failure or partial result; inspect the diagnostics above.'
				exit 1
			}
			return
		}
		if (-not $actionName) { Write-Host 'Choose a listed menu option.'; continue }
		$result = Invoke-CleanupAction -ActionName $actionName
		if ($result.Failed -gt 0)
		{
			$script:HadCleanupFailure = $true
			Write-Warning 'Cleanup was partial or some targets were preserved. Inspect every failed target; the operation did not fully succeed.'
		}
	}
}

if ($MyInvocation.InvocationName -ne '.')
{
	Invoke-CleanerMain
}
