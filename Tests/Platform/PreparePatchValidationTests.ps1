[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$Helper = Join-Path $PSScriptRoot 'PreparePatchValidation.ps1'
$TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ("pf-patch-helper-test-{0}" -f [guid]::NewGuid().ToString('N'))
$WorkingRepo = Join-Path $TemporaryRoot 'working'
$BareRemote = Join-Path $TemporaryRoot 'origin.git'
$VerificationRepo = Join-Path $TemporaryRoot 'verify'
$RequestPath = Join-Path $TemporaryRoot 'request.json'

function Invoke-TestGit
{
	param([string]$Repository, [string[]]$Arguments, [switch]$AllowFailure)
	$output = & git -C $Repository @Arguments 2>&1
	$exitCode = $LASTEXITCODE
	if (-not $AllowFailure -and $exitCode -ne 0)
	{
		throw "git $($Arguments -join ' ') failed ($exitCode): $($output -join [Environment]::NewLine)"
	}
	return [pscustomobject]@{ ExitCode = $exitCode; Output = @($output) }
}

function Invoke-Helper
{
	param([string]$Root, [string]$Commit, [string[]]$Paths, [string]$OutputPath, [switch]$AllowFailure)
	$pathListFile = "$OutputPath.paths.json"
	[IO.File]::WriteAllText($pathListFile, (ConvertTo-Json -InputObject @($Paths) -Compress), [Text.UTF8Encoding]::new($false))
	$arguments = @('-NoProfile', '-File', $Helper, '-RepositoryRoot', $Root, '-BaseSha', $Commit,
		'-RemoteRef', 'refs/heads/main', '-WorkflowRef', 'main', '-Selection', 'windows-asset-diagnostic-only',
		'-OutputJsonPath', $OutputPath, '-IncludePathFile', $pathListFile)
	$output = & pwsh @arguments 2>&1
	$exitCode = $LASTEXITCODE
	if (-not $AllowFailure -and $exitCode -ne 0)
	{
		throw "Patch helper failed ($exitCode): $($output -join [Environment]::NewLine)"
	}
	return [pscustomobject]@{ ExitCode = $exitCode; Output = @($output) }
}

try
{
	New-Item -ItemType Directory -Path $TemporaryRoot -Force | Out-Null
	& git init --bare --initial-branch=main $BareRemote *> $null
	if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the disposable bare fixture remote.' }
	& git init --initial-branch=main $WorkingRepo *> $null
	if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the disposable working fixture.' }
	Invoke-TestGit $WorkingRepo @('config', 'user.name', 'Patch Helper Test') | Out-Null
	Invoke-TestGit $WorkingRepo @('config', 'user.email', 'patch-helper@example.invalid') | Out-Null
	Invoke-TestGit $WorkingRepo @('config', 'core.filemode', 'false') | Out-Null

	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'tracked.txt'), "base`n", [Text.UTF8Encoding]::new($false))
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'mode.txt'), "mode base`n", [Text.UTF8Encoding]::new($false))
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'delete Ω.txt'), "delete me`n", [Text.UTF8Encoding]::new($false))
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'Protected unrelated Ω.txt'), "protected base`n", [Text.UTF8Encoding]::new($false))
	Invoke-TestGit $WorkingRepo @('add', '--all') | Out-Null
	Invoke-TestGit $WorkingRepo @('commit', '-m', 'synthetic patch helper base') | Out-Null
	Invoke-TestGit $WorkingRepo @('remote', 'add', 'origin', $BareRemote) | Out-Null
	Invoke-TestGit $WorkingRepo @('push', '-u', 'origin', 'main') | Out-Null
	$BaseSha = [string](Invoke-TestGit $WorkingRepo @('rev-parse', 'HEAD')).Output[-1]
	& git clone --quiet $BareRemote $VerificationRepo
	if ($LASTEXITCODE -ne 0) { throw 'Could not create the disposable verification fixture.' }
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'Unpublished Ω.txt'), "earlier local commit café`n", [Text.UTF8Encoding]::new($false))
	Invoke-TestGit $WorkingRepo @('add', '--', 'Unpublished Ω.txt') | Out-Null
	Invoke-TestGit $WorkingRepo @('commit', '-m', 'unpublished helper input') | Out-Null

	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'tracked.txt'), "staged Ω value`n", [Text.UTF8Encoding]::new($false))
	Invoke-TestGit $WorkingRepo @('add', '--', 'tracked.txt') | Out-Null
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'tracked.txt'), "final unstaged Ω value`n", [Text.UTF8Encoding]::new($false))
	Invoke-TestGit $WorkingRepo @('update-index', '--chmod=+x', '--', 'mode.txt') | Out-Null
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'Added Ω.txt'), "added café 雪 Ω`n", [Text.UTF8Encoding]::new($false))
	Remove-Item -LiteralPath (Join-Path $WorkingRepo 'delete Ω.txt')
	[IO.File]::WriteAllText((Join-Path $WorkingRepo 'Protected unrelated Ω.txt'), "must remain local`n", [Text.UTF8Encoding]::new($false))

	$indexRelative = [string](Invoke-TestGit $WorkingRepo @('rev-parse', '--git-path', 'index')).Output[-1]
	$indexPath = if ([IO.Path]::IsPathRooted($indexRelative)) { $indexRelative } else { Join-Path $WorkingRepo $indexRelative }
	$indexBefore = (Get-FileHash -LiteralPath $indexPath -Algorithm SHA256).Hash
	$statusBefore = ((Invoke-TestGit $WorkingRepo @('status', '--porcelain=v1')).Output -join "`n")
	$result = Invoke-Helper $WorkingRepo $BaseSha @('tracked.txt', 'mode.txt', 'Added Ω.txt', 'delete Ω.txt', 'Unpublished Ω.txt') $RequestPath
	$indexAfter = (Get-FileHash -LiteralPath $indexPath -Algorithm SHA256).Hash
	$statusAfter = ((Invoke-TestGit $WorkingRepo @('status', '--porcelain=v1')).Output -join "`n")
	if ($indexBefore -ne $indexAfter) { throw "Patch helper changed the user's real staging index." }
	if ($statusBefore -ne $statusAfter) { throw 'Patch helper changed staged/unstaged status.' }
	$request = Get-Content -LiteralPath $RequestPath -Raw -Encoding utf8 | ConvertFrom-Json
	if ($request.patch_base_sha -ne $BaseSha -or $request.validation_mode -ne 'patch-validation')
	{
		throw 'Generated request did not preserve its base and patch mode.'
	}
	$compressed = [Convert]::FromBase64String($request.patch_payload_gzip_base64)
	$compressedStream = [IO.MemoryStream]::new($compressed)
	$gzip = [IO.Compression.GZipStream]::new($compressedStream, [IO.Compression.CompressionMode]::Decompress)
	$patchStream = [IO.MemoryStream]::new()
	$gzip.CopyTo($patchStream)
	$gzip.Dispose()
	$patch = $patchStream.ToArray()
	$patchStream.Dispose()
	$compressedStream.Dispose()
	$patchHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($patch)).ToLowerInvariant()
	if ($patchHash -ne $request.patch_sha256) { throw 'Generated patch checksum does not match its decoded bytes.' }
	$PatchFile = Join-Path $TemporaryRoot 'patch.diff'
	[IO.File]::WriteAllBytes($PatchFile, $patch)
	if ($patch.Length -gt 2MB) { throw 'Helper failed to enforce the decoded patch limit.' }
	$PatchText = [Text.Encoding]::UTF8.GetString($patch)
	if (-not $PatchText.Contains('Added Ω.txt') -or -not $PatchText.Contains('delete Ω.txt') -or
		-not $PatchText.Contains('Unpublished Ω.txt') -or
		-not $PatchText.Contains('new mode 100755') -or $PatchText.Contains('Protected unrelated Ω.txt'))
	{
		throw 'Patch omitted selected Unicode/mode changes or included the protected unrelated file.'
	}
	Invoke-TestGit $VerificationRepo @('apply', '--check', '--binary', $PatchFile) | Out-Null
	Invoke-TestGit $VerificationRepo @('apply', '--binary', $PatchFile) | Out-Null
	$verifiedTracked = [IO.File]::ReadAllText((Join-Path $VerificationRepo 'tracked.txt'))
	if ($verifiedTracked.TrimEnd("`r", "`n") -ne 'final unstaged Ω value')
	{
		throw "The patch did not capture the current unstaged content over the earlier staged content: <$verifiedTracked>"
	}
	if ([IO.File]::ReadAllText((Join-Path $VerificationRepo 'Added Ω.txt')).TrimEnd("`r", "`n") -ne 'added café 雪 Ω')
	{
		throw 'The patch did not preserve Unicode added-file content.'
	}
	if ([IO.File]::ReadAllText((Join-Path $VerificationRepo 'Unpublished Ω.txt')).TrimEnd("`r", "`n") -ne 'earlier local commit café')
	{
		throw 'The patch did not include an earlier unpublished local commit relative to the remote base.'
	}
	if (Test-Path -LiteralPath (Join-Path $VerificationRepo 'delete Ω.txt')) { throw 'The patch did not capture the selected deletion.' }
	if ([IO.File]::ReadAllText((Join-Path $VerificationRepo 'Protected unrelated Ω.txt')).TrimEnd("`r", "`n") -ne 'protected base')
	{
		throw 'An excluded protected file was present in the experimental patch.'
	}

	$wrongBase = Invoke-Helper $WorkingRepo ('0' * 40) @('tracked.txt') (Join-Path $TemporaryRoot 'wrong-base.json') -AllowFailure
	if ($wrongBase.ExitCode -eq 0) { throw 'Helper accepted a base not present on the remote ref.' }
	$caseAlias = Invoke-Helper $WorkingRepo $BaseSha @('tracked.txt', 'TRACKED.TXT') (Join-Path $TemporaryRoot 'case-alias.json') -AllowFailure
	if ($caseAlias.ExitCode -eq 0) { throw 'Helper accepted case-equivalent included paths.' }
	$randomBytes = [byte[]]::new(64000)
	[Security.Cryptography.RandomNumberGenerator]::Fill($randomBytes)
	[IO.File]::WriteAllBytes((Join-Path $WorkingRepo 'oversized.bin'), $randomBytes)
	$tooLarge = Invoke-Helper $WorkingRepo $BaseSha @('oversized.bin') (Join-Path $TemporaryRoot 'oversized.json') -AllowFailure
	if ($tooLarge.ExitCode -eq 0) { throw 'Helper accepted an oversized payload.' }
	Write-Output 'PATCH_HELPER_TESTS=PASS; add/modify/delete; unpublished commit; Unicode paths/content; staged+unstaged isolation; executable mode; protected exclusion; checksum; remote base; case aliases; size bound'
}
finally
{
	if (Test-Path -LiteralPath $TemporaryRoot) { Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force }
}
