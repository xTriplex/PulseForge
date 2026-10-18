[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\..\CleanGeneratedFiles.ps1')

$script:Assertions = 0
$script:PromptAnswers = @()
$script:PromptIndex = 0
$script:CapturedOutput = New-Object 'System.Collections.Generic.List[string]'
$script:OutsidePaths = New-Object 'System.Collections.Generic.List[string]'
$script:CreatedJunctions = New-Object 'System.Collections.Generic.List[string]'
$script:JunctionSkipReasons = New-Object 'System.Collections.Generic.List[string]'
$script:JunctionAssertionsExecuted = 0

function Assert-True
{
	param([bool]$Condition, [string]$Message)
	$script:Assertions++
	if (-not $Condition) { throw "Assertion failed: $Message" }
}

function Assert-Throws
{
	param([scriptblock]$Action, [string]$Message)
	$script:Assertions++
	$threw = $false
	try { & $Action } catch { $threw = $true }
	if (-not $threw) { throw "Expected failure: $Message" }
}

function Read-Host
{
	param([string]$Prompt)
	if ($script:PromptIndex -ge $script:PromptAnswers.Count) { throw "Unexpected prompt: $Prompt" }
	$value = $script:PromptAnswers[$script:PromptIndex]
	$script:PromptIndex++
	return $value
}

function Reset-Prompts
{
	param([string[]]$Answers)
	$script:PromptAnswers = $Answers
	$script:PromptIndex = 0
	$script:CapturedOutput.Clear()
}

function Set-FixtureFile
{
	param([string]$Root, [string]$RelativePath, [string]$Value)
	$full = Join-Path $Root $RelativePath
	$parent = Split-Path -Parent $full
	$null = New-Item -ItemType Directory -Path $parent -Force
	Set-Content -LiteralPath $full -Value $Value
}

function Initialize-FixtureGit
{
	param([string]$Root)
	& git -C $Root init -q
	if ($LASTEXITCODE -ne 0) { throw "Could not initialize disposable fixture repository '$Root'." }
	& git -C $Root add -- CMakeLists.txt
	if ($LASTEXITCODE -ne 0) { throw "Could not stage disposable fixture marker in '$Root'." }
	& git -C $Root -c user.name='PulseForge Cleaner Tests' -c user.email='cleaner-tests@example.invalid' commit -qm 'Create cleaner fixture'
	if ($LASTEXITCODE -ne 0) { throw "Could not commit disposable fixture marker in '$Root'." }
}

function Set-FixtureCleanerRoot
{
	param([string]$Root)
	Set-CleanerRoot -Path $Root
	$script:RegisteredWorktreeProvider = $null
}

function Try-NewFixtureJunction
{
	param([string]$Path, [string]$Target)
	try
	{
		$null = New-Item -ItemType Junction -Path $Path -Target $Target -ErrorAction Stop
	}
	catch
	{
		return [pscustomobject]@{ Created = $false; Reason = $_.Exception.Message }
	}
	$item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
	if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)
	{
		throw 'Junction creation returned a non-reparse directory; test setup is invalid.'
	}
	return [pscustomobject]@{ Created = $true; Reason = $null }
}

