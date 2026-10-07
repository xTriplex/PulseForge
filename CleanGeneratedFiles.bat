@echo off
setlocal EnableExtensions DisableDelayedExpansion

for %%I in ("%~dp0.") do set "PF_ROOT=%%~fI"
if not exist "%PF_ROOT%\CMakeLists.txt" goto invalidRoot

set "PF_BUILD_TARGET=%PF_ROOT%\Build"
set "PF_OUT_TARGET=%PF_ROOT%\out"
for %%I in ("%PF_BUILD_TARGET%") do set "PF_BUILD_TARGET=%%~fI"
for %%I in ("%PF_OUT_TARGET%") do set "PF_OUT_TARGET=%%~fI"

echo PulseForge generated-files maintenance
echo Repository: %PF_ROOT%
echo.
echo A complete Build cleanup also removes the local CMake cache and fetched dependencies,
echo including dependencies such as NVRHI, Vulkan-Headers, Lua, and DXC when fetch-managed.
echo Reconfiguration may need network access and a full rebuild. Cleanup is never automatic.
echo.

:menu
echo [1] Clean canonical Build directory
echo [2] Clean Visual Studio out directory
echo [3] Deep clean both Build and out
echo [4] Show generated directory sizes
echo [5] Cancel
choice /C 12345 /N /M "Select an option: "
if errorlevel 5 goto cancel
if errorlevel 4 goto showSizes
if errorlevel 3 goto cleanBoth
if errorlevel 2 goto cleanOut
if errorlevel 1 goto cleanBuild
goto menu

:cleanBuild
set "PF_TARGET_NAME=Build"
set "PF_TARGET=%PF_BUILD_TARGET%"
set "PF_CONFIRMATION=DELETE BUILD"
goto cleanOne

:cleanOut
set "PF_TARGET_NAME=out"
set "PF_TARGET=%PF_OUT_TARGET%"
set "PF_CONFIRMATION=DELETE OUT"
goto cleanOne

