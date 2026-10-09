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
	Set-FixtureFile $path 'CMakeSettings.json' 'must remain'
	Set-FixtureFile $path 'imgui.ini' 'must remain'
	Set-FixtureFile $path '.git\config' 'git metadata'
	Set-FixtureFile $path 'Project.vcxproj.user' 'user project settings'
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
		if ($entry.Kind -eq 'File')
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
try
{
	$root = New-Fixture
	$fixtureRoots.Add($root)
	Set-CleanerRoot -Path $root
	Assert-True ((Resolve-MenuSelection '1') -eq 'Build' -and (Resolve-MenuSelection '2') -eq 'Out' -and (Resolve-MenuSelection '3') -eq 'BuildOut') 'legacy menu selections remain mapped'
	Assert-True ((Resolve-MenuSelection '4') -eq 'Legacy' -and (Resolve-MenuSelection '5') -eq 'VisualStudio' -and (Resolve-MenuSelection '6') -eq 'Comprehensive') 'new cleanup menu selections map to their actions'
	Assert-True ((Resolve-MenuSelection '7') -eq 'Sizes' -and (Resolve-MenuSelection '8') -eq 'Cancel' -and $null -eq (Resolve-MenuSelection '9')) 'size, cancel, and invalid menu options are handled'
	$processMetadata = @(
		[pscustomobject]@{ Name = 'cmd.exe'; ProcessId = 101; CommandLine = 'cmd.exe /c start "" PulseForgeEditor.exe' },
		[pscustomobject]@{ Name = 'powershell.exe'; ProcessId = 102; CommandLine = ('powershell.exe -Command cd ' + $root + '\out\build\x64-Debug') },
		[pscustomobject]@{ Name = 'cmd.exe'; ProcessId = 103; CommandLine = 'cmd.exe /k' }
	)
	$launcherMatches = @(Find-PulseForgeLauncherProcesses -ProcessMetadata $processMetadata)
	Assert-True ($launcherMatches.Count -eq 2) 'known launch wrappers match process command lines without flagging every shell'
	Assert-True ($launcherMatches[0].Reason -match 'command line') 'launcher match explains it is command-line evidence only'
	Assert-Throws { Get-TargetFullPath -RelativePath '..\CMakeSettings.json' } 'path traversal is rejected'
	Assert-Throws { Get-TargetFullPath -RelativePath 'CMakeSettings.json' } 'non-allowlisted file is rejected'
	Assert-Throws { Get-TargetFullPath -RelativePath 'PulseForge\Vendor\JoltPhysics\Build' } 'Jolt upstream build path is not allowlisted'

	$size = Get-TargetInspection -RelativePath 'Build'
	Assert-True ($size.Exists -and $size.Bytes -gt 0) 'size inspection reports fixture bytes'

	Reset-Prompts @('DELETE BUILD')
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 1 -and -not (Test-Path (Join-Path $root 'Build'))) 'Build-only action removes only Build'
	Assert-True (Test-Path (Join-Path $root 'out')) 'Build-only preserves out'
	Assert-ProtectedFixtureData $root
	Set-FixtureFile $root 'Build\generated.obj' 'generated fixture'
	Reset-Prompts @('not the confirmation')
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 0 -and (Test-Path (Join-Path $root 'Build'))) 'incorrect confirmation cancels without deleting'

	Reset-Prompts @('DELETE OUT')
	$result = Invoke-CleanupAction -ActionName Out
	Assert-True ($result.Succeeded -eq 1 -and -not (Test-Path (Join-Path $root 'out'))) 'out-only action removes only out'
	Assert-True (Test-Path (Join-Path $root 'Build')) 'out-only preserves Build'
	Set-FixtureFile $root 'out\generated.obj' 'generated fixture'

	Reset-Prompts @('DELETE BUILD AND OUT')
	$result = Invoke-CleanupAction -ActionName BuildOut
	Assert-True ($result.Succeeded -eq 2 -and -not (Test-Path (Join-Path $root 'Build')) -and -not (Test-Path (Join-Path $root 'out'))) 'combined action removes Build and out'
	Set-FixtureFile $root 'Build\generated.obj' 'generated fixture'
	Set-FixtureFile $root 'out\generated.obj' 'generated fixture'

	Reset-Prompts @('DELETE LEGACY OUTPUTS')
	$result = Invoke-CleanupAction -ActionName Legacy
	Assert-True ($result.Succeeded -eq 13) 'legacy action removes every exact legacy allowlist entry'
	Assert-True (Test-Path (Join-Path $root 'Build')) 'legacy action preserves Build'
	Assert-ProtectedFixtureData $root

	Reset-Prompts @('DELETE VISUAL STUDIO CACHES')
	$result = Invoke-CleanupAction -ActionName VisualStudio
	Assert-True ($result.Succeeded -eq 5) 'Visual Studio action removes its five allowlisted targets'
	Assert-ProtectedFixtureData $root
	Assert-True (-not (Test-Path (Join-Path $root '.vs\PulseForge\v17\ipch'))) 'PCH cache removed'
	Assert-True (-not (Test-Path (Join-Path $root '.vs\PulseForge\FileContentIndex'))) 'VS index cache removed'

	# An unexpected file inside a restricted cache is preserved while other safe targets continue.
	$unsafeRoot = New-Fixture
	$fixtureRoots.Add($unsafeRoot)
	Set-CleanerRoot -Path $unsafeRoot
	Set-Content -LiteralPath (Join-Path $unsafeRoot '.vs\PulseForge\FileContentIndex\settings.json') -Value 'must not be deleted'
	Reset-Prompts @('CLEAN SAFE TARGETS ONLY', 'DELETE VISUAL STUDIO CACHES')
	$result = Invoke-CleanupAction -ActionName VisualStudio
	Assert-True ($result.Succeeded -eq 4 -and $result.Failed -eq 1) 'unsafe target is preserved and safe targets still clean'
	Assert-True (Test-Path (Join-Path $unsafeRoot '.vs\PulseForge\FileContentIndex\settings.json')) 'unexpected restricted-cache content remains'
	Assert-True (-not (Test-Path (Join-Path $unsafeRoot '.vs\PulseForge\v17\Browse.VC.db'))) 'other safe target cleaned after partial inspection failure'

	# An exclusive open handle simulates a Windows lock and verifies per-target continuation.
	$lockedRoot = New-Fixture
	$fixtureRoots.Add($lockedRoot)
	Set-CleanerRoot -Path $lockedRoot
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
	Set-CleanerRoot -Path $linkRoot
	$outside = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerOutside-' + [Guid]::NewGuid().ToString('N'))
	$script:OutsidePaths.Add($outside)
	Set-FixtureFile $outside 'must-survive.txt' 'outside allowlisted tree'
	$topJunction = Join-Path $linkRoot 'Build'
	Remove-Item -LiteralPath $topJunction -Recurse -Force
	$topLinkResult = Try-NewFixtureJunction -Path $topJunction -Target $outside
	if ($topLinkResult.Created)
	{
		$script:CreatedJunctions.Add($topJunction)
		$script:JunctionAssertionsExecuted++
		Assert-Throws { Get-TargetInspection -RelativePath 'Build' } 'junction target is rejected'
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
	Set-CleanerRoot -Path $nestedRoot
	$nestedJunction = Join-Path $nestedRoot 'Build\external-link'
	$nestedLinkResult = Try-NewFixtureJunction -Path $nestedJunction -Target $outside
	if ($nestedLinkResult.Created)
	{
		$script:CreatedJunctions.Add($nestedJunction)
		$script:JunctionAssertionsExecuted++
		Assert-Throws { Get-TargetInspection -RelativePath 'Build' } 'nested junction in allowlisted directory is rejected'
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'nested junction destination remains intact'
		[IO.Directory]::Delete($nestedJunction, $false)
		Assert-True (Test-Path -LiteralPath (Join-Path $outside 'must-survive.txt')) 'removing nested fixture junction preserves its destination'
	}
	else
	{
		$script:JunctionSkipReasons.Add(('nested junction test: ' + $nestedLinkResult.Reason))
	}

	# Verify complete action on a fresh synthetic tree, then verify user data remains.
	$allRoot = New-Fixture
	$fixtureRoots.Add($allRoot)
	Set-CleanerRoot -Path $allRoot
	Reset-Prompts @('DELETE ALL APPROVED GENERATED FILES')
	$result = Invoke-CleanupAction -ActionName Comprehensive
	Assert-True ($result.Succeeded -eq $script:TargetManifest.Count) 'comprehensive action removes all present allowlisted fixture targets'
	Assert-ProtectedFixtureData $allRoot

	# Missing targets are harmless and no confirmation is requested when nothing is present.
	$emptyRoot = Join-Path ([IO.Path]::GetTempPath()) ('PulseForgeCleanerEmpty-' + [Guid]::NewGuid().ToString('N'))
	$null = New-Item -ItemType Directory -Path $emptyRoot
	$fixtureRoots.Add($emptyRoot)
	Set-FixtureFile $emptyRoot 'CMakeLists.txt' '# empty fixture'
	Set-CleanerRoot -Path $emptyRoot
	Reset-Prompts @()
	$result = Invoke-CleanupAction -ActionName Build
	Assert-True ($result.Succeeded -eq 0 -and $result.Skipped -eq 1) 'missing directory is reported and not an error'

	# Exercise the full size report against a fixture and check aggregate output is emitted.
	Set-CleanerRoot -Path $allRoot
	Set-FixtureFile $allRoot 'CMakeFiles\report-fixture.obj' 'measurement fixture'
	Reset-Prompts @()
	$report = Show-SizeReport
	Assert-True ($report.TotalBytes -gt 0) 'size report returns aggregate total'
	Assert-True (@($report.Entries | Where-Object { $_.RelativePath -eq 'CMakeFiles' }).Count -eq 1) 'size report includes legacy target paths'

	if ($script:JunctionAssertionsExecuted -eq 0 -and $script:JunctionSkipReasons.Count -eq 0)
	{
		throw 'Junction tests neither executed nor reported an explicit skip.'
	}
	Write-Output ("PASS: {0} cleanup safety assertions; junction assertions executed: {1}." -f $script:Assertions, ($script:JunctionAssertionsExecuted * 3))
	foreach ($reason in $script:JunctionSkipReasons) { Write-Output ('SKIP: ' + $reason) }
}
finally
{
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
