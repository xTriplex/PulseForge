[CmdletBinding()]
param(
	[string]$BaseSha,
	[ValidateSet('full', 'automation-probe-only', 'macos-desktop-only', 'windows-asset-diagnostic-only')]
	[string]$Selection = 'full',
	[string]$RemoteRef = 'refs/heads/main',
	[string]$WorkflowRef = 'main',
	[string]$RepositoryRoot,
	[string]$OutputJsonPath,
	[switch]$Dispatch,
	[switch]$Help,
	[string[]]$IncludePath = @(),
	[string]$IncludePathFile
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Show-Usage
{
	@'
PreparePatchValidation.ps1 captures selected working-tree paths into a bounded request for
.github/workflows/portable-editor-ci.yml. It uses a temporary Git index and does not modify
or stage the user's real index. Staged mode/content is used as the seed, then the working tree
is overlaid so unstaged edits are included. Explicit paths determine what is sent.

Example:
  pwsh -NoProfile -File Tests/Platform/PreparePatchValidation.ps1 `
    -BaseSha <remote-commit-sha> -RemoteRef refs/heads/main `
    -IncludePath Tests/OneFocusedTest.cpp,Tests/RelatedFixture.txt `
    -Selection windows-asset-diagnostic-only -OutputJsonPath "$env:TEMP/pf-patch-request.json"

Add -Dispatch to send that JSON to GitHub CLI's workflow_dispatch stdin interface. No patch
content or credential is placed in command text. This requires an already authenticated `gh`
CLI and does not extract or print its credentials.

Limits: <= 100 explicit include paths; decoded patch <= 2 MiB; gzip <= 30000 bytes;
base64 field <= 40000 characters; resulting patch <= 200 changed paths and 20000 path bytes.
Oversized or unsupported patches are rejected; they are never committed or uploaded elsewhere.
Patching workflow YAML after dispatch does not change the workflow definition already running.
Workflow-graph changes must be committed and validated by the workflow definition from that commit.
An experimental patched-source run validates its recorded result tree, not its base commit.
'@ | Write-Output
}

function Invoke-Git
{
	param([string[]]$Arguments, [switch]$AllowFailure)
	$output = & git @Arguments 2>&1
	$exitCode = $LASTEXITCODE
	if (-not $AllowFailure -and $exitCode -ne 0)
	{
		throw "git $($Arguments -join ' ') failed ($exitCode): $($output -join [Environment]::NewLine)"
	}
	return [pscustomobject]@{ ExitCode = $exitCode; Output = @($output) }
}

function Get-IndexPath
{
	param([string]$Root)
	$pathResult = Invoke-Git -Arguments @('-C', $Root, 'rev-parse', '--git-path', 'index')
	$path = [string]$pathResult.Output[-1]
	if (-not [IO.Path]::IsPathRooted($path)) { $path = Join-Path $Root $path }
	return [IO.Path]::GetFullPath($path)
}

function Get-FileFingerprint
{
	param([string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '<absent>' }
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Normalize-IncludedPath
{
	param([string]$Path)
	if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Included paths cannot be empty.' }
	$value = $Path.Replace('\', '/')
	if ([IO.Path]::IsPathRooted($Path) -or $value.StartsWith('/') -or $value -match '^[A-Za-z]:')
	{
		throw "Included path must be repository-relative: $Path"
	}
	$value = $value.TrimEnd('/')
	$parts = $value.Split('/')
	if (-not $value -or $parts -contains '' -or $parts -contains '.' -or $parts -contains '..' -or
		$parts -contains '.git' -or $value -match '[\r\n\t]')
	{
		throw "Included path is not a supported repository path: $Path"
	}
	if ($value -eq '.github/workflows' -or $value.StartsWith('.github/workflows/', [StringComparison]::OrdinalIgnoreCase))
	{
		throw 'Workflow YAML cannot be validated through a patch dispatch; commit the workflow change and run that definition.'
	}
	return $value
}

function Send-WorkflowRequest
{
	param([string]$RequestPath, [string]$Ref, [string]$Root)
	$gh = Get-Command gh -CommandType Application -ErrorAction SilentlyContinue
	if (-not $gh) { throw 'GitHub CLI (gh) is not installed. Keep the JSON request and dispatch it through an authorized GitHub interface.' }
	$processInfo = [Diagnostics.ProcessStartInfo]::new()
	$processInfo.FileName = $gh.Source
	$processInfo.UseShellExecute = $false
	$processInfo.WorkingDirectory = $Root
	$processInfo.RedirectStandardInput = $true
	$processInfo.RedirectStandardOutput = $true
	$processInfo.RedirectStandardError = $true
	foreach ($argument in @(
		'workflow', 'run', '.github/workflows/portable-editor-ci.yml', '--ref', $Ref, '--json'))
	{
		$processInfo.ArgumentList.Add($argument)
	}
	$process = [Diagnostics.Process]::new()
	$process.StartInfo = $processInfo
	try
	{
		if (-not $process.Start()) { throw 'Could not start GitHub CLI workflow dispatch.' }
		$stdoutTask = $process.StandardOutput.ReadToEndAsync()
		$stderrTask = $process.StandardError.ReadToEndAsync()
		$requestBytes = [IO.File]::ReadAllBytes($RequestPath)
		$process.StandardInput.BaseStream.Write($requestBytes, 0, $requestBytes.Length)
		$process.StandardInput.Close()
		if (-not $process.WaitForExit(120000))
		{
			$process.Kill()
			if (-not $process.WaitForExit(5000))
			{
				throw 'The owned GitHub CLI dispatch process did not exit after cancellation.'
			}
			throw 'GitHub CLI workflow dispatch did not finish within 120 seconds; only its owned process was stopped.'
		}
		$stdout = $stdoutTask.GetAwaiter().GetResult()
		$stderr = $stderrTask.GetAwaiter().GetResult()
		if ($stdout) { Write-Output $stdout.TrimEnd() }
		if ($stderr) { [Console]::Error.WriteLine($stderr.TrimEnd()) }
		if ($process.ExitCode -ne 0) { throw "GitHub workflow dispatch failed with exit code $($process.ExitCode)." }
	}
	finally
	{
		if (-not $process.HasExited)
		{
			try { $process.Kill() } catch [InvalidOperationException] { }
			if (-not $process.WaitForExit(5000))
			{
				throw 'The owned GitHub CLI dispatch process did not exit during cleanup.'
			}
		}
		$process.Dispose()
	}
}

if ($Help)
{
	Show-Usage
	exit 0
}

try
{
	if (-not $RepositoryRoot)
	{
		$rootResult = Invoke-Git -Arguments @('rev-parse', '--show-toplevel')
		$RepositoryRoot = [string]$rootResult.Output[-1]
	}
	$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
	if ($IncludePathFile)
	{
		if ($IncludePath.Count -gt 0) { throw 'Use either -IncludePath or -IncludePathFile, not both.' }
		$IncludePath = @(Get-Content -LiteralPath $IncludePathFile -Raw -Encoding utf8 | ConvertFrom-Json)
	}
	if (-not $BaseSha -or $BaseSha -notmatch '^[0-9a-fA-F]{40}$')
	{
		throw 'BaseSha must be an existing remote 40-character commit SHA.'
	}
	if ($IncludePath.Count -lt 1) { throw 'At least one explicit -IncludePath or -IncludePathFile is required.' }
	if ($IncludePath.Count -gt 100) { throw 'At most 100 included paths are supported per patch request.' }
	$normalizedPaths = @($IncludePath | ForEach-Object { Normalize-IncludedPath $_ })
	$caseAliasMap = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach ($path in $normalizedPaths)
	{
		$existingPath = $null
		if ($caseAliasMap.TryGetValue($path, [ref]$existingPath) -and $existingPath -cne $path)
		{
			throw "Included paths contain case-equivalent aliases: $existingPath and $path"
		}
		$caseAliasMap[$path] = $path
	}
	$normalizedPaths = @($normalizedPaths | Sort-Object -Unique)
	$totalPathBytes = [Text.Encoding]::UTF8.GetByteCount(($normalizedPaths -join "`0"))
	if ($totalPathBytes -gt 20000) { throw 'Included path list exceeds the 20000-byte bound.' }
	$remoteFormat = Invoke-Git -Arguments @('check-ref-format', $RemoteRef) -AllowFailure
	if ($remoteFormat.ExitCode -ne 0) { throw "RemoteRef is not a valid Git ref: $RemoteRef" }
	$workflowFormat = Invoke-Git -Arguments @('check-ref-format', '--branch', $WorkflowRef) -AllowFailure
	if ($workflowFormat.ExitCode -ne 0) { throw "WorkflowRef is not a valid Git ref: $WorkflowRef" }

	Invoke-Git -Arguments @('-C', $RepositoryRoot, 'fetch', '--no-tags', 'origin', $RemoteRef) | Out-Null
	Invoke-Git -Arguments @('-C', $RepositoryRoot, 'cat-file', '-e', "$BaseSha^{commit}") | Out-Null
	$ancestor = Invoke-Git -Arguments @('-C', $RepositoryRoot, 'merge-base', '--is-ancestor', $BaseSha, 'FETCH_HEAD') -AllowFailure
	if ($ancestor.ExitCode -ne 0) { throw "Base SHA $BaseSha is not reachable from fetched remote ref $RemoteRef." }
	$indexPath = Get-IndexPath -Root $RepositoryRoot
	$indexFingerprintBefore = Get-FileFingerprint -Path $indexPath
	$tempIndex = Join-Path ([IO.Path]::GetTempPath()) ("pulseforge-patch-index-{0}.idx" -f [guid]::NewGuid().ToString('N'))
	$patchPath = Join-Path ([IO.Path]::GetTempPath()) ("pulseforge-patch-{0}.diff" -f [guid]::NewGuid().ToString('N'))
	if (-not $OutputJsonPath)
	{
		$OutputJsonPath = Join-Path ([IO.Path]::GetTempPath()) ("pulseforge-patch-request-{0}.json" -f [guid]::NewGuid().ToString('N'))
	}
	$OutputJsonPath = [IO.Path]::GetFullPath($OutputJsonPath)
	if (Test-Path -LiteralPath $OutputJsonPath) { throw "Refusing to overwrite an existing patch request: $OutputJsonPath" }
	$previousIndexEnv = $env:GIT_INDEX_FILE
	try
	{
		$env:GIT_INDEX_FILE = $tempIndex
		Invoke-Git -Arguments @('-C', $RepositoryRoot, 'read-tree', $BaseSha) | Out-Null
		if ($previousIndexEnv) { $env:GIT_INDEX_FILE = $previousIndexEnv } else { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue }

		$lsFilesArgs = @('-C', $RepositoryRoot, 'ls-files', '--stage', '-z', '--') + $normalizedPaths
		$realIndexEntriesResult = Invoke-Git -Arguments $lsFilesArgs
		$realIndexEntries = ($realIndexEntriesResult.Output -join '')
		foreach ($record in $realIndexEntries.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries))
		{
			$tab = $record.IndexOf("`t")
			if ($tab -lt 0) { throw 'Could not parse the real index entry without modifying it.' }
			$metadata = $record.Substring(0, $tab).Split(' ')
			if ($metadata.Count -lt 3) { throw 'Could not parse the real index mode and object identity.' }
			if ($metadata[2] -ne '0') { throw "Included path has unresolved index stages: $($record.Substring($tab + 1))" }
			$path = $record.Substring($tab + 1)
			if ($path -match '[\r\n\t]') { throw 'Newline/tab pathnames are unsupported by this bounded helper.' }
			$cacheInfo = "$($metadata[0]),$($metadata[1]),$path"
			$env:GIT_INDEX_FILE = $tempIndex
			Invoke-Git -Arguments @('-C', $RepositoryRoot, 'update-index', '--add', '--cacheinfo', $cacheInfo) | Out-Null
			if ($previousIndexEnv) { $env:GIT_INDEX_FILE = $previousIndexEnv } else { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue }
		}

		$env:GIT_INDEX_FILE = $tempIndex
		$gitAddArgs = @('-C', $RepositoryRoot, '--literal-pathspecs', 'add', '--all', '--force', '--') + $normalizedPaths
		Invoke-Git -Arguments $gitAddArgs | Out-Null
		$gitDiffArgs = @('-C', $RepositoryRoot, '-c', 'core.quotePath=false', 'diff', '--binary', '--full-index', '--no-renames', '--cached', $BaseSha, "--output=$patchPath", '--') + $normalizedPaths
		Invoke-Git -Arguments $gitDiffArgs | Out-Null
		$patch = [IO.File]::ReadAllBytes($patchPath)
		if ($patch.Length -eq 0) { throw 'The selected paths contain no net changes relative to the remote base.' }
		if ($patch.Length -gt 2MB) { throw "Decoded patch is $($patch.Length) bytes; limit is 2097152 bytes." }

		$compressedStream = [IO.MemoryStream]::new()
		$gzip = [IO.Compression.GZipStream]::new($compressedStream, [IO.Compression.CompressionLevel]::Optimal, $true)
		$gzip.Write($patch, 0, $patch.Length)
		$gzip.Dispose()
		$compressed = $compressedStream.ToArray()
		$compressedStream.Dispose()
		if ($compressed.Length -gt 30000) { throw "Compressed patch is $($compressed.Length) bytes; limit is 30000 bytes." }
		$payload = [Convert]::ToBase64String($compressed)
		if ($payload.Length -gt 40000) { throw "Base64 patch field is $($payload.Length) characters; limit is 40000." }
		$patchHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($patch)).ToLowerInvariant()
		$indexFingerprintAfter = Get-FileFingerprint -Path $indexPath
		if ($indexFingerprintAfter -ne $indexFingerprintBefore) { throw 'The real Git index changed while preparing the patch.' }

		$request = [ordered]@{
			validation_mode = 'patch-validation'
			patch_base_sha = $BaseSha.ToLowerInvariant()
			patch_payload_gzip_base64 = $payload
			patch_sha256 = $patchHash
			patch_validation_selection = $Selection
		}
		$json = $request | ConvertTo-Json -Depth 4 -Compress
		$parent = [IO.Path]::GetDirectoryName($OutputJsonPath)
		if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container))
		{
			New-Item -ItemType Directory -Path $parent -Force | Out-Null
		}
		$tempOutputPath = "$OutputJsonPath.$([guid]::NewGuid().ToString('N')).tmp"
		try
		{
			[IO.File]::WriteAllText($tempOutputPath, $json + "`n", [Text.UTF8Encoding]::new($false))
			[IO.File]::Move($tempOutputPath, $OutputJsonPath)
		}
		finally
		{
			Remove-Item -LiteralPath $tempOutputPath -Force -ErrorAction SilentlyContinue
		}
		Write-Output "PATCH_REQUEST=READY; base=$($BaseSha.ToLowerInvariant()); patch_sha256=$patchHash; raw_bytes=$($patch.Length); compressed_bytes=$($compressed.Length); included_paths=$($normalizedPaths.Count); json=$OutputJsonPath"
		Write-Output 'EXPERIMENTAL PATCHED-SOURCE VALIDATION: this validates the resulting patch tree, never the unmodified base commit.'
		if ($Dispatch) { Send-WorkflowRequest -RequestPath $OutputJsonPath -Ref $WorkflowRef -Root $RepositoryRoot }
		else { Write-Output "Dispatch with the GitHub CLI JSON stdin mode using this file, or rerun with -Dispatch after authenticating gh: $OutputJsonPath" }
	}
	finally
	{
		if ($previousIndexEnv) { $env:GIT_INDEX_FILE = $previousIndexEnv } else { Remove-Item Env:GIT_INDEX_FILE -ErrorAction SilentlyContinue }
		Remove-Item -LiteralPath $tempIndex, $patchPath -Force -ErrorAction SilentlyContinue
	}
}
catch
{
	[Console]::Error.WriteLine("PATCH_REQUEST=REJECTED; $($_.Exception.Message)")
	exit 2
}