:cleanOne
echo.
echo Action: recursively delete this generated directory.
echo Full resolved path: %PF_TARGET%
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $root=[IO.Path]::GetFullPath($env:PF_ROOT).TrimEnd('\')+'\'; if(-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt') -PathType Leaf)){Write-Error 'CMakeLists.txt is missing at the repository root.'; exit 1}; function AssertSafe([string]$Path){$full=[IO.Path]::GetFullPath($Path); if(-not $full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing a target outside the repository root.'}; if(Test-Path -LiteralPath $full){$item=Get-Item -LiteralPath $full -Force; if(-not $item.PSIsContainer){throw 'Refusing to clean a non-directory target.'}; $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($full); while($pending.Count -gt 0){$directory=$pending.Pop(); foreach($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)){$attributes=[IO.File]::GetAttributes($entry); if(($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw ('Refusing a tree containing a junction or symbolic link: ' + $entry)}; if(($attributes -band [IO.FileAttributes]::Directory) -ne 0){$pending.Push($entry)}}}}; return $full}; function GetSize([string]$Path){if(-not (Test-Path -LiteralPath $Path -PathType Container)){return 0L}; $sum=(Get-ChildItem -LiteralPath $Path -File -Force -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum; if($null -eq $sum){return 0L}; return [long]$sum}; try{$target=AssertSafe $env:PF_TARGET; if(-not (Test-Path -LiteralPath $target -PathType Container)){Write-Host ($env:PF_TARGET_NAME + ' does not exist; nothing to clean.'); exit 0}; $bytes=GetSize $target; Write-Host ('Approximate size: {0:N2} GiB ({1:N0} bytes)' -f ($bytes/1GB),$bytes); $active=@(Get-Process -Name @('cmake','ninja','MSBuild','cl','link') -ErrorAction SilentlyContinue | Sort-Object Id -Unique); if($active.Count -gt 0){Write-Warning 'A build may be active. Deleting build trees could corrupt it.'; foreach($process in $active){Write-Host ('  {0}.exe (PID {1})' -f $process.ProcessName,$process.Id)}; if((Read-Host 'Type I UNDERSTAND ACTIVE BUILDS MAY BE DAMAGED to continue') -cne 'I UNDERSTAND ACTIVE BUILDS MAY BE DAMAGED'){Write-Host 'Confirmation did not match; canceled.'; exit 1}}; $expected=$env:PF_CONFIRMATION; if((Read-Host ('Type ' + $expected + ' to continue')) -cne $expected){Write-Host 'Confirmation did not match; canceled.'; exit 1}; Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop; if(Test-Path -LiteralPath $target){throw 'The target still exists after cleanup.'}; Write-Host ('Approximate file data removed: {0:N2} GiB ({1:N0} bytes). Actual free-space change may differ.' -f ($bytes/1GB),$bytes); exit 0}catch{Write-Error $_; exit 1}"
if errorlevel 1 echo Cleanup aborted. No further action was taken.
goto menu

:cleanBoth
echo.
echo DEEP CLEAN: recursively delete both generated directories:
echo   %PF_BUILD_TARGET%
echo   %PF_OUT_TARGET%
echo This removes build outputs, CMake caches, and fetched dependency copies.
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $root=[IO.Path]::GetFullPath($env:PF_ROOT).TrimEnd('\')+'\'; if(-not (Test-Path -LiteralPath (Join-Path $root 'CMakeLists.txt') -PathType Leaf)){Write-Error 'CMakeLists.txt is missing at the repository root.'; exit 1}; function AssertSafe([string]$Path){$full=[IO.Path]::GetFullPath($Path); if(-not $full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing a target outside the repository root.'}; if(Test-Path -LiteralPath $full){$item=Get-Item -LiteralPath $full -Force; if(-not $item.PSIsContainer){throw 'Refusing to clean a non-directory target.'}; $pending=[Collections.Generic.Stack[string]]::new(); $pending.Push($full); while($pending.Count -gt 0){$directory=$pending.Pop(); foreach($entry in [IO.Directory]::EnumerateFileSystemEntries($directory)){$attributes=[IO.File]::GetAttributes($entry); if(($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw ('Refusing a tree containing a junction or symbolic link: ' + $entry)}; if(($attributes -band [IO.FileAttributes]::Directory) -ne 0){$pending.Push($entry)}}}}; return $full}; function GetSize([string]$Path){if(-not (Test-Path -LiteralPath $Path -PathType Container)){return 0L}; $sum=(Get-ChildItem -LiteralPath $Path -File -Force -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum; if($null -eq $sum){return 0L}; return [long]$sum}; try{$targets=@(@{Name='Build';Path=$env:PF_BUILD_TARGET},@{Name='out';Path=$env:PF_OUT_TARGET}); foreach($item in $targets){$item.Path=AssertSafe $item.Path; Write-Host ('Target: ' + $item.Path); $item.Bytes=GetSize $item.Path; Write-Host ('  Approximate size: {0:N2} GiB ({1:N0} bytes)' -f ($item.Bytes/1GB),$item.Bytes)}; $active=@(Get-Process -Name @('cmake','ninja','MSBuild','cl','link') -ErrorAction SilentlyContinue | Sort-Object Id -Unique); if($active.Count -gt 0){Write-Warning 'A build may be active. Deleting build trees could corrupt it.'; foreach($process in $active){Write-Host ('  {0}.exe (PID {1})' -f $process.ProcessName,$process.Id)}; if((Read-Host 'Type I UNDERSTAND ACTIVE BUILDS MAY BE DAMAGED to continue') -cne 'I UNDERSTAND ACTIVE BUILDS MAY BE DAMAGED'){Write-Host 'Confirmation did not match; canceled.'; exit 1}}; if((Read-Host 'Deep clean permanently removes both generated trees. Type DELETE BOTH to continue') -cne 'DELETE BOTH'){Write-Host 'Confirmation did not match; canceled.'; exit 1}; foreach($item in $targets){if(Test-Path -LiteralPath $item.Path -PathType Container){Remove-Item -LiteralPath $item.Path -Recurse -Force -ErrorAction Stop; if(Test-Path -LiteralPath $item.Path){throw ('The target still exists: ' + $item.Path)}; Write-Host ('Approximate file data removed from {0}: {1:N2} GiB ({2:N0} bytes).' -f $item.Name,($item.Bytes/1GB),$item.Bytes)}}; exit 0}catch{Write-Error $_; exit 1}"
if errorlevel 1 echo Deep clean aborted or partially completed. Review the diagnostics above.
goto menu

:showSizes
echo.
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $root=[IO.Path]::GetFullPath($env:PF_ROOT); foreach($name in 'Build','out'){$path=Join-Path $root $name; if(Test-Path -LiteralPath $path -PathType Container){$item=Get-Item -LiteralPath $path -Force; if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){Write-Output ($name + ': not measured (junction or symbolic link)'); continue}; $sum=(Get-ChildItem -LiteralPath $path -File -Force -Recurse -ErrorAction SilentlyContinue | Measure-Object -Property Length -Sum).Sum; if($null -eq $sum){$sum=0}; Write-Output ('{0}: approximately {1:N2} GiB ({2:N0} bytes)' -f $name,($sum/1GB),$sum)}else{Write-Output ($name + ': not present')}}"
echo.
goto menu

:cancel
echo Cleanup canceled. No additional action was taken.
exit /b 0

:invalidRoot
echo ERROR: CMakeLists.txt was not found beside this script. No files were deleted.
exit /b 2
