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
PreparePatchValidation.ps1 captures selected working-tree paths into a bounded patch file for
.github/workflows/portable-editor-ci.yml. It uses a temporary Git index and does not modify
or stage the user's real index. Staged mode/content is used as the seed, then the working tree
is overlaid so unstaged edits are included. Explicit paths determine what is sent.

Example:
  pwsh -NoProfile -File Tests/Platform/PreparePatchValidation.ps1 `
    -BaseSha <remote-commit-sha> -RemoteRef refs/heads/main `
    -IncludePath Tests/OneFocusedTest.cpp,Tests/RelatedFixture.txt `
    -Selection windows-asset-diagnostic-only -OutputJsonPath "$env:TEMP/pf-patch-request.json"

The helper writes a gzip patch file and a small JSON request. With -Dispatch it transfers
bounded chunks through short-lived GitHub Actions artifacts, verifies each chunk run, then
dispatches one complete patch validation. Artifacts are public to repository readers and expire
after one day. No patch content or credential is placed in executable command text. This requires
an already authenticated `gh` CLI and does not extract or print its credentials.

Limits: <= 1,000 explicit include paths; decoded patch <= 32 MiB; compressed patch <= 8 MiB;
<= 44,000 bytes per transport chunk and <= 191 chunks (an ordinary 100 KiB compressed patch uses three);
<= 1,000 changed paths and 100 KiB of paths; binary patch
sections <= 4 MiB each and 8 MiB total. These bounds limit accidental binary inclusion and
decompression abuse. Oversized/unsupported patches are rejected, never committed or uploaded elsewhere.
Patching workflow YAML after dispatch does not change the workflow definition already running.
Workflow-definition changes require explicit authorization to publish and dispatch that workflow revision.
An experimental patched-source run validates its recorded result tree, not its base commit.
'@ | Write-Output
}

function Get-TransferRun
{
	param([string]$Title, [string]$Root)
	$gh = Get-Command gh -CommandType Application -ErrorAction SilentlyContinue
	if (-not $gh) { throw 'GitHub CLI (gh) is not installed or not on PATH.' }
	$deadline = [DateTime]::UtcNow.AddMinutes(3)
	do
	{
		$json = & $gh.Source run list --workflow portable-editor-ci.yml --event workflow_dispatch --limit 100 `
			--json databaseId,displayTitle,status,conclusion,url,headSha,headBranch 2>&1
		$code = $LASTEXITCODE
		if ($code -ne 0) { throw "Could not inspect patch transport runs ($code): $($json -join [Environment]::NewLine)" }
		$runs = (($json -join "`n") | ConvertFrom-Json)
		$match = @($runs | Where-Object { $_.displayTitle -ceq $Title } | Select-Object -First 1)
		if ($match.Count -gt 0) { return $match[0] }
		Start-Sleep -Seconds 2
	} while ([DateTime]::UtcNow -lt $deadline)
	throw "Timed out waiting for the unique transport run title '$Title'."
}

