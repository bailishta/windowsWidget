$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw '需要 Visual Studio C++ Build Tools 和 Windows SDK 10.0.26100.0。' }
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $msbuild
$start.Arguments = '"' + (Join-Path $PSScriptRoot 'BluetoothBattery.vcxproj') + '" /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo'
$start.UseShellExecute = $false; $start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
$start.EnvironmentVariables.Clear()
$normalized = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $normalized[$entry.Key.ToUpperInvariant()] = $entry.Value }
foreach ($key in $normalized.Keys) { $start.EnvironmentVariables[$key] = $normalized[$key] }
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
Write-Host $stdout.GetAwaiter().GetResult(); Write-Host $stderr.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) { throw '蓝牙插件构建失败' }
Write-Host '独立组件包：out/mods/bluetooth-battery（无需编译主程序）'