function New-Fixture
{
	$path = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerFixture-' + [Guid]::NewGuid().ToString('N'))
	$null = New-Item -ItemType Directory -Path $path
	Set-FixtureFile $path 'CMakeLists.txt' '# synthetic fixture marker'
	Initialize-FixtureGit $path
	Set-FixtureFile $path 'CMakeSettings.json' 'must remain'
	Set-FixtureFile $path 'Out\EditorApplicationScene.patch' 'preserved recovery patch'
	Set-FixtureFile $path 'imgui.ini' 'must remain'
	Set-FixtureFile $path 'Project.vcxproj.user' 'user project settings'
	Set-FixtureFile $path 'Build\Diagnostics\preserved.txt' 'local diagnostic input'
	Set-FixtureFile $path 'Build\IntegrationEvidence\preserved.txt' 'local validation input'
	Set-FixtureFile $path 'Build\QualityTools\.venv\pyvenv.cfg' 'shared quality environment'
	Set-FixtureFile $path 'PulseForgeGame\.pulseforge\derived\cache.bin' 'must remain'
	Set-FixtureFile $path 'PulseForge\Vendor\JoltPhysics\Build\CMakeLists.txt' 'upstream build config'
	Set-FixtureFile $path 'Source.cpp' 'source must remain'
	Set-FixtureFile $path '.vs\ProjectSettings.json' 'user settings'
	Set-FixtureFile $path '.vs\CopilotSnapshots\snapshot.mpack' 'snapshot'
	Set-FixtureFile $path '.vs\PulseForge\copilot-chat\session.json' 'chat data'
	Set-FixtureFile $path '.vs\PulseForge\v17\.suo' 'user state'
	Set-FixtureFile $path '.vs\PulseForge\v17\DocumentLayout.json' 'user layout'
	Set-FixtureFile $path '.vs\PulseForge\v17\Solution.VC.db' 'not allowlisted'
	foreach ($entry in $script:TargetManifest)
	{
		$target = Join-Path $path $entry.Path
		if ($entry.Kind -eq 'CMakeBuildDirectory')
		{
			$null = New-Item -ItemType Directory -Path $target -Force
			$source = if ($entry.ContainsKey('ExpectedSourceDirectory')) { Join-Path $path $entry.ExpectedSourceDirectory } else { $path }
			$generator = if ($entry.ContainsKey('AllowedGenerators')) { $entry.AllowedGenerators[0] } else { 'Visual Studio 17 2022' }
			$cache = "CMAKE_HOME_DIRECTORY:INTERNAL=$source`r`nCMAKE_GENERATOR:INTERNAL=$generator`r`n"
			Set-Content -LiteralPath (Join-Path $target 'CMakeCache.txt') -Value $cache
			Set-FixtureFile $target 'PulseForge.sln' 'generated solution'
			Set-FixtureFile $target 'PulseForgeEditor.vcxproj' 'generated project'
			Set-FixtureFile $target 'CMakeFiles\generated.obj' 'generated object'
				Set-FixtureFile $target '_deps\pulseforgenvrhi-build\generated.lib' 'generated dependency output'
			Set-FixtureFile $target 'QualityTools\.venv\pyvenv.cfg' 'preserve local tools'
			Set-FixtureFile $target 'QualityReports\summary.csv' 'preserve prior evidence'
			Set-FixtureFile $target 'CMakeUserPresets.json' 'preserve user configuration'
		}
		elseif ($entry.Kind -eq 'CMakeSettingsOutputTree')
		{
			$buildTree = Join-Path $target 'build\x64-Debug'
			$cache = "CMAKE_HOME_DIRECTORY:INTERNAL=$path`r`nCMAKE_GENERATOR:INTERNAL=Ninja`r`n"
			Set-FixtureFile $buildTree 'CMakeCache.txt' $cache
			Set-FixtureFile $buildTree 'build.ninja' 'generated build graph'
			Set-FixtureFile $buildTree '_deps\pulseforgenvrhi-src\source.cpp' 'fetched pinned dependency source'
			Set-FixtureFile $target 'install\x64-Debug\bin\PulseForgeGame.exe' 'generated install output'
		}
		elseif ($entry.Kind -eq 'CMakeSettingsBuildDirectory')
		{
			Set-FixtureFile $target 'x64-Debug\CMakeCache.txt' "CMAKE_HOME_DIRECTORY:INTERNAL=$path`r`nCMAKE_GENERATOR:INTERNAL=Ninja`r`n"
			Set-FixtureFile $target 'x64-Debug\build.ninja' 'generated build graph'
		}
		elseif ($entry.Kind -eq 'CMakeFixtureBuildDirectory')
		{
			$source = Join-Path $path (Split-Path -Parent $entry.ExpectedSourceDirectory)
			$source = Join-Path $source (Split-Path -Leaf $entry.ExpectedSourceDirectory)
			Set-FixtureFile $source 'CMakeLists.txt' 'fixture source'
			Set-FixtureFile $source 'A.cpp' 'fixture source'
			Set-FixtureFile $source 'B.cpp' 'fixture source'
			Set-FixtureFile $source 'Core.cpp' 'fixture source'
			Set-FixtureFile $target 'CMakeCache.txt' "CMAKE_HOME_DIRECTORY:INTERNAL=$source`r`nCMAKE_GENERATOR:INTERNAL=Ninja`r`n"
			Set-FixtureFile $target 'build.ninja' 'generated fixture build graph'
		}
		elseif ($entry.Kind -eq 'File')
		{
			$null = New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force
			Set-Content -LiteralPath $target -Value 'generated fixture'
		}
		elseif ($entry.Kind -eq 'RestrictedDirectory')
		{
			$null = New-Item -ItemType Directory -Path $target -Force
			$extension = $entry.AllowedExtensions[0]
			Set-Content -LiteralPath (Join-Path $target ('fixture' + $extension)) -Value 'cache fixture'
		}
		else
		{
			$null = New-Item -ItemType Directory -Path $target -Force
			Set-FixtureFile $target 'generated.obj' 'generated fixture'
		}
	}
	return $path
}

