param(
	[Parameter(Mandatory = $true)]
	[string]$TestExecutable,
	[string]$WorkingDirectory = (Get-Location).Path,
	[string]$LogDirectory = $env:TEMP,
	[int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $TestExecutable -PathType Leaf)) {
	throw "Asset test executable is missing: $TestExecutable"
}
if (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
	throw "Asset test working directory is missing: $WorkingDirectory"
}
if ($TimeoutSeconds -lt 10 -or $TimeoutSeconds -gt 300) {
	throw "Diagnostic timeout must be between 10 and 300 seconds: $TimeoutSeconds"
}

New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

function Find-Cdb {
	$Candidates = [System.Collections.Generic.List[string]]::new()
	$Command = Get-Command cdb.exe -ErrorAction SilentlyContinue
	if ($Command) { $Candidates.Add($Command.Source) }
	foreach ($ProgramFilesRoot in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
		if ($ProgramFilesRoot) {
			$Candidates.Add((Join-Path $ProgramFilesRoot 'Windows Kits/10/Debuggers/x64/cdb.exe'))
		}
	}
	return $Candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}

function Write-ProcessLine {
	param([string]$Path, [string]$Line, [bool]$IsError)
	[System.IO.File]::AppendAllText($Path, $Line + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
	if ($IsError) { [Console]::Error.WriteLine($Line) }
	else { [Console]::Out.WriteLine($Line) }
}

function Write-DiagnosticLine {
	param([string]$Line)
	[Console]::Out.WriteLine($Line)
}

function Capture-ProcessStacks {
	param([System.Diagnostics.Process]$Target, [string]$Path, [string]$ExecutableDirectory)

	$Cdb = Find-Cdb
	if (-not $Cdb) {
		Write-DiagnosticLine 'ASSET_TEST_STACK_CAPTURE_UNAVAILABLE cdb.exe was not installed on the runner.'
		return $false
	}

	$CommandText = ".sympath `"$ExecutableDirectory`"; .reload /f; ~* kp; .detach; q"
	$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$StartInfo.FileName = $Cdb
	$StartInfo.UseShellExecute = $false
	$StartInfo.CreateNoWindow = $true
	$StartInfo.RedirectStandardOutput = $true
	$StartInfo.RedirectStandardError = $true
	[void]$StartInfo.ArgumentList.Add('-p')
	[void]$StartInfo.ArgumentList.Add([string]$Target.Id)
	[void]$StartInfo.ArgumentList.Add('-c')
	[void]$StartInfo.ArgumentList.Add($CommandText)
	Write-DiagnosticLine "ASSET_TEST_STACK_CAPTURE_BEGIN debugger=$Cdb pid=$($Target.Id)"

	$Debugger = [System.Diagnostics.Process]::new()
	$Debugger.StartInfo = $StartInfo
	try {
		if (-not $Debugger.Start()) {
			Write-DiagnosticLine 'ASSET_TEST_STACK_CAPTURE_FAILED debugger process did not start.'
			return $false
		}
		$StdoutTask = $Debugger.StandardOutput.ReadToEndAsync()
		$StderrTask = $Debugger.StandardError.ReadToEndAsync()
		if (-not $Debugger.WaitForExit(30000)) {
			Write-DiagnosticLine "ASSET_TEST_STACK_CAPTURE_TIMEOUT debugger_pid=$($Debugger.Id)"
			$Debugger.Kill()
			$Debugger.WaitForExit(5000) | Out-Null
		}
		$Stdout = $StdoutTask.GetAwaiter().GetResult()
		$Stderr = $StderrTask.GetAwaiter().GetResult()
		[System.IO.File]::WriteAllText($Path, $Stdout + $Stderr, [System.Text.UTF8Encoding]::new($false))
		Write-DiagnosticLine "ASSET_TEST_STACK_CAPTURE_EXIT debugger_exit=$($Debugger.ExitCode)"
		Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue | ForEach-Object { [Console]::Out.WriteLine($_) }
		return $Debugger.ExitCode -eq 0
	}
	finally {
		$Debugger.Dispose()
	}
}

foreach ($Mode in @('--cook-only', '--export-only')) {
	$SafeMode = $Mode.TrimStart('-').Replace('-', '_')
	$StdoutPath = Join-Path $LogDirectory "asset-test-$SafeMode.stdout.log"
	$StderrPath = Join-Path $LogDirectory "asset-test-$SafeMode.stderr.log"
	$StackPath = Join-Path $LogDirectory "asset-test-$SafeMode.stacks.log"
	Remove-Item -LiteralPath $StdoutPath, $StderrPath, $StackPath -Force -ErrorAction SilentlyContinue
	Write-Output "ASSET_TEST_PROCESS_BEGIN mode=$Mode executable=$TestExecutable cwd=$WorkingDirectory timeout_seconds=$TimeoutSeconds"

	$StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$StartInfo.FileName = [System.IO.Path]::GetFullPath($TestExecutable)
	$StartInfo.WorkingDirectory = [System.IO.Path]::GetFullPath($WorkingDirectory)
	$StartInfo.UseShellExecute = $false
	$StartInfo.CreateNoWindow = $true
	$StartInfo.RedirectStandardOutput = $true
	$StartInfo.RedirectStandardError = $true
	[void]$StartInfo.ArgumentList.Add($Mode)

	$Process = [System.Diagnostics.Process]::new()
	$Process.StartInfo = $StartInfo
	try {
		if (-not $Process.Start()) { throw "Could not start asset test process: $TestExecutable $Mode" }
		Write-Output "ASSET_TEST_PROCESS_STARTED mode=$Mode pid=$($Process.Id)"
		$OutputRead = $Process.StandardOutput.ReadLineAsync()
		$ErrorRead = $Process.StandardError.ReadLineAsync()
		$Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
		$TimedOut = $false
		$OutputEnded = $false
		$ErrorEnded = $false

		while (-not ($OutputEnded -and $ErrorEnded -and $Process.HasExited)) {
			if (-not $OutputEnded -and $OutputRead.IsCompleted) {
				$Line = $OutputRead.GetAwaiter().GetResult()
				if ($null -eq $Line) { $OutputEnded = $true }
				else {
					Write-ProcessLine -Path $StdoutPath -Line $Line -IsError $false
					$OutputRead = $Process.StandardOutput.ReadLineAsync()
				}
			}
			if (-not $ErrorEnded -and $ErrorRead.IsCompleted) {
				$Line = $ErrorRead.GetAwaiter().GetResult()
				if ($null -eq $Line) { $ErrorEnded = $true }
				else {
					Write-ProcessLine -Path $StderrPath -Line $Line -IsError $true
					$ErrorRead = $Process.StandardError.ReadLineAsync()
				}
			}
			if ([DateTime]::UtcNow -ge $Deadline) { $TimedOut = $true; break }
			if (-not ($OutputRead.IsCompleted -or $ErrorRead.IsCompleted)) {
				$Process.WaitForExit(100) | Out-Null
			}
		}

		if ($TimedOut) {
			$Process.Refresh()
			Write-Output "ASSET_TEST_PROCESS_HUNG mode=$Mode pid=$($Process.Id) cpu_seconds=$([Math]::Round($Process.TotalProcessorTime.TotalSeconds, 2)) working_set_bytes=$($Process.WorkingSet64)"
			$Process.Threads | ForEach-Object {
				$Thread = $_
				$WaitReason = 'unavailable'
				try { $WaitReason = $Thread.WaitReason.ToString() } catch { }
				Write-Output "ASSET_TEST_THREAD pid=$($Process.Id) tid=$($Thread.Id) state=$($Thread.ThreadState) wait_reason=$WaitReason cpu_ms=$($Thread.TotalProcessorTime.TotalMilliseconds)"
			}
			try {
				Get-CimInstance Win32_Process -Filter "ParentProcessId = $($Process.Id)" -ErrorAction Stop |
					ForEach-Object { Write-Output "ASSET_TEST_CHILD pid=$($_.ProcessId) parent_pid=$($_.ParentProcessId) name=$($_.Name) path=$($_.ExecutablePath)" }
			} catch {
				Write-Output "ASSET_TEST_CHILD_ENUMERATION_UNAVAILABLE reason=$($_.Exception.Message)"
			}
			Get-Content -LiteralPath $StdoutPath, $StderrPath -ErrorAction SilentlyContinue | Select-Object -Last 100

			$StacksCaptured = Capture-ProcessStacks -Target $Process -Path $StackPath `
				-ExecutableDirectory (Split-Path -Parent $TestExecutable)
			if ($Process.HasExited) {
				Write-Output "ASSET_TEST_EXITED_DURING_STACK_CAPTURE pid=$($Process.Id) exit_code=$($Process.ExitCode)"
			} else {
				$Process.Kill()
				$Process.WaitForExit(10000) | Out-Null
				Write-Output "ASSET_TEST_OWNED_PROCESS_TERMINATED pid=$($Process.Id) stacks_captured=$StacksCaptured"
			}
			throw "Asset suite $Mode exceeded $TimeoutSeconds seconds; owned PID $($Process.Id) was inspected and terminated."
		}

		$Process.WaitForExit()
		Write-Output "ASSET_TEST_PROCESS_EXIT mode=$Mode pid=$($Process.Id) exit_code=$($Process.ExitCode)"
		if ($Process.ExitCode -ne 0) { throw "Asset suite $Mode failed with exit code $($Process.ExitCode)." }
	}
	finally {
		if (-not $Process.HasExited) {
			$Process.Kill()
			$Process.WaitForExit(10000) | Out-Null
		}
		$Process.Dispose()
	}
}
