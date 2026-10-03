param([ValidateSet('Debug','Release')][string]$Configuration='Release',[switch]$Restore,[switch]$Test,[string]$Project='WindowsWidget.proj')
$ErrorActionPreference='Stop'
$repo = $PSScriptRoot
Set-Location -LiteralPath $repo
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'MSBuild C++ tools not found. Install VS 2022 Build Tools with Desktop development with C++.' }
[xml]$packageList=Get-Content 'packages.config'
$missingPackages=@($packageList.packages.package | Where-Object {!(Test-Path (Join-Path 'packages' ($_.id+'.'+$_.version)))})
if ($Restore -or $missingPackages.Count -gt 0) {
    New-Item -ItemType Directory -Force .tools,packages | Out-Null
    if (!(Test-Path '.tools\nuget.exe')) { Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/v6.14.0/nuget.exe' -OutFile '.tools\nuget.exe' }
    & '.\.tools\nuget.exe' restore 'packages.config' -PackagesDirectory packages -Source https://api.nuget.org/v3/index.json -NonInteractive
    if ($LASTEXITCODE) { throw 'NuGet restore failed' }
}
# Some launcher environments contain both PATH and Path. Normalize for .NET Framework MSBuild.
$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName=$msbuild
$psi.WorkingDirectory=$repo
$psi.UseShellExecute=$false
$psi.CreateNoWindow=$true
$psi.RedirectStandardOutput=$true
$psi.RedirectStandardError=$true
$values=@{}
foreach($entry in $psi.Environment.GetEnumerator()) { $values[$entry.Key]=$entry.Value }
$psi.Environment.Clear()
foreach($key in $values.Keys){$psi.Environment[$key]=$values[$key]}
$psi.Arguments='"'+$Project+'" /m:3 /nr:false /p:Configuration='+$Configuration+' /p:Platform=x64 /v:minimal /nologo'
$process=[System.Diagnostics.Process]::Start($psi)
$stdout=$process.StandardOutput.ReadToEndAsync()
$stderr=$process.StandardError.ReadToEndAsync()
$process.WaitForExit()
$buildOutput=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult()
New-Item -ItemType Directory -Force out | Out-Null
Set-Content 'out\build.log' $buildOutput -Encoding utf8
Write-Output $buildOutput
if($process.ExitCode){exit $process.ExitCode}
if($Project -eq 'WindowsWidget.proj') {
    $output=Join-Path $repo "out\$Configuration"
    New-Item -ItemType Directory -Force "$output\plugins\clock","$output\plugins\calendar","$output\plugins\todo","$output\plugins\weather","$output\sdk\example","$output\ui" | Out-Null
    Copy-Item 'src\manager\MainWindow.xaml' "$output\ui\MainWindow.xaml" -Force
    Copy-Item 'plugins\clock\widget.json',"$output\ClockWidget.dll" -Destination "$output\plugins\clock" -Force
    foreach($p in @('calendar','todo','weather')){Copy-Item "plugins\$p\widget.json" "$output\plugins\$p" -Force}
    Copy-Item 'sdk\WidgetSdk.h' -Destination "$output\sdk" -Force
    Copy-Item 'sdk\example\HelloWidget.cpp','sdk\example\HelloWidget.vcxproj','sdk\example\widget.json' -Destination "$output\sdk\example" -Force
    if(Test-Path 'docs\SDK.md'){Copy-Item 'docs\SDK.md' "$output\sdk\README.md" -Force}
    # The development host is built straight into sdk\devhost by its own project.
    if(Test-Path 'docs\PLUGIN_DEV.md'){Copy-Item 'docs\PLUGIN_DEV.md' "$output\sdk" -Force}
    $vsRoot=& $vswhere -latest -products '*' -property installationPath
    # Sort by the toolset version, not the path: a lexicographic sort puts 14.9 above 14.44.
    $crt=Get-ChildItem "$vsRoot\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory | Sort-Object -Descending -Property @{Expression={ $parsed=$_.Parent.Parent.Name -as [version]; if($parsed){$parsed}else{[version]'0.0'} }} | Select-Object -First 1
    if($crt){Copy-Item "$($crt.FullName)\*.dll" $output -Force}
    if($Test){& "$output\WidgetTests.exe" 2>&1 | Tee-Object -FilePath 'out\test-results.txt';if($LASTEXITCODE){exit $LASTEXITCODE}}
}
