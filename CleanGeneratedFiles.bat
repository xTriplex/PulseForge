@echo off
setlocal EnableExtensions DisableDelayedExpansion

for %%I in ("%~dp0.") do set "PF_ROOT=%%~fI"
if not exist "%PF_ROOT%\CMakeLists.txt" goto InvalidRoot

echo.
echo PulseForge generated-file cleanup
echo Repository: %PF_ROOT%
echo.

:Menu
echo 1. Clean canonical Build directory
echo 2. Clean Visual Studio out directory
echo 3. Deep clean both Build and out
echo 4. Show generated directory sizes
echo 5. Cancel
echo.
set "PF_CHOICE="
set /p "PF_CHOICE=Select an option: "

if "%PF_CHOICE%"=="1" goto CleanBuild
if "%PF_CHOICE%"=="2" goto CleanOut
if "%PF_CHOICE%"=="3" goto DeepClean
if "%PF_CHOICE%"=="4" goto ShowSizes
if "%PF_CHOICE%"=="5" goto Cancel
echo Please enter a number from 1 to 5.
echo.
goto Menu

:CleanBuild
set "PF_TARGETS=Build"
call :CleanTargets "the canonical CMake build tree"
goto Menu

:CleanOut
set "PF_TARGETS=out"
call :CleanTargets "the Visual Studio CMake build tree"
goto Menu

:DeepClean
set "PF_TARGETS=Build,out"
call :CleanTargets "both generated build trees"
goto Menu

:CleanTargets
echo.
echo Repository: %PF_ROOT%
for %%T in (%PF_TARGETS:,= %) do echo Target:     %PF_ROOT%\%%T
echo.
echo This permanently removes only the generated directory or directories listed above.
echo A full Build cleanup also removes CMake caches and fetched dependencies such as NVRHI, Vulkan-Headers, Lua, and DXC.
echo Reconfiguration may need network access and the next build will recompile substantially more.
echo Normal development should retain these trees for incremental builds.
echo.
if "%PF_TARGETS%"=="Build,out" goto ConfirmBoth
set "PF_CONFIRM="
set /p "PF_CONFIRM=Type YES to continue: "
if /i not "%PF_CONFIRM%"=="YES" goto CancelCleanup
goto ConfirmationAccepted

:ConfirmBoth
set "PF_CONFIRM="
set /p "PF_CONFIRM=Type DELETE BOTH to continue: "
if /i not "%PF_CONFIRM%"=="DELETE BOTH" goto CancelCleanup

:ConfirmationAccepted
set "PF_BUILD_ACK=0"
call :CheckBuildProcesses
if errorlevel 1 exit /b 0

