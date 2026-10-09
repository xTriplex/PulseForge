[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:RepoRoot = $null
$script:HadCleanupFailure = $false
# Mutating operations may target only these exact repository-relative paths.
# Restricted cache directories are removed only after every contained file passes its extension allowlist.
$script:TargetManifest = @(
	@{ Path = 'Build'; Category = 'Build'; Kind = 'Directory'; AllowedExtensions = $null },
	@{ Path = 'out'; Category = 'Build'; Kind = 'Directory'; AllowedExtensions = $null },
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
	Build = @{ Label = 'canonical Build directory'; Paths = @('Build'); Confirm = 'DELETE BUILD' }
	Out = @{ Label = 'Visual Studio out directory'; Paths = @('out'); Confirm = 'DELETE OUT' }
	BuildOut = @{ Label = 'Build and out directories'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Build' } | ForEach-Object { $_.Path }); Confirm = 'DELETE BUILD AND OUT' }
	Legacy = @{ Label = 'legacy generated outputs'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Legacy outputs' } | ForEach-Object { $_.Path }); Confirm = 'DELETE LEGACY OUTPUTS' }
	VisualStudio = @{ Label = 'selected Visual Studio regenerable caches'; Paths = @($script:TargetManifest | Where-Object { $_.Category -eq 'Visual Studio caches' } | ForEach-Object { $_.Path }); Confirm = 'DELETE VISUAL STUDIO CACHES' }
	Comprehensive = @{ Label = 'all approved generated outputs and selected caches'; Paths = @($script:TargetManifest | ForEach-Object { $_.Path }); Confirm = 'DELETE ALL APPROVED GENERATED FILES' }
}

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
		return [pscustomobject]@{ RelativePath = $entry.Path; FullPath = $full; Exists = $false; Bytes = [long]0; Entry = $entry; Error = $null }
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
	else
	{
		$bytes = Get-TreeInspection -Path $full -Entry $entry
	}
	return [pscustomobject]@{ RelativePath = $entry.Path; FullPath = $full; Exists = $true; Bytes = $bytes; Entry = $entry; Error = $null }
}

function Format-Bytes
{
	param([long]$Bytes)
	return ('{0:N2} GiB ({1:N0} bytes)' -f ($Bytes / 1GB), $Bytes)
}

function Get-ActiveProcessWarnings
{
	$names = @('devenv', 'Code', 'codex', 'PulseForgeEditor', 'PulseForgeGame', 'cmake', 'cmake-gui', 'ninja', 'MSBuild', 'cl', 'link', 'VBCSCompiler', 'dotnet')
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
		@{ Pattern = '(?i)' + $rootPattern + '[\\/](Build|out[\\/]build)([\\/]|\b)'; Reason = 'command line references a repository build directory' }
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
	foreach ($category in @('Build', 'Legacy outputs', 'Visual Studio caches'))
	{
		if ($categoryTotals.ContainsKey($category))
		{
			Write-Host ('{0} subtotal: {1}' -f $category, (Format-Bytes $categoryTotals[$category]))
		}
	}
	Write-Host ('All allowlisted targets total: {0}' -f (Format-Bytes $total))
	return [pscustomobject]@{ Entries = @($rows.ToArray()); TotalBytes = $total; CategoryTotals = $categoryTotals }
}

function Resolve-MenuSelection
{
	param([string]$Selection)
	switch ($Selection)
	{
		'1' { return 'Build' }
		'2' { return 'Out' }
		'3' { return 'BuildOut' }
		'4' { return 'Legacy' }
		'5' { return 'VisualStudio' }
		'6' { return 'Comprehensive' }
		'7' { return 'Sizes' }
		'8' { return 'Cancel' }
		default { return $null }
	}
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
	Write-Host 'The following user data is explicitly outside the cleanup allowlist: .git, CMakeSettings.json, imgui.ini, *.vcxproj.user, all source/assets, and PulseForgeGame\.pulseforge\derived.'
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
			Remove-Item -LiteralPath $current.FullPath -Recurse -Force -ErrorAction Stop
			if (Test-Path -LiteralPath $current.FullPath) { throw 'Target still exists after deletion.' }
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
	Write-Host 'Cleanup is never automatic. Build/out cleanup removes CMake outputs and may remove fetched dependencies.'
	Write-Host 'The legacy action is limited to explicitly allowlisted generated paths. Visual Studio cleanup preserves settings and Copilot snapshots.'
	Write-Host ''
	while ($true)
	{
		Write-Host '[1] Clean canonical Build directory'
		Write-Host '[2] Clean Visual Studio out directory'
		Write-Host '[3] Deep clean Build + out'
		Write-Host '[4] Clean legacy generated outputs (root + listed vendor Bin/Int + root CMakeFiles)'
		Write-Host '[5] Clean selected Visual Studio regenerable caches'
		Write-Host '[6] Comprehensive cleanup (Build, out, legacy outputs, selected VS caches)'
		Write-Host '[7] Show sizes and totals for every supported cleanup target'
		Write-Host '[8] Cancel'
		$selection = Read-Host 'Select an option'
		$actionName = Resolve-MenuSelection -Selection $selection
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