function Assert-ProtectedFixtureData
{
	param([string]$Root)
	$paths = @(
		'CMakeSettings.json', 'imgui.ini', '.git\config', 'Project.vcxproj.user', 'Source.cpp',
		'Out\EditorApplicationScene.patch',
		'Build\QualityTools\.venv\pyvenv.cfg',
		'PulseForgeGame\.pulseforge\derived\cache.bin',
		'PulseForge\Vendor\JoltPhysics\Build\CMakeLists.txt',
		'.vs\ProjectSettings.json', '.vs\CopilotSnapshots\snapshot.mpack',
		'.vs\PulseForge\copilot-chat\session.json', '.vs\PulseForge\v17\.suo',
		'.vs\PulseForge\v17\DocumentLayout.json', '.vs\PulseForge\v17\Solution.VC.db'
	)
	foreach ($relative in $paths)
	{
		Assert-True (Test-Path -LiteralPath (Join-Path $Root $relative)) "protected path remains: $relative"
	}
}

$fixtureRoots = New-Object 'System.Collections.Generic.List[string]'
$fixtureWorktrees = New-Object 'System.Collections.Generic.List[object]'
try
{
	$root = New-Fixture
	$fixtureRoots.Add($root)
	Set-FixtureCleanerRoot $root
	Assert-True ((Resolve-MenuSelection '1') -eq 'Vulkan' -and (Resolve-MenuSelection '2') -eq 'OpenGL' -and (Resolve-MenuSelection '3') -eq 'Build') 'backend and combined build menu selections map correctly'
	Assert-True ((Resolve-MenuSelection '4') -eq 'Legacy' -and (Resolve-MenuSelection '5') -eq 'VisualStudio' -and (Resolve-MenuSelection '6') -eq 'Comprehensive') 'legacy, VS-cache, and comprehensive options map correctly'
	Assert-True ((Resolve-MenuSelection '7') -eq 'Preview' -and (Resolve-MenuSelection '8') -eq 'Sizes' -and (Resolve-MenuSelection '9') -eq 'Diagnostics' -and (Resolve-MenuSelection '10') -eq 'Cancel' -and $null -eq (Resolve-MenuSelection '11')) 'preview, size, diagnostics, cancel, and invalid selections map correctly'
	$processMetadata = @(
		[pscustomobject]@{ Name = 'cmd.exe'; ProcessId = 101; CommandLine = 'cmd.exe /c start "" PulseForgeEditor.exe' },
		[pscustomobject]@{ Name = 'powershell.exe'; ProcessId = 102; CommandLine = ('powershell.exe -Command cd ' + $root + '\Out\build\x64-Debug') },
		[pscustomobject]@{ Name = 'cmd.exe'; ProcessId = 103; CommandLine = 'cmd.exe /k' }
	)
	$launcherMatches = @(Find-PulseForgeLauncherProcesses -ProcessMetadata $processMetadata)
	Assert-True ($launcherMatches.Count -eq 2) 'known launch wrappers match process command lines without flagging every shell'
	Assert-True ($launcherMatches[0].Reason -match 'command line') 'launcher match explains it is command-line evidence only'
	Assert-Throws { Get-TargetFullPath -RelativePath '..\CMakeSettings.json' } 'path traversal is rejected'
	Assert-Throws { Get-TargetFullPath -RelativePath 'CMakeSettings.json' } 'non-allowlisted file is rejected'
	Assert-Throws { Get-TargetFullPath -RelativePath 'PulseForge\Vendor\JoltPhysics\Build' } 'Jolt upstream build path is not allowlisted'
	Assert-Throws { Get-TargetFullPath -RelativePath 'Build' } 'the whole Build tree is not allowlisted'
	Assert-True ((Get-TargetFullPath -RelativePath 'Out') -eq (Join-Path $root 'Out')) 'the exact Out target is allowlisted'
	Assert-True ((Get-TargetFullPath -RelativePath 'Out\build') -eq (Join-Path $root 'Out\build')) 'the validated CMakeSettings build subtree is allowlisted'
	Assert-True ((Get-TargetFullPath -RelativePath 'Out\QualityGraphProbe\build') -eq (Join-Path $root 'Out\QualityGraphProbe\build')) 'the exact quality-graph fixture output is allowlisted'
	Assert-Throws { Get-TargetFullPath -RelativePath 'Out\build\x64-Debug' } 'individual configuration subpaths are not independently allowlisted'

	$vulkanPath = 'Build\CMake\VS2022-x64'
	$size = Get-TargetInspection -RelativePath $vulkanPath
	Assert-True ($size.Exists -and $size.Bytes -gt 0) 'size inspection reports validated CMake outputs'
	Assert-True (@($size.GeneratedItems | Where-Object { [IO.Path]::GetFileName($_.Path) -eq 'CMakeCache.txt' }).Count -eq 1) 'CMake cache is included in the generated-output plan'
	Assert-True (@($size.PreservedItems | Where-Object { [IO.Path]::GetFileName($_) -in @('QualityTools', 'CMakeUserPresets.json') }).Count -eq 2) 'quality tools and user presets are preserved'
	Assert-True (@($size.GeneratedItems | Where-Object { [IO.Path]::GetFileName($_.Path) -eq 'QualityReports' }).Count -eq 1) 'generated quality reports are included in the cleanup plan'
	Assert-True (@($size.GeneratedItems | Where-Object { [IO.Path]::GetFileName($_.Path) -eq '_deps' }).Count -eq 1) 'dependency build outputs are removable when the cache contains no sources'
	$cachePath = Join-Path $root "$vulkanPath\CMakeCache.txt"
	$cacheBytes = (Get-Item -LiteralPath $cachePath).Length
	$preview = Show-CleanupPreview -ActionName Comprehensive
	Assert-True ($preview.TotalBytes -gt 0 -and (Get-Item -LiteralPath $cachePath).Length -eq $cacheBytes) 'preview measures without changing files'
	Assert-Throws { Get-TargetInspection -RelativePath 'Out' } 'Out with an unrelated probe sibling is preserved as a whole'
	$outBuildPlan = Get-TargetInspection -RelativePath 'Out\build'
	Assert-True ($outBuildPlan.Bytes -gt 0 -and $outBuildPlan.GeneratedItems.Count -eq 1) 'partial Out cleanup validates every repository-owned Ninja configuration'
	$qualityBuildPlan = Get-TargetInspection -RelativePath 'Out\QualityGraphProbe\build'
	Assert-True ($qualityBuildPlan.Bytes -gt 0) 'quality-graph output is checked against its exact fixture source directory'
	Assert-True (Test-Path -LiteralPath (Join-Path $root 'Out\build\x64-Debug\_deps\pulseforgenvrhi-src\source.cpp')) 'preview leaves fetched dependency sources intact'
	Assert-ProtectedFixtureData $root

	$validOutRoot = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerValidOut-' + [Guid]::NewGuid().ToString('N'))
	$null = New-Item -ItemType Directory -Path $validOutRoot
	$fixtureRoots.Add($validOutRoot)
	Set-FixtureFile $validOutRoot 'CMakeLists.txt' '# valid Out fixture marker'
	Initialize-FixtureGit $validOutRoot
	Set-FixtureFile $validOutRoot 'Out\build\x64-Debug\CMakeCache.txt' "CMAKE_HOME_DIRECTORY:INTERNAL=$validOutRoot`r`nCMAKE_GENERATOR:INTERNAL=Ninja`r`n"
	Set-FixtureFile $validOutRoot 'Out\build\x64-Debug\build.ninja' 'generated build graph'
	Set-FixtureFile $validOutRoot 'Out\build\x64-Debug\_deps\pulseforgenvrhi-src\source.cpp' 'pinned source'
	Set-FixtureFile $validOutRoot 'Out\install\x64-Debug\bin\PulseForgeGame.exe' 'install output'
	Set-FixtureCleanerRoot $validOutRoot
	$outPlan = Get-TargetInspection -RelativePath 'Out'
	Assert-True ($outPlan.Bytes -gt 0 -and $outPlan.GeneratedItems.Count -eq 2) 'conventional Out layout validates build and install trees'
	Assert-True (Test-Path -LiteralPath (Join-Path $validOutRoot 'Out\build\x64-Debug\_deps\pulseforgenvrhi-src\source.cpp')) 'Out preview retains fetched dependency source checkouts'

	$unexpectedOutRoot = New-Fixture
	$fixtureRoots.Add($unexpectedOutRoot)
	Set-FixtureCleanerRoot $unexpectedOutRoot
	Set-FixtureFile $unexpectedOutRoot 'Out\notes.txt' 'user file must not be deleted'
	Assert-Throws { Get-TargetInspection -RelativePath 'Out' } 'unexpected Out-root content rejects the whole target'
	Assert-True (Test-Path -LiteralPath (Join-Path $unexpectedOutRoot 'Out\notes.txt')) 'unexpected Out-root content remains untouched'

	$wrongGeneratorRoot = New-Fixture
	$fixtureRoots.Add($wrongGeneratorRoot)
	Set-FixtureCleanerRoot $wrongGeneratorRoot
	Set-Content -LiteralPath (Join-Path $wrongGeneratorRoot 'Out\build\x64-Debug\CMakeCache.txt') -Value ("CMAKE_HOME_DIRECTORY:INTERNAL=$wrongGeneratorRoot`r`nCMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022`r`n")
	Assert-Throws { Get-TargetInspection -RelativePath 'Out' } 'Out using an unexpected generator is preserved'
	Assert-True (Test-Path -LiteralPath (Join-Path $wrongGeneratorRoot 'Out\build\x64-Debug\CMakeCache.txt')) 'unsupported Out cache remains untouched'
	Assert-Throws { Get-TargetInspection -RelativePath 'Out\build' } 'partial Out cleanup also rejects an unsupported generator'

	$wrongFixtureSourceRoot = New-Fixture
	$fixtureRoots.Add($wrongFixtureSourceRoot)
	Set-FixtureCleanerRoot $wrongFixtureSourceRoot
$wrongFixtureCache = Join-Path $wrongFixtureSourceRoot 'Out\QualityGraphProbe\build\CMakeCache.txt'
	Set-Content -LiteralPath $wrongFixtureCache -Value "CMAKE_HOME_DIRECTORY:INTERNAL=$wrongFixtureSourceRoot`r`nCMAKE_GENERATOR:INTERNAL=Ninja`r`n"
	Assert-Throws { Get-TargetInspection -RelativePath 'Out\QualityGraphProbe\build' } 'quality fixture build from another source root is preserved'
	Assert-True (Test-Path -LiteralPath $wrongFixtureCache) 'mismatched quality fixture cache remains untouched'

	$trackedRoot = New-Fixture
	$fixtureRoots.Add($trackedRoot)
	Set-FixtureCleanerRoot $trackedRoot
$trackedRelativePath = "$vulkanPath\Debug\Authored.cpp"
	Set-FixtureFile $trackedRoot $trackedRelativePath 'tracked fixture source'
	& git -C $trackedRoot add -- $trackedRelativePath
	if ($LASTEXITCODE -ne 0) { throw 'Could not stage tracked-file protection fixture.' }
	& git -C $trackedRoot -c user.name='PulseForge Cleaner Tests' -c user.email='cleaner-tests@example.invalid' commit -qm 'Add tracked generated-path fixture'
	if ($LASTEXITCODE -ne 0) { throw 'Could not commit tracked-file protection fixture.' }
	Assert-Throws { Get-TargetInspection -RelativePath $vulkanPath } 'tracked content inside a generated target blocks cleanup'
	Assert-True (Test-Path -LiteralPath (Join-Path $trackedRoot $trackedRelativePath)) 'tracked file remains after cleanup planning rejects its target'

	Set-FixtureCleanerRoot $root

	# Registered nested worktrees block the containing build target.
	$nestedWorktree = Join-Path $root "$vulkanPath\RegisteredWorktree"
	& git -C $root worktree add --detach $nestedWorktree HEAD | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'Could not create the disposable nested-worktree fixture.' }
	$fixtureWorktrees.Add([pscustomobject]@{ Root = $root; Path = $nestedWorktree })
	Assert-Throws { Get-TargetFullPath -RelativePath $vulkanPath } 'a registered worktree inside the target is rejected'
	& git -C $root worktree remove $nestedWorktree | Out-Null
	if ($LASTEXITCODE -ne 0) { throw 'Could not normally remove the nested-worktree fixture.' }
	$fixtureWorktrees.RemoveAt($fixtureWorktrees.Count - 1)
	Assert-True (-not (Test-Path -LiteralPath $nestedWorktree)) 'normal Git removal deletes the disposable worktree fixture'
	$script:RegisteredWorktreeProvider = { throw 'simulated failure querying registered worktrees' }
	Assert-Throws { Get-TargetFullPath -RelativePath $vulkanPath } 'worktree-query failure blocks cleanup'
	$script:RegisteredWorktreeProvider = $null

	Reset-Prompts @('CLEAN SAFE TARGETS ONLY', 'DELETE CMAKE BUILD OUTPUTS')
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 5 -and $result.Failed -eq 1 -and (Test-Path (Join-Path $root $vulkanPath))) "build action cleans safe CMake targets while rejecting the unexpected Out root (succeeded=$($result.Succeeded), failed=$($result.Failed), skipped=$($result.Skipped))"
	Assert-True (-not (Test-Path -LiteralPath $cachePath)) 'CMake cache is removed last'
	Assert-True (-not (Test-Path -LiteralPath (Join-Path $root 'Out\build'))) 'validated CMakeSettings configurations are removed independently of Out user files'
	Assert-True (Test-Path -LiteralPath (Join-Path $root 'Out\QualityGraphProbe\CMakeLists.txt')) 'fixture source remains after generated build output removal'
	Assert-True (Test-Path -LiteralPath (Join-Path $root 'Build\QualityTools\.venv\pyvenv.cfg')) 'shared quality tools remain outside configuration cleanup targets'
	Assert-True (Test-Path -LiteralPath (Join-Path $root "$vulkanPath\QualityTools\.venv\pyvenv.cfg")) 'quality tool environment survives'
	Assert-True (-not (Test-Path -LiteralPath (Join-Path $root "$vulkanPath\QualityReports\summary.csv"))) 'generated quality reports are removed'
	Assert-True (Test-Path -LiteralPath (Join-Path $root "$vulkanPath\CMakeUserPresets.json")) 'local CMake presets survive'
	Assert-ProtectedFixtureData $root

	$cancelRoot = New-Fixture
	$fixtureRoots.Add($cancelRoot)
	Set-FixtureCleanerRoot $cancelRoot
	Reset-Prompts @('not the confirmation')
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 0 -and (Test-Path (Join-Path $cancelRoot "$vulkanPath\CMakeCache.txt"))) 'incorrect confirmation cancels without deleting'

	$dependencyRoot = New-Fixture
	$fixtureRoots.Add($dependencyRoot)
	Set-FixtureCleanerRoot $dependencyRoot
	Set-FixtureFile $dependencyRoot "$vulkanPath\_deps\pulseforgenvrhi-src\CMakeLists.txt" 'pinned source'
	$dependencyPlan = Get-TargetInspection -RelativePath $vulkanPath
	Assert-True (@($dependencyPlan.PreservedItems | Where-Object { [IO.Path]::GetFileName($_) -eq '_deps' }).Count -eq 1) 'a cache containing source checkouts is preserved whole'
	Reset-Prompts @('DELETE VULKAN BUILD OUTPUTS')
	$result = Invoke-CleanupAction -ActionName Vulkan
	Assert-True ($result.Succeeded -eq 1 -and (Test-Path (Join-Path $dependencyRoot "$vulkanPath\_deps\pulseforgenvrhi-src\CMakeLists.txt"))) 'Visual Studio cleanup preserves its cached dependency source checkout'

	$badCacheRoot = New-Fixture
	$fixtureRoots.Add($badCacheRoot)
	Set-FixtureCleanerRoot $badCacheRoot
	$badCachePath = Join-Path $badCacheRoot "$vulkanPath\CMakeCache.txt"
	Set-Content -LiteralPath $badCachePath -Value ("CMAKE_HOME_DIRECTORY:INTERNAL=C:\elsewhere" + [Environment]::NewLine + 'CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022')
	Assert-Throws { Get-TargetInspection -RelativePath $vulkanPath } 'build tree belonging to another source directory is rejected'

	Set-FixtureCleanerRoot $root
	Reset-Prompts @('DELETE LEGACY OUTPUTS')
	$result = Invoke-CleanupAction -ActionName Legacy
	Assert-True ($result.Succeeded -eq 14) 'legacy action removes every exact legacy and completed-probe allowlist entry'
	Assert-True (Test-Path (Join-Path $root 'Build')) 'legacy action preserves Build'
	Assert-ProtectedFixtureData $root

	Set-FixtureCleanerRoot $root
	Reset-Prompts @('DELETE VISUAL STUDIO CACHES')
	$result = Invoke-CleanupAction -ActionName VisualStudio
	Assert-True ($result.Succeeded -eq 5) 'Visual Studio action removes its five allowlisted targets'
	Assert-ProtectedFixtureData $root
	Assert-True (-not (Test-Path (Join-Path $root '.vs\PulseForge\v17\ipch'))) 'PCH cache removed'
	Assert-True (-not (Test-Path (Join-Path $root '.vs\PulseForge\FileContentIndex'))) 'VS index cache removed'

	# An unexpected file inside a restricted cache is preserved while other safe targets continue.
	$unsafeRoot = New-Fixture
	$fixtureRoots.Add($unsafeRoot)
		Set-FixtureCleanerRoot $unsafeRoot
	Set-Content -LiteralPath (Join-Path $unsafeRoot '.vs\PulseForge\FileContentIndex\settings.json') -Value 'must not be deleted'
	Reset-Prompts @('CLEAN SAFE TARGETS ONLY', 'DELETE VISUAL STUDIO CACHES')
	$result = Invoke-CleanupAction -ActionName VisualStudio
	Assert-True ($result.Succeeded -eq 4 -and $result.Failed -eq 1) 'unsafe target is preserved and safe targets still clean'
	Assert-True (Test-Path (Join-Path $unsafeRoot '.vs\PulseForge\FileContentIndex\settings.json')) 'unexpected restricted-cache content remains'
	Assert-True (-not (Test-Path (Join-Path $unsafeRoot '.vs\PulseForge\v17\Browse.VC.db'))) 'other safe target cleaned after partial inspection failure'

	# An exclusive open handle simulates a Windows lock and verifies per-target continuation.
	$lockedRoot = New-Fixture
	$fixtureRoots.Add($lockedRoot)
		Set-FixtureCleanerRoot $lockedRoot
	$lockedPath = Join-Path $lockedRoot '.vs\PulseForge\v17\Browse.VC.db'
	$lock = New-Object IO.FileStream($lockedPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
	try
	{
		Reset-Prompts @('DELETE VISUAL STUDIO CACHES')
		$result = Invoke-CleanupAction -ActionName VisualStudio
		Assert-True ($result.Failed -eq 1 -and $result.Succeeded -eq 4) 'locked file failure is isolated while other cache targets clean'
	}
	finally { $lock.Dispose() }
	Assert-True (Test-Path -LiteralPath $lockedPath) 'locked database remains after failed deletion'

	# A junction in an exact allowlisted location is rejected without traversing its destination.
	$linkRoot = New-Fixture
	$fixtureRoots.Add($linkRoot)
		Set-FixtureCleanerRoot $linkRoot
	$outside = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerOutside-' + [Guid]::NewGuid().ToString('N'))
	$script:OutsidePaths.Add($outside)
	Set-FixtureFile $outside 'must-survive.txt' 'outside allowlisted tree'
	$noToolsRoot = New-Fixture
	$fixtureRoots.Add($noToolsRoot)
	Set-FixtureCleanerRoot $noToolsRoot
	$noToolsLink = Join-Path $noToolsRoot 'Build\NoToolsProbeBuild\_tools'
	$noToolsLinkResult = Try-NewFixtureJunction -Path $noToolsLink -Target $outside
	if ($noToolsLinkResult.Created)
	{
		$script:CreatedJunctions.Add($noToolsLink)
		$script:JunctionAssertionsExecuted++
		$noToolsPlan = Get-TargetInspection -RelativePath 'Build\NoToolsProbeBuild'
		Assert-True (@($noToolsPlan.PreservedItems | Where-Object { [IO.Path]::GetFileName($_) -eq '_tools' }).Count -eq 1) 'known probe tool link is preserved while its cache ownership is validated'
		Remove-InspectedTarget -Inspection $noToolsPlan
		Assert-True (-not (Test-Path -LiteralPath (Join-Path $noToolsRoot 'Build\NoToolsProbeBuild\CMakeCache.txt'))) 'probe CMake cache is removed'
		Assert-True (Test-Path -LiteralPath $noToolsLink) 'probe DXC link remains untraversed'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'probe link destination remains intact'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('probe tool junction test: ' + $noToolsLinkResult.Reason))
	}
	Set-FixtureCleanerRoot $linkRoot
		$topJunction = Join-Path $linkRoot $vulkanPath
		[IO.Directory]::Delete($topJunction, $true)
	$topLinkResult = Try-NewFixtureJunction -Path $topJunction -Target $outside
	if ($topLinkResult.Created)
	{
		$script:CreatedJunctions.Add($topJunction)
		$script:JunctionAssertionsExecuted++
				Assert-Throws { Get-TargetInspection -RelativePath $vulkanPath } 'junction target is rejected'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'junction destination remains untouched'
		[IO.Directory]::Delete($topJunction, $false)
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'removing the fixture junction does not remove its destination'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('top-level junction test: ' + $topLinkResult.Reason))
	}

	# Also verify traversal catches a junction nested below an otherwise valid allowlisted target.
		$nestedRoot = New-Fixture
	$fixtureRoots.Add($nestedRoot)
		Set-FixtureCleanerRoot $nestedRoot
		$nestedJunction = Join-Path $nestedRoot "$vulkanPath\CMakeFiles\external-link"
	$nestedLinkResult = Try-NewFixtureJunction -Path $nestedJunction -Target $outside
	if ($nestedLinkResult.Created)
	{
		$script:CreatedJunctions.Add($nestedJunction)
		$script:JunctionAssertionsExecuted++
				Assert-Throws { Get-TargetInspection -RelativePath $vulkanPath } 'nested junction in CMake output is rejected'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'nested junction destination remains intact'
		[IO.Directory]::Delete($nestedJunction, $false)
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'removing nested fixture junction preserves its destination'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('nested junction test: ' + $nestedLinkResult.Reason))
	}
	$outJunction = Join-Path $nestedRoot 'Out\build\x64-Debug\_deps\external-cache'
	$outLinkResult = Try-NewFixtureJunction -Path $outJunction -Target $outside
	if ($outLinkResult.Created)
	{
		$script:CreatedJunctions.Add($outJunction)
		$script:JunctionAssertionsExecuted++
		Assert-Throws { Get-TargetInspection -RelativePath 'Out' } 'junction inside Out dependency cache is rejected'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'Out junction destination remains intact'
		[IO.Directory]::Delete($outJunction, $false)
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'removing the Out fixture junction preserves its destination'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('Out nested junction test: ' + $outLinkResult.Reason))
	}

	# Diagnostic cleanup removes safe siblings while leaving a linked external target untouched.
	$generatedRoot = New-Fixture
	$fixtureRoots.Add($generatedRoot)
	Set-FixtureCleanerRoot $generatedRoot
	$diagnosticLink = Join-Path $generatedRoot 'Build\Diagnostics\nested\external-tools'
	Set-FixtureFile $generatedRoot 'Build\Diagnostics\nested\safe-output.bin' 'safe generated output'
	$diagnosticLinkResult = Try-NewFixtureJunction -Path $diagnosticLink -Target $outside
	if ($diagnosticLinkResult.Created)
	{
		$script:CreatedJunctions.Add($diagnosticLink)
		$script:JunctionAssertionsExecuted++
		$diagnosticsPlan = Get-TargetInspection -RelativePath 'Build\Diagnostics'
		Assert-True ($diagnosticsPlan.GeneratedItems.Count -eq 3 -and $diagnosticsPlan.PreservedItems.Count -eq 1) 'diagnostic cleanup plans safe siblings inside a linked subtree'
		Reset-Prompts @('DELETE DIAGNOSTICS AND EVIDENCE')
		$result = Invoke-CleanupAction -ActionName Diagnostics
		Assert-True ($result.Succeeded -eq 10 -and $result.Failed -eq 0) 'diagnostic action cleans exact evidence targets'
		Assert-True (-not (Test-Path -LiteralPath (Join-Path $generatedRoot 'Build\Diagnostics\generated.obj'))) 'safe diagnostic output is removed'
		Assert-True (-not (Test-Path -LiteralPath (Join-Path $generatedRoot 'Build\Diagnostics\nested\safe-output.bin'))) 'safe output beside a nested link is removed'
		Assert-True (Test-Path -LiteralPath $diagnosticLink) 'linked external tool path itself is retained'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'diagnostic junction target remains untouched'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('diagnostic junction test: ' + $diagnosticLinkResult.Reason))
	}

	# Verify complete action on a fresh synthetic tree, then verify user data remains.
	$allRoot = New-Fixture
	$fixtureRoots.Add($allRoot)
		Set-FixtureCleanerRoot $allRoot
	Reset-Prompts @('CLEAN SAFE TARGETS ONLY', 'DELETE ALL APPROVED GENERATED FILES INCLUDING OUT DEPENDENCIES')
	$result = Invoke-CleanupAction -ActionName Comprehensive
	Assert-True ($result.Succeeded -eq ($script:TargetManifest.Count - 1) -and $result.Failed -eq 1) 'comprehensive action removes safe fixture targets and preserves Out with unrelated fixture content'
	Assert-True (-not (Test-Path -LiteralPath (Join-Path $allRoot 'Build\Diagnostics\preserved.txt'))) 'comprehensive cleanup removes diagnostics and evidence output'
	Assert-True (-not (Test-Path -LiteralPath (Join-Path $allRoot 'Build\IntegrationEvidence\preserved.txt'))) 'comprehensive cleanup removes integration evidence output'
	Assert-ProtectedFixtureData $allRoot

	# Missing targets are harmless and no confirmation is requested when nothing is present.
	$emptyRoot = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerEmpty-' + [Guid]::NewGuid().ToString('N'))
	$null = New-Item -ItemType Directory -Path $emptyRoot
	$fixtureRoots.Add($emptyRoot)
	Set-FixtureFile $emptyRoot 'CMakeLists.txt' '# empty fixture'
		Initialize-FixtureGit $emptyRoot
		Set-FixtureCleanerRoot $emptyRoot
	Reset-Prompts @()
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 0 -and $result.Failed -eq 0 -and $result.Skipped -eq 6) 'missing configurations and probe outputs are skipped without unsafe-target failures'

	# Exercise the full size report against a fixture and check aggregate output is emitted.
		$reportRoot = New-Fixture
		$fixtureRoots.Add($reportRoot)
		Set-FixtureCleanerRoot $reportRoot
	Reset-Prompts @()
	$report = Show-SizeReport
	Assert-True ($report.TotalBytes -gt 0) 'size report returns aggregate total'
		Assert-True (@($report.Entries | Where-Object { $_.RelativePath -eq $vulkanPath }).Count -eq 1) 'size report includes validated CMake targets'

	if ($script:JunctionAssertionsExecuted -eq 0 -and $script:JunctionSkipReasons.Count -eq 0)
	{
		throw 'Junction tests neither executed nor reported an explicit skip.'
	}
	Write-Output ("PASS: {0} cleanup safety assertions; junction assertions executed: {1}." -f $script:Assertions, ($script:JunctionAssertionsExecuted * 3))
	foreach ($reason in $script:JunctionSkipReasons) { Write-Output ('SKIP: ' + $reason) }
}
finally
{
	foreach ($worktree in $fixtureWorktrees)
	{
		try { & git -C $worktree.Root worktree remove $worktree.Path | Out-Null } catch { }
	}
	foreach ($junction in $script:CreatedJunctions)
	{
		try { [IO.Directory]::Delete($junction, $false) } catch { }
	}
	foreach ($fixture in $fixtureRoots)
	{
		if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force -ErrorAction SilentlyContinue }
	}
	foreach ($outside in $script:OutsidePaths)
	{
		if (Test-Path -LiteralPath $outside) { Remove-Item -LiteralPath $outside -Recurse -Force -ErrorAction SilentlyContinue }
	}
}
