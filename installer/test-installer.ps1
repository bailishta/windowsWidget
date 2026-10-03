param([string]$Installer = '')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$Installer) { $Installer = Join-Path $repo 'out/installers/WindowsWidget-0.1.0-x64-Setup.exe' }
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{914F343C-4302-4BD7-83A4-385AF6D70448}_is1'
if (Test-Path -LiteralPath $uninstallKey) { throw 'An installed WindowsWidget already exists; use a clean test account or VM.' }
$mutex = $null
if ([Threading.Mutex]::TryOpenExisting('Local\WindowsWidget.CompanionDemo.Singleton', [ref]$mutex)) {
    $mutex.Dispose(); throw 'Quit the running WindowsWidget before this isolated test.'
}
$id = [Guid]::NewGuid().ToString('N')
$testRoot = Join-Path $repo ('out/installer-tests/' + $id)
$appDir = Join-Path $testRoot '安装 测试'
$dataDir = Join-Path $testRoot 'isolated-data'
$menuGroup = 'WindowsWidget Installer Test ' + $id
$menuDir = Join-Path ([Environment]::GetFolderPath('Programs')) $menuGroup
New-Item -ItemType Directory -Path $testRoot,$dataDir -Force | Out-Null
$report = [ordered]@{ installer = $Installer; testRoot = $testRoot; assertions = 0; checks = @() }
function Assert-Test([bool]$Condition, [string]$Description) {
    if (!$Condition) { throw "Failed: $Description" }
    $report.assertions++; $report.checks += $Description
}
function Invoke-Setup([string]$LogName, [switch]$Upgrade) {
    $arguments = '/VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /LANG=chinesesimplified /TASKS="" /DIR="' + $appDir + '" /GROUP="' + $menuGroup + '" /LOG="' + (Join-Path $testRoot $LogName) + '"'
    $p = Start-Process -FilePath $Installer -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$p.WaitForExit(120000)) { throw 'Installer test timed out.' }
    Assert-Test ($p.ExitCode -eq 0) "Setup $LogName exits successfully"
}
function User-DataSnapshot {
    $root = Join-Path $env:LOCALAPPDATA 'WindowsWidget/CompanionDemo'
    $snapshot = @{}
    if (Test-Path -LiteralPath $root) {
        Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.json' | ForEach-Object {
            $snapshot[$_.FullName] = (Get-FileHash -LiteralPath $_.FullName).Hash
        }
    }
    return $snapshot
}
function Start-IsolatedApp {
    $ready = Join-Path $dataDir 'plugin-data/builtin.todo/companion.jsonl'
    if (Test-Path -LiteralPath $ready) { Remove-Item -LiteralPath $ready }
    $p = Start-Process -FilePath (Join-Path $appDir 'WindowsWidget.exe') -ArgumentList ('--background --data-dir "' + $dataDir + '"') -WindowStyle Hidden -PassThru
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ($p.HasExited) { throw "Installed app exited unexpectedly: $($p.ExitCode)" }
        if ((Test-Path -LiteralPath $ready) -and ((Get-Content -LiteralPath $ready -Raw) -match 'plugin_ready')) { break }
        Start-Sleep -Milliseconds 200
    }
    Assert-Test (!$p.HasExited) 'Installed app starts with isolated data'
    Assert-Test ((Test-Path -LiteralPath $ready) -and ((Get-Content -LiteralPath $ready -Raw) -match 'plugin_ready')) 'Independent todo component starts'
    return $p
}
$before = User-DataSnapshot
$runBefore = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'WindowsWidget.Companion' -ErrorAction SilentlyContinue).'WindowsWidget.Companion'
try {
    Invoke-Setup 'install.log'
    Assert-Test (Test-Path -LiteralPath $uninstallKey) 'Per-user uninstall registration exists'
    Assert-Test (Test-Path -LiteralPath (Join-Path $menuDir '随行组件.lnk')) 'Start menu shortcut exists'
    $metadata = Get-Content -LiteralPath ($Installer + '.json') -Raw | ConvertFrom-Json
    Assert-Test ((Get-FileHash -LiteralPath (Join-Path $appDir 'WindowsWidget.exe')).Hash -eq $metadata.sourceExecutableSha256) 'Installed executable matches tested build'
    foreach ($component in @('weather','todo','quick','system')) {
        $definition = Get-Content -LiteralPath (Join-Path $appDir "components/$component/widget.json") -Raw | ConvertFrom-Json
        Assert-Test (Test-Path -LiteralPath (Join-Path $appDir "components/$component/$($definition.entry)")) "$component executable installed"
        Assert-Test (Test-Path -LiteralPath (Join-Path $appDir "components/$component/Microsoft.UI.Xaml.dll")) "$component WinUI runtime installed"
    }
    Assert-Test (@(Get-ChildItem -LiteralPath $appDir -Recurse -File | Where-Object Extension -in @('.pdb','.ilk','.lib','.exp')).Count -eq 0) 'No debug build artifacts installed'
    $pluginDir = Join-Path $dataDir 'plugins/builtin.todo'
    New-Item -ItemType Directory -Path (Split-Path -Parent $pluginDir) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $appDir 'components/todo') -Destination $pluginDir -Recurse
    '{"schema":1,"library_version":1,"hide_tray_icon":true,"plugins":[{"id":"builtin.todo","enabled":true}]}' | Set-Content -LiteralPath (Join-Path $dataDir 'manager.json') -Encoding UTF8
    $sentinel = Join-Path $dataDir 'keep-user-data.txt'
    'Preserve this user data across install, upgrade and uninstall.' | Set-Content -LiteralPath $sentinel
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
    $extraFile = Join-Path $appDir 'keep-unowned-file.txt'
    'Not owned by the installer.' | Set-Content -LiteralPath $extraFile
    $appProcess = Start-IsolatedApp
    $todoProcesses = @(Get-Process -Name TodoComponent -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Join-Path $pluginDir 'TodoComponent.exe') })
    Assert-Test ($todoProcesses.Count -eq 1) 'One independent todo process runs'
    Invoke-Setup 'upgrade.log' -Upgrade
    Assert-Test ($appProcess.WaitForExit(15000)) 'Upgrade gracefully stops the manager'
    foreach ($child in $todoProcesses) { Assert-Test ($child.WaitForExit(15000)) 'Upgrade stops the component process' }
    Assert-Test ((Get-FileHash -LiteralPath $sentinel).Hash -eq $sentinelHash) 'Upgrade preserves isolated user data'
    Assert-Test (Test-Path -LiteralPath $extraFile) 'Upgrade preserves unowned files'
    $appProcess = Start-IsolatedApp
    $uninstall = Join-Path $appDir 'unins000.exe'
    $p = Start-Process -FilePath $uninstall -ArgumentList ('/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG="' + (Join-Path $testRoot 'uninstall.log') + '"') -WindowStyle Hidden -PassThru
    if (!$p.WaitForExit(120000)) { throw 'Uninstaller test timed out.' }
    Assert-Test ($p.ExitCode -eq 0) 'Uninstaller exits successfully'
    Assert-Test ($appProcess.WaitForExit(15000)) 'Uninstall gracefully stops the manager'
    Assert-Test (!(Test-Path -LiteralPath (Join-Path $appDir 'WindowsWidget.exe'))) 'Uninstall removes the application executable'
    Assert-Test (!(Test-Path -LiteralPath $uninstallKey)) 'Uninstall removes its registration'
    Assert-Test (!(Test-Path -LiteralPath (Join-Path $menuDir '随行组件.lnk'))) 'Uninstall removes its shortcut'
    Assert-Test ((Get-FileHash -LiteralPath $sentinel).Hash -eq $sentinelHash) 'Uninstall preserves isolated user data'
    Assert-Test (Test-Path -LiteralPath (Join-Path $dataDir 'manager.json')) 'Uninstall preserves component settings'
    Assert-Test (Test-Path -LiteralPath (Join-Path $pluginDir 'widget.json')) 'Uninstall preserves imported component packages'
    Assert-Test (Test-Path -LiteralPath $extraFile) 'Uninstall preserves unowned files'
    $after = User-DataSnapshot
    Assert-Test ($before.Count -eq $after.Count) 'Real user data file count is unchanged'
    foreach ($path in $before.Keys) { Assert-Test ($before[$path] -eq $after[$path]) 'Real user JSON data remains unchanged' }
    $runAfter = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'WindowsWidget.Companion' -ErrorAction SilentlyContinue).'WindowsWidget.Companion'
    Assert-Test ($runBefore -eq $runAfter) 'Existing autostart registration is unchanged'
    $report.passed = $true
} catch {
    $report.passed = $false; $report.error = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $testRoot 'validation.json') -Encoding UTF8
}
Write-Host "Installer lifecycle tests passed: $($report.assertions). Report: $(Join-Path $testRoot 'validation.json')"
