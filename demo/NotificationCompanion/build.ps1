param([switch]$Test, [switch]$Package, [switch]$ReuseModules, [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = 'ControlCenter')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Install Visual Studio 2022 C++ Build Tools, Windows SDK 10.0.26100.0.' }
[xml]$dependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'packages.config')
foreach ($dep in $dependencies.packages.package) {
    if (!(Test-Path -LiteralPath (Join-Path $repo "packages/$($dep.id).$($dep.version)"))) {
        throw "Missing pinned dependency: $($dep.id) $($dep.version). Restore packages.config with NuGet into the repository packages directory."
    }
}
function Invoke-DemoBuild([string]$projectPath, [string]$targetOutput = $OutputName) {
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $msbuild
$start.Arguments = '"' + $projectPath + '" /m /p:Configuration=Release /p:Platform=x64 /p:DemoOutputName=' + $targetOutput + ' /v:minimal /nologo'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
# Normalize environment key casing for MSBuild's .NET Framework child tools.
$start.EnvironmentVariables.Clear()
$normalized = @{}
foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $normalized[$entry.Key.ToUpperInvariant()] = $entry.Value }
foreach ($key in $normalized.Keys) { $start.EnvironmentVariables[$key] = $normalized[$key] }
$process = [Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
$process.WaitForExit()
Write-Host $stdout.GetAwaiter().GetResult()
Write-Host $stderr.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) { throw "Build failed: $($process.ExitCode)" }
}
if ($Test) { Invoke-DemoBuild (Join-Path $PSScriptRoot 'DiagnosticRunner.vcxproj') }
Invoke-DemoBuild (Join-Path $repo 'plugins/bluetooth-battery/BluetoothBattery.vcxproj')
Invoke-DemoBuild (Join-Path $PSScriptRoot 'CompanionDemo.vcxproj')
foreach ($component in @('weather','todo','quick','system')) {
    if (!$ReuseModules) {
        Invoke-DemoBuild (Join-Path $repo "plugins/companion/$component/Component.vcxproj") ($OutputName + '-Module-' + $component)
    } else {
        # Reuse only when runtime source is unchanged, e.g. correcting hidden-test expectations.
        # Module executables reject self-test/runtime-test arguments at their entry points.
        $moduleManifest = Get-Content -LiteralPath (Join-Path $repo "plugins/companion/$component/widget.json") -Raw | ConvertFrom-Json
        $moduleExe = Get-Item -LiteralPath (Join-Path $repo ('out/companion-demo/' + $OutputName + '-Module-' + $component + '/' + $moduleManifest.entry))
        $runtimeSources = @(Get-ChildItem -LiteralPath $PSScriptRoot -File | Where-Object { $_.Extension -in @('.h','.inc') -and $_.Name -notmatch 'Tests\.inc$' })
        $runtimeSources += Get-Item -LiteralPath (Join-Path $PSScriptRoot 'main.cpp'),(Join-Path $repo "plugins/companion/$component/Component.cpp"),(Join-Path $repo 'sdk/WidgetSdk.h'),(Join-Path $repo 'sdk/WidgetUi.h')
        if ($runtimeSources | Where-Object LastWriteTimeUtc -gt $moduleExe.LastWriteTimeUtc) { throw 'Module runtime source changed; rebuild without -ReuseModules.' }
        Write-Host "Reusing current runtime module: $($moduleExe.Name)"
    }
}
$output = Join-Path $repo ('out/companion-demo/' + $OutputName)
$vsroot = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
$redistRoot = Join-Path $vsroot 'VC/Redist/MSVC'
$redist = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$redist) { throw 'MSVC Redistributable not found.' }
$crt = Join-Path $redist.FullName 'x64/Microsoft.VC143.CRT'
Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $output
$licenseRoot = Join-Path $output 'licenses'
foreach ($dep in $dependencies.packages.package) {
    $source = Join-Path $repo "packages/$($dep.id).$($dep.version)"
    $notices = Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Name -match '(?i)(license|notice)' }
    if ($notices) {
        $target = Join-Path $licenseRoot "$($dep.id).$($dep.version)"
        New-Item -ItemType Directory -Path $target -Force | Out-Null
        $notices | Copy-Item -Destination $target
    }
}
New-Item -ItemType Directory -Path (Join-Path $output 'plugins') -Force | Out-Null
# Optional, independently compiled packages. Each carries its own WinUI runtime and can be imported as a mod.
foreach ($component in @('weather','todo','quick','system')) {
    $moduleOutput = Join-Path $repo ('out/companion-demo/' + $OutputName + '-Module-' + $component)
    $componentPackage = Join-Path $output ('components/' + $component)
    New-Item -ItemType Directory -Path $componentPackage -Force | Out-Null
    Get-ChildItem -LiteralPath $moduleOutput | Where-Object { $_.Extension -notin @('.pdb','.ilk','.lib','.exp') } | Copy-Item -Destination $componentPackage -Recurse -Force
    Get-ChildItem -LiteralPath $crt -Filter '*.dll' | Copy-Item -Destination $componentPackage -Force
    Copy-Item -LiteralPath $licenseRoot -Destination $componentPackage -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repo "plugins/companion/$component/widget.json") -Destination $componentPackage -Force
    Copy-Item -LiteralPath (Join-Path $repo "assets/icons/$component.png") -Destination (Join-Path $componentPackage 'icon.png') -Force
    $moduleIcons = Join-Path $componentPackage 'icons'
    New-Item -ItemType Directory -Path $moduleIcons -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $repo 'assets/icons') -File | Where-Object { $_.Extension -in @('.png','.ico') } | Copy-Item -Destination $moduleIcons -Force
}
$iconOutput = Join-Path $output 'icons'
New-Item -ItemType Directory -Path $iconOutput -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repo 'assets/icons') -File | Where-Object { $_.Extension -in @('.png','.ico') } | Copy-Item -Destination $iconOutput -Force
Copy-Item -LiteralPath (Join-Path $repo 'sdk/components/README.md') -Destination (Join-Path $output 'plugins/README.md') -Force
Copy-Item -LiteralPath (Join-Path $repo 'sdk/WidgetSdk.h') -Destination (Join-Path $output 'plugins/WidgetSdk.h') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PluginProcess.h') -Destination (Join-Path $output 'plugins/PluginProcess.h') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PluginCatalog.h') -Destination (Join-Path $output 'plugins/PluginCatalog.h') -Force
Copy-Item -LiteralPath (Join-Path $repo 'sdk/components/widget.schema.json') -Destination (Join-Path $output 'plugins/widget.schema.json') -Force
# Copy-Item merges folders; explicitly clear retired examples from cached build outputs.
foreach ($retired in @('my-checklist','my-links')) {
    $retiredOutput = [IO.Path]::GetFullPath((Join-Path $output ('examples/' + $retired)))
    $outputRoot = [IO.Path]::GetFullPath($output).TrimEnd('\') + '\'
    if (!$retiredOutput.StartsWith($outputRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Retired example path escapes build output.' }
    if (Test-Path -LiteralPath $retiredOutput) {
        if ((Get-Item -LiteralPath $retiredOutput).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Retired output is a reparse point.' }
        Remove-Item -LiteralPath $retiredOutput -Recurse -Force
    }
}
Copy-Item -LiteralPath (Join-Path $repo 'sdk/components/examples') -Destination $output -Recurse -Force
$modExample = Join-Path $output 'examples/bluetooth-battery'
New-Item -ItemType Directory -Path $modExample -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'out/mods/bluetooth-battery/BluetoothBattery.dll') -Destination $modExample -Force
Copy-Item -LiteralPath (Join-Path $repo 'plugins/bluetooth-battery/widget.json') -Destination $modExample -Force
Copy-Item -LiteralPath (Join-Path $repo 'assets/icons/bluetooth-battery.png') -Destination (Join-Path $modExample 'icon.png') -Force
Copy-Item -LiteralPath (Join-Path $repo 'sdk/components/widget.schema.json') -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $output 'README.md')
if ($Test) {
    $runs = @()
    foreach ($language in @('zh', 'en')) {
        $data = Join-Path $repo ('out/companion-demo/test-results/' + $language + '-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $data -Force | Out-Null
        $arguments = '--self-test --data-dir "' + $data + '"'
        if ($language -eq 'en') { $arguments += ' --en' }
        $testStart = [Diagnostics.ProcessStartInfo]::new()
        $testStart.FileName = Join-Path $repo 'out/companion-demo/diagnostics/DiagnosticRunner.exe'
        $testStart.Arguments = '"' + (Join-Path $output 'WindowsWidget.exe') + '" ' + $arguments
        $testStart.UseShellExecute = $false; $testStart.CreateNoWindow = $true
        $testStart.RedirectStandardError = $true; $testStart.RedirectStandardOutput = $true
        $testProcess = [Diagnostics.Process]::Start($testStart)
        $diagnosticErrors = $testProcess.StandardError.ReadToEndAsync()
        $diagnosticOutput = $testProcess.StandardOutput.ReadToEndAsync()
        $testProcess.WaitForExit()
        $diagnosticErrors.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $data 'debugger.txt') -Encoding UTF8
        $diagnosticOutput.GetAwaiter().GetResult() | Add-Content -LiteralPath (Join-Path $data 'debugger.txt') -Encoding UTF8
        if ($testProcess.ExitCode -ne 0) { throw "Self-test failed ($language): $($testProcess.ExitCode). Logs: $data" }
        $records = Get-Content -LiteralPath (Join-Path $data 'companion.jsonl') | ForEach-Object { $_ | ConvertFrom-Json }
        $pass = $records | Where-Object { $_.event -eq 'self_test_pass' } | Select-Object -Last 1
        if (!$pass -or $pass.assertions -lt 90) { throw "Self-test completion missing: $data" }
        $startupPass = $records | Where-Object { $_.event -eq 'startup_settings_test_pass' } | Select-Object -Last 1
        if (!$startupPass -or $startupPass.assertions -lt 30) { throw "Startup settings tests did not complete: $data" }
        $runs += [ordered]@{ language = $language; assertions = $pass.assertions; exitCode = $testProcess.ExitCode; nativeSamples = $pass.native_samples; uiaPasses = $pass.uia_passes; nativeSampleAgeMs = $pass.native_sample_age_ms; uiaDiagnosticDelayMs = $pass.uia_diagnostic_delay_ms; acrylicAttached = $pass.acrylic_attached; motionFrameRequests = $pass.motion_frame_requests; log = (Join-Path $data 'companion.jsonl') }
        $runs[-1].startupSettingsAssertions = $startupPass.assertions
    }
    $runtimeData = Join-Path $repo ('out/companion-demo/test-results/runtime-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $runtimeData -Force | Out-Null
    $runtimeStart = [Diagnostics.ProcessStartInfo]::new()
    $runtimeStart.FileName = Join-Path $repo 'out/companion-demo/diagnostics/DiagnosticRunner.exe'
    $runtimeStart.Arguments = '"' + (Join-Path $output 'WindowsWidget.exe') + '" --runtime-test --data-dir "' + $runtimeData + '"'
    $runtimeStart.UseShellExecute = $false; $runtimeStart.CreateNoWindow = $true
    $runtimeStart.RedirectStandardError = $true; $runtimeStart.RedirectStandardOutput = $true
    $runtimeProcess = [Diagnostics.Process]::Start($runtimeStart)
    $runtimeErrors = $runtimeProcess.StandardError.ReadToEndAsync()
    $runtimeOutput = $runtimeProcess.StandardOutput.ReadToEndAsync()
    $runtimeProcess.WaitForExit()
    $runtimeErrors.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $runtimeData 'debugger.txt') -Encoding UTF8
    $runtimeOutput.GetAwaiter().GetResult() | Add-Content -LiteralPath (Join-Path $runtimeData 'debugger.txt') -Encoding UTF8
    if ($runtimeProcess.ExitCode -ne 0) { throw "Independent plugin test failed: $($runtimeProcess.ExitCode). Logs: $runtimeData" }
    $runtimeRecords = Get-Content -LiteralPath (Join-Path $runtimeData 'companion.jsonl') | ForEach-Object { $_ | ConvertFrom-Json }
    $runtimePass = $runtimeRecords | Where-Object { $_.event -eq 'runtime_test_pass' } | Select-Object -Last 1
    if (!$runtimePass) { throw "Independent plugin test did not complete: $runtimeData" }
    $hidePass = $runtimeRecords | Where-Object { $_.event -eq 'manager_hide_test_pass' } | Select-Object -Last 1
    if (!$hidePass -or $hidePass.assertions -lt 8) { throw "Manager hiding tests did not complete: $runtimeData" }
    $os = Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion'
    $report = [ordered]@{ time = [DateTime]::Now.ToString('o'); build = 'inline-editing-1'; output = $output; os = "$($os.DisplayVersion) $($os.CurrentBuild).$($os.UBR)"; msbuild = $msbuild; runs = $runs; mouseTests = 'Independent widget windows: actual Shell retention pending user'; fullscreenTests = 'Previous FollowFix confirmed by user; independent cards pending user'; layout = '8 DIP gap, bottom right packing, negative origins, 150 percent DPI, reserved Shell and free drag constraints checked'; acrylicAndAnimation = 'Shared animation clock and native frame broker; hidden tests do not measure pixels or real Shell synchrony'; shellClassification = 'Only confirmed notification/quick-settings identities trigger; banner regression replayed' }
    $report.independentPlugins = [ordered]@{ assertions = $runtimePass.assertions; exitCode = $runtimeProcess.ExitCode; log = (Join-Path $runtimeData 'companion.jsonl') }
    $report.startup = [ordered]@{ registry = 'Enable, replace, disable and unrelated-value preservation tested in an isolated volatile HKCU key; real Run key not changed'; preferences = 'Silent preference reload, Unicode command quoting, corrupt settings preservation, atomic save and failed writes checked'; managerHideAssertions = $hidePass.assertions; logon = 'Real sign-out/reboot and tray mouse tests pending user' }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $repo 'out/companion-demo/validation.json') -Encoding UTF8
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'validation.json') -Encoding UTF8
    Write-Host 'Hidden WinUI self-tests passed for Chinese and English.'
}
if ($Package) {
    $destination = Join-Path $repo ('out/packages/WindowsWidget-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Get-ChildItem -LiteralPath $output | Where-Object { $_.Extension -notin @('.pdb', '.lib', '.exp') } | Copy-Item -Destination $destination -Recurse
    Copy-Item -LiteralPath (Join-Path $repo 'docs') -Destination (Join-Path $destination 'docs') -Recurse
    Copy-Item -LiteralPath (Join-Path $repo 'docs/THIRD_PARTY.md') -Destination $destination
    $validation = Join-Path $repo 'out/companion-demo/validation.json'
    if (Test-Path -LiteralPath $validation) {
        $lastReport = Get-Content -LiteralPath $validation -Raw | ConvertFrom-Json
        if ($lastReport.output -eq $output) { Copy-Item -LiteralPath $validation -Destination (Join-Path $destination 'validation.json') }
    }
    Write-Host "Directory package: $destination"
}
Write-Host "Executable: $(Join-Path $output 'WindowsWidget.exe')"