set "PF_CLEAN_ROOT=%PF_ROOT%"
set "PF_CLEAN_TARGETS=%PF_TARGETS%"
set "PF_CLEAN_BUILD_ACK=%PF_BUILD_ACK%"
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; try { $root = [IO.Path]::GetFullPath($env:PF_CLEAN_ROOT).TrimEnd([IO.Path]::DirectorySeparatorChar); if (-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt') -PathType Leaf)) { throw 'Repository marker CMakeLists.txt is missing.' }; $names = @($env:PF_CLEAN_TARGETS -split ','); if ($names.Count -eq 0 -or @($names | Where-Object { $_ -notin @('Build', 'out') }).Count -ne 0) { throw 'A cleanup target is not allowlisted.' }; $targets = @(); foreach ($name in $names) { $path = [IO.Path]::GetFullPath((Join-Path $root $name)).TrimEnd([IO.Path]::DirectorySeparatorChar); if ([string]::Equals([IO.Path]::GetDirectoryName($path), $root, [StringComparison]::OrdinalIgnoreCase) -eq $false -or [IO.Path]::GetFileName($path) -cne $name) { throw ('Target is not a direct repository child: ' + $path) }; if (Test-Path -LiteralPath $path) { $item = Get-Item -LiteralPath $path -Force; if (-not $item.PSIsContainer) { throw ('Target is not a directory: ' + $path) }; if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw ('Refusing to remove a reparse point: ' + $path) }; $links = @(Get-ChildItem -LiteralPath $path -Force -Recurse -ErrorAction Stop | Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 }); if ($links.Count -ne 0) { throw ('Refusing a tree containing reparse points: ' + $links[0].FullName) }; $measure = Get-ChildItem -LiteralPath $path -Force -Recurse -File -ErrorAction Stop | Measure-Object -Property Length -Sum; $bytes = if ($null -eq $measure.Sum) { [int64]0 } else { [int64]$measure.Sum }; $targets += [pscustomobject]@{ Name = $name; Path = $path; Bytes = $bytes } } else { $targets += [pscustomobject]@{ Name = $name; Path = $path; Bytes = [int64]0 } } }; $active = @(Get-Process -Name cmake, ninja, MSBuild, cl, link -ErrorAction SilentlyContinue); if ($active.Count -gt 0 -and $env:PF_CLEAN_BUILD_ACK -ne '1') { throw 'A likely build process started during cleanup checks; no directories were removed. Close it and retry.' }; foreach ($target in $targets) { if (Test-Path -LiteralPath $target.Path) { Remove-Item -LiteralPath $target.Path -Recurse -Force -ErrorAction Stop; if (Test-Path -LiteralPath $target.Path) { throw ('Directory still exists after removal: ' + $target.Path) } } }; $reclaimed = [int64](($targets | Measure-Object -Property Bytes -Sum).Sum); Write-Output ('Cleanup complete. Approximate file data removed: {0:N2} GB ({1:N0} bytes).' -f ($reclaimed / 1GB), $reclaimed) } catch { Write-Error $_; exit 1 }"
if errorlevel 1 (
	echo Cleanup failed or was refused. A failure during deletion may have removed some generated files.
)
echo.
exit /b 0

:CheckBuildProcesses
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; try { $processes = @(Get-Process -Name cmake, ninja, MSBuild, cl, link -ErrorAction SilentlyContinue); if ($processes.Count -gt 0) { Write-Output 'WARNING: likely build processes are running:'; $processes | Sort-Object Id -Unique | ForEach-Object { Write-Output ('  {0} (PID {1})' -f $_.ProcessName, $_.Id) }; exit 10 }; Write-Output 'No likely active CMake, Ninja, MSBuild, compiler, or linker process was detected.'; exit 0 } catch { Write-Error ('Could not reliably check active build processes: ' + $_); exit 20 }"
if errorlevel 20 goto BuildCheckFailed
if errorlevel 10 goto BuildProcessDetected
exit /b 0

:BuildProcessDetected
echo Do not remove build trees while a build is active. Cancel unless you have verified these processes are idle.
set "PF_ACK="
set /p "PF_ACK=Type I UNDERSTAND to acknowledge the risk and continue: "
if /i not "%PF_ACK%"=="I UNDERSTAND" goto BuildCheckCancelled
set "PF_BUILD_ACK=1"
exit /b 0

:BuildCheckFailed
echo Refusing cleanup because active-build detection failed.
exit /b 1

:BuildCheckCancelled
echo Confirmation did not match. No cleanup was performed.
exit /b 1

:ShowSizes
set "PF_CLEAN_ROOT=%PF_ROOT%"
powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $root = [IO.Path]::GetFullPath($env:PF_CLEAN_ROOT); foreach ($name in @('Build', 'out')) { $path = Join-Path $root $name; if (-not (Test-Path -LiteralPath $path -PathType Container)) { Write-Output ('{0}: not present' -f $name); continue }; try { $measure = Get-ChildItem -LiteralPath $path -Force -Recurse -File -ErrorAction Stop | Measure-Object -Property Length -Sum; $bytes = if ($null -eq $measure.Sum) { [int64]0 } else { [int64]$measure.Sum }; Write-Output ('{0}: {1:N2} GB ({2:N0} bytes, {3:N0} files)' -f $name, ($bytes / 1GB), $bytes, $measure.Count) } catch { Write-Output ('{0}: size unavailable ({1})' -f $name, $_.Exception.Message) } }"
echo.
goto Menu

:CancelCleanup
echo Confirmation did not match. No cleanup was performed.
exit /b 0

:Cancel
echo Cleanup cancelled. No files were removed.
exit /b 0

:InvalidRoot
echo ERROR: CMakeLists.txt was not found beside this script. No files were removed.
exit /b 2