function Wait-TransferRun
{
	param([long]$RunId, [string]$ArtifactName, [string]$ExpectedChunkHash, [string]$Root)
	$gh = Get-Command gh -CommandType Application -ErrorAction SilentlyContinue
	$watch = [Diagnostics.ProcessStartInfo]::new()
	$watch.FileName = $gh.Source
	$watch.UseShellExecute = $false
	$watch.RedirectStandardOutput = $true
	$watch.RedirectStandardError = $true
	foreach ($argument in @('run', 'watch', "$RunId", '--exit-status')) { $watch.ArgumentList.Add($argument) }
	$process = [Diagnostics.Process]::new()
	$process.StartInfo = $watch
	try
	{
		if (-not $process.Start()) { throw "Could not watch owned transport run $RunId." }
		$stdout = $process.StandardOutput.ReadToEndAsync()
		$stderr = $process.StandardError.ReadToEndAsync()
		if (-not $process.WaitForExit(600000))
		{
			$process.Kill()
			if (-not $process.WaitForExit(5000)) { throw "Could not stop gh run watch for transport run $RunId." }
			throw "Transport run $RunId did not finish within ten minutes."
		}
		if ($process.ExitCode -ne 0)
		{
			throw "Transport run $RunId failed: $(($stderr.GetAwaiter().GetResult()).Trim()) $(($stdout.GetAwaiter().GetResult()).Trim())"
		}
	}
	finally
	{
		if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
		$process.Dispose()
	}
	$download = Join-Path ([IO.Path]::GetTempPath()) ("pf-patch-artifact-{0}" -f [guid]::NewGuid().ToString('N'))
	New-Item -ItemType Directory -Path $download | Out-Null
	try
	{
		& $gh.Source run download "$RunId" --name $ArtifactName --dir $download *> $null
		if ($LASTEXITCODE -ne 0) { throw "Could not retrieve chunk artifact '$ArtifactName' from run $RunId." }
		$chunkPath = Join-Path $download 'patch-chunk.bin'
		$metadataPath = Join-Path $download 'patch-chunk.json'
		if (-not (Test-Path -LiteralPath $chunkPath -PathType Leaf) -or -not (Test-Path -LiteralPath $metadataPath -PathType Leaf))
		{
			throw "Transport run $RunId did not publish the expected chunk files."
		}
		$actualHash = (Get-FileHash -LiteralPath $chunkPath -Algorithm SHA256).Hash.ToLowerInvariant()
		if ($actualHash -ne $ExpectedChunkHash) { throw "Transport run $RunId artifact chunk checksum mismatch." }
		return $RunId
	}
	finally { Remove-Item -LiteralPath $download -Recurse -Force -ErrorAction SilentlyContinue }
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

function Assert-PatchSizePolicy
{
	param([byte[]]$Patch)
	$text = [Text.Encoding]::ASCII.GetString($Patch)
	$sectionStart = 0
	$marker = "GIT binary patch`n"
	$diffMarker = 'diff --git '
	$totalBinaryBytes = 0
	while ($true)
	{
		$nextDiff = $text.IndexOf($diffMarker, $sectionStart, [StringComparison]::Ordinal)
		$sectionEnd = if ($nextDiff -lt 0) { $text.Length } else { $nextDiff }
		$binaryMarker = $text.IndexOf($marker, $sectionStart, [StringComparison]::Ordinal)
		if ($binaryMarker -ge 0 -and $binaryMarker -lt $sectionEnd)
		{
			$sectionBytes = $sectionEnd - $binaryMarker
			if ($sectionBytes -gt 4MB) { throw "A binary diff section is $sectionBytes bytes; per-file limit is 4194304 bytes." }
			$totalBinaryBytes += $sectionBytes
			if ($totalBinaryBytes -gt 8MB) { throw "Binary diff sections total $totalBinaryBytes bytes; aggregate limit is 8388608 bytes." }
		}
		if ($nextDiff -lt 0) { break }
		$sectionStart = $nextDiff + $diffMarker.Length
	}
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
	if ($value -eq '.patch-validation-tools' -or $value.StartsWith('.patch-validation-tools/', [StringComparison]::OrdinalIgnoreCase))
	{
		throw 'The isolated patch-verifier checkout cannot be included in a source patch.'
	}
	if ($value -eq '.github/workflows' -or $value.StartsWith('.github/workflows/', [StringComparison]::OrdinalIgnoreCase))
	{
		throw 'Workflow YAML cannot be validated through a source patch; a separately authorized published workflow revision is required.'
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
	if ($IncludePath.Count -gt 1000) { throw 'At most 1000 included paths are supported per patch request.' }
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
	if ($totalPathBytes -gt 100KB) { throw 'Included path list exceeds the 102400-byte bound.' }
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
		$expectedTreeResult = Invoke-Git -Arguments @('-C', $RepositoryRoot, 'write-tree')
		$expectedTree = [string]$expectedTreeResult.Output[-1]
		if ($expectedTree -notmatch '^[0-9a-fA-F]{40}$') { throw 'Temporary patch index did not produce a valid expected source tree identity.' }
		$patch = [IO.File]::ReadAllBytes($patchPath)
		if ($patch.Length -eq 0) { throw 'The selected paths contain no net changes relative to the remote base.' }
		$maxPatchBytes = 32MB
		if ($patch.Length -gt $maxPatchBytes) { throw "Decoded patch is $($patch.Length) bytes; limit is $maxPatchBytes bytes." }
		Assert-PatchSizePolicy -Patch $patch

		$compressedStream = [IO.MemoryStream]::new()
		$gzip = [IO.Compression.GZipStream]::new($compressedStream, [IO.Compression.CompressionLevel]::Optimal, $true)
		$gzip.Write($patch, 0, $patch.Length)
		$gzip.Dispose()
		$compressed = $compressedStream.ToArray()
		$compressedStream.Dispose()
		$maxCompressedBytes = 8MB
		if ($compressed.Length -gt $maxCompressedBytes)
		{
			throw "Compressed patch is $($compressed.Length) bytes; limit is $maxCompressedBytes bytes. Select a complete smaller change set; files were not omitted or committed."
		}
		$chunkBytes = 44000
		$chunkCount = [Math]::Ceiling($compressed.Length / [double]$chunkBytes)
		$maxChunkCount = [Math]::Ceiling($maxCompressedBytes / [double]$chunkBytes)
		if ($chunkCount -gt $maxChunkCount) { throw "Patch requires $chunkCount artifact chunks; limit is $maxChunkCount." }
		$patchHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($patch)).ToLowerInvariant()
		$compressedHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($compressed)).ToLowerInvariant()
		$indexFingerprintAfter = Get-FileFingerprint -Path $indexPath
		if ($indexFingerprintAfter -ne $indexFingerprintBefore) { throw 'The real Git index changed while preparing the patch.' }
		$parent = [IO.Path]::GetDirectoryName($OutputJsonPath)
		if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container))
		{
			New-Item -ItemType Directory -Path $parent -Force | Out-Null
		}
		$patchFilePath = "$OutputJsonPath.patch.gz"
		if (Test-Path -LiteralPath $patchFilePath) { throw "Refusing to overwrite an existing patch file: $patchFilePath" }
		[IO.File]::WriteAllBytes($patchFilePath, $compressed)
		$transferId = [guid]::NewGuid().ToString('N')
		$runIds = [Collections.Generic.List[long]]::new()
		$transportWorkflowSha = $null
		if ($Dispatch)
		{
			for ($chunkIndex = 0; $chunkIndex -lt $chunkCount; $chunkIndex++)
			{
				$offset = $chunkIndex * $chunkBytes
				$count = [Math]::Min($chunkBytes, $compressed.Length - $offset)
				$chunk = [byte[]]::new($count)
				[Array]::Copy($compressed, $offset, $chunk, 0, $count)
				$chunkHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($chunk)).ToLowerInvariant()
				$chunkRequestPath = Join-Path ([IO.Path]::GetTempPath()) ("pf-patch-chunk-{0}.json" -f [guid]::NewGuid().ToString('N'))
				$chunkRequest = [ordered]@{
					validation_mode = 'patch-transfer'
					patch_base_sha = $BaseSha.ToLowerInvariant()
					patch_transport_label = "$transferId-$chunkIndex-$chunkCount"
					patch_request_json = ([ordered]@{
						kind = 'chunk'
						transfer_id = $transferId
						index = [string]$chunkIndex
						count = [string]$chunkCount
					chunk_base64 = [Convert]::ToBase64String($chunk)
						chunk_sha256 = $chunkHash
						patch_sha256 = $patchHash
					} | ConvertTo-Json -Compress)
				}
				[IO.File]::WriteAllText($chunkRequestPath, ($chunkRequest | ConvertTo-Json -Compress) + "`n", [Text.UTF8Encoding]::new($false))
				try
				{
					Send-WorkflowRequest -RequestPath $chunkRequestPath -Ref $WorkflowRef -Root $RepositoryRoot
					$title = "Portable editor CI · Patch transfer · $transferId-$chunkIndex-$chunkCount"
					$run = Get-TransferRun -Title $title -Root $RepositoryRoot
					if ($run.status -eq 'completed' -and $run.conclusion -ne 'success') { throw "Transport run $($run.databaseId) concluded $($run.conclusion)." }
					$expectedWorkflowBranch = $WorkflowRef -replace '^refs/heads/', ''
					if ($run.headBranch -cne $expectedWorkflowBranch -or $run.headSha -notmatch '^[0-9a-fA-F]{40}$')
					{
						throw "Transport run $($run.databaseId) used an unexpected workflow ref or had no exact workflow SHA."
					}
					if ($transportWorkflowSha -and $transportWorkflowSha -cne $run.headSha)
					{
						throw 'The workflow definition changed while patch chunks were being uploaded; this transfer cannot be assembled.'
					}
					$transportWorkflowSha = [string]$run.headSha
					Wait-TransferRun -RunId $run.databaseId -ArtifactName ("pulseforge-patch-{0}-{1}-{2}" -f $transferId, $chunkIndex, $chunkCount) `
						-ExpectedChunkHash $chunkHash -Root $RepositoryRoot | Out-Null
					$runIds.Add([long]$run.databaseId)
					Write-Output "PATCH_TRANSFER=PASS; chunk=$($chunkIndex + 1)/$chunkCount; run=$($run.databaseId); url=$($run.url)"
				}
				finally { Remove-Item -LiteralPath $chunkRequestPath -Force -ErrorAction SilentlyContinue }
			}
		}
		$request = [ordered]@{
			validation_mode = 'patch-validation'
			patch_base_sha = $BaseSha.ToLowerInvariant()
			patch_sha256 = $patchHash
			patch_validation_selection = $Selection
			patch_request_json = ([ordered]@{
				kind = 'manifest'
				base_sha = $BaseSha.ToLowerInvariant()
				expected_tree = $expectedTree.ToLowerInvariant()
				selection = $Selection
				transfer_id = $transferId
				count = [int]$chunkCount
				run_ids = @($runIds.ToArray())
				transport_workflow_sha = $transportWorkflowSha
				patch_sha256 = $patchHash
				compressed_sha256 = $compressedHash
			} | ConvertTo-Json -Depth 4 -Compress)
		}
		$json = $request | ConvertTo-Json -Depth 4 -Compress
		$tempOutputPath = "$OutputJsonPath.$([guid]::NewGuid().ToString('N')).tmp"
		try
		{
			[IO.File]::WriteAllText($tempOutputPath, $json + "`n", [Text.UTF8Encoding]::new($false))
			[IO.File]::Move($tempOutputPath, $OutputJsonPath)
		}
		finally { Remove-Item -LiteralPath $tempOutputPath -Force -ErrorAction SilentlyContinue }
		Write-Output "PATCH_REQUEST=READY; base=$($BaseSha.ToLowerInvariant()); patch_sha256=$patchHash; expected_tree=$($expectedTree.ToLowerInvariant()); compressed_sha256=$compressedHash; raw_bytes=$($patch.Length); compressed_bytes=$($compressed.Length); chunks=$chunkCount; included_paths=$($normalizedPaths.Count); patch_file=$patchFilePath; json=$OutputJsonPath"
		Write-Output 'EXPERIMENTAL PATCHED-SOURCE VALIDATION: this validates the resulting patch tree, never the unmodified base commit.'
		if ($Dispatch) { Send-WorkflowRequest -RequestPath $OutputJsonPath -Ref $WorkflowRef -Root $RepositoryRoot }
		else { Write-Output 'The gzip file remains local. Use -Dispatch to upload bounded one-day artifact chunks and start complete patched-source validation.' }
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
