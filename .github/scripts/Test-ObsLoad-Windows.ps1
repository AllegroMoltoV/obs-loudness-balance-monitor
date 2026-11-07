[CmdletBinding()]
param(
    [ValidateSet('RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $ArchivePath,
    [string] $PackagePath,
    [string] $OutputRoot,
    [ValidateRange(20, 300)]
    [int] $TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_ENVIRONMENT -ne 'github-hosted') {
    throw 'Normal-install verification requires a fresh GitHub-hosted runner.'
}
$ConfigDir = Join-Path $env:APPDATA 'obs-studio'
$SystemPlugins = Join-Path $env:ProgramData 'obs-studio/plugins'
$ProductInstall = Join-Path $SystemPlugins 'loudness-balance-monitor'
$ProbeInstall = Join-Path $SystemPlugins 'lbm-baseline-probe'
foreach ($Existing in @($ConfigDir, $ProductInstall, $ProbeInstall)) {
    if (Test-Path -LiteralPath $Existing) { throw "Fresh runner required; existing state: $Existing" }
}
if (@(Get-Process -Name obs64 -ErrorAction SilentlyContinue).Count) { throw 'An OBS process already exists.' }
foreach ($Variable in @('OBS_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_PATH', 'OBS_LEGACY_PLUGINS_DATA_PATH')) {
    if ([Environment]::GetEnvironmentVariable($Variable)) { throw "External plugin search path is set: $Variable" }
}

$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputRoot) { $OutputRoot = Join-Path $ProjectRoot '.deps/obs-load-windows' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'OutputRoot already exists. Use a new directory to preserve evidence.' }
$BuildSpec = Get-Content -LiteralPath (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
if (!$PackagePath) { $PackagePath = Join-Path $ProjectRoot "release/$($BuildSpec.name)-$($BuildSpec.version)-windows-x64.zip" }
$ProbeDll = Join-Path $ProjectRoot "build_x64/baseline-probe/$Configuration/lbm-baseline-probe.dll"
foreach ($File in @($PackagePath, $ProbeDll)) {
    if (!(Test-Path -LiteralPath $File -PathType Leaf)) { throw "Missing built module: $File" }
}
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$PackageDir = Join-Path $OutputRoot 'package'
Expand-Archive -LiteralPath $PackagePath -DestinationPath $PackageDir
$ProductDll = Join-Path $PackageDir 'loudness-balance-monitor/loudness-balance-monitor.dll'
$ProductData = Join-Path $PackageDir 'loudness-balance-monitor/data'
if (!(Test-Path -LiteralPath $ProductDll -PathType Leaf) -or !(Test-Path -LiteralPath $ProductData -PathType Container)) { throw 'Unexpected product package layout.' }
if (Test-Path -LiteralPath (Join-Path $PackageDir 'loudness-balance-monitor/bin/64bit')) { throw 'Legacy binary layout found in the new distribution.' }
$OldPackagePath = Join-Path $OutputRoot 'loudness-balance-monitor-1.0.1-windows-x64.zip'
Invoke-WebRequest -Uri 'https://github.com/AllegroMoltoV/obs-loudness-balance-monitor/releases/download/v1.0.1/loudness-balance-monitor-1.0.1-windows-x64.zip' -OutFile $OldPackagePath
$OldPackageHash = (Get-FileHash -LiteralPath $OldPackagePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($OldPackageHash -ne '755c85def433f7807534c22d2b15d46c4e76ca16fe7b26a302d0c689e9c99ad5') { throw 'Official v1.0.1 package checksum mismatch.' }
$OldPackageDir = Join-Path $OutputRoot 'old-package'
Expand-Archive -LiteralPath $OldPackagePath -DestinationPath $OldPackageDir
$OldProductDll = Join-Path $OldPackageDir 'loudness-balance-monitor/bin/64bit/loudness-balance-monitor.dll'
if (!(Test-Path -LiteralPath $OldProductDll -PathType Leaf)) { throw 'Unexpected v1.0.1 package layout.' }
$ResultDir = Join-Path $OutputRoot 'results'
New-Item -ItemType Directory -Path $ResultDir | Out-Null
if (!$ArchivePath) {
    $ArchivePath = Join-Path $OutputRoot 'OBS-Studio-33.0.1-Windows-x64.zip'
    Invoke-WebRequest -Uri 'https://github.com/obsproject/obs-studio/releases/download/33.0.1/OBS-Studio-33.0.1-Windows-x64.zip' -OutFile $ArchivePath
}
$ExpectedHash = '0d9436451c2d772b33428e3fdae9776f071895c335de17283176306fb72651d3'
$ActualHash = (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ActualHash -ne $ExpectedHash) { throw 'Official OBS runtime archive checksum mismatch.' }
$Runtime = Join-Path $OutputRoot 'runtime'
Expand-Archive -LiteralPath $ArchivePath -DestinationPath $Runtime
$BinaryDir = Join-Path $Runtime 'bin/64bit'
$ObsExe = Join-Path $BinaryDir 'obs64.exe'
if (!(Test-Path -LiteralPath $ObsExe -PathType Leaf)) { throw 'Unexpected OBS archive layout.' }
New-Item -ItemType Directory -Path $SystemPlugins -Force | Out-Null
# Install the distribution's normal ProgramData layout on the disposable runner.
Copy-Item -LiteralPath (Join-Path $PackageDir 'loudness-balance-monitor') -Destination $ProductInstall -Recurse
New-Item -ItemType Directory -Path $ProbeInstall -Force | Out-Null
Copy-Item -LiteralPath $ProbeDll -Destination $ProbeInstall
$InstalledDll = Join-Path $ProductInstall 'loudness-balance-monitor.dll'
$OldInstalledDll = Join-Path $ProductInstall 'bin/64bit/loudness-balance-monitor.dll'
if ((Get-FileHash -LiteralPath $InstalledDll).Hash -ne (Get-FileHash -LiteralPath $ProductDll).Hash) {
    throw 'Installed DLL differs from the distribution.'
}
$ProfileDir = Join-Path $ConfigDir 'basic/profiles/Baseline'
$SceneDir = Join-Path $ConfigDir 'basic/scenes'
New-Item -ItemType Directory -Path $ProfileDir, $SceneDir -Force | Out-Null
$UserIni = "[General]`nFirstRun=true`n[Basic]`nProfile=Baseline`nProfileDir=Baseline`nSceneCollection=Baseline`nSceneCollectionFile=Baseline.json`nConfigOnNewProfile=false`n[BasicWindow]`nPreviewEnabled=false`n"
Set-Content -LiteralPath (Join-Path $ConfigDir 'user.ini') -Value $UserIni -Encoding utf8
Set-Content -LiteralPath (Join-Path $ConfigDir 'global.ini') -Value "[General]`nEnableAutoUpdates=false`n" -Encoding utf8
$ProfileIni = "[General]`nName=Baseline`n[Audio]`nSampleRate=48000`nChannelSetup=Stereo`nMonitoringDeviceId=default`n[Video]`nBaseCX=640`nBaseCY=360`nOutputCX=640`nOutputCY=360`nFPSType=0`nFPSCommon=30`n"
Set-Content -LiteralPath (Join-Path $ProfileDir 'basic.ini') -Value $ProfileIni -Encoding utf8
@{
    name = 'Baseline'
    sources = @(@{ name = 'Scene'; id = 'scene'; settings = @{ items = @() } })
    scene_order = @(@{ name = 'Scene' })
    current_scene = 'Scene'
    current_program_scene = 'Scene'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $SceneDir 'Baseline.json') -Encoding utf8
$PreviousResultDir = $env:LBM_BASELINE_RESULT_DIR
$PreviousPhase = $env:LBM_BASELINE_PHASE
$Process = $null
try {
    foreach ($Stage in @('fresh', 'old-setup', 'upgrade', 'restore-hidden')) {
        $Phase = switch ($Stage) { 'fresh' { 'setup' }; 'old-setup' { 'setup' }; 'upgrade' { 'restore' }; default { 'restore-hidden' } }
        if ($Stage -eq 'old-setup') {
            # Remove only the new-layout binaries inside this disposable installation.
            $InstallRoot = [IO.Path]::GetFullPath($ProductInstall)
            foreach ($Name in @('loudness-balance-monitor.dll', 'loudness-balance-monitor.pdb')) {
                $RemovePath = [IO.Path]::GetFullPath((Join-Path $InstallRoot $Name))
                if ([IO.Path]::GetDirectoryName($RemovePath) -ne $InstallRoot) { throw 'Binary removal escaped the disposable product directory.' }
                if (Test-Path -LiteralPath $RemovePath -PathType Leaf) { Remove-Item -LiteralPath $RemovePath }
            }
            Get-ChildItem -LiteralPath (Join-Path $OldPackageDir 'loudness-balance-monitor') | Copy-Item -Destination $ProductInstall -Recurse -Force
        } elseif ($Stage -eq 'upgrade') {
            # Keep the legacy bin tree to verify new-layout precedence during upgrades.
            Get-ChildItem -LiteralPath (Join-Path $PackageDir 'loudness-balance-monitor') | Copy-Item -Destination $ProductInstall -Recurse -Force
        }
        $ExpectedDll = if ($Stage -eq 'old-setup') { $OldInstalledDll } else { $InstalledDll }
        $SourceDll = if ($Stage -eq 'old-setup') { $OldProductDll } else { $ProductDll }
        if ((Get-FileHash -LiteralPath $ExpectedDll).Hash -ne (Get-FileHash -LiteralPath $SourceDll).Hash) { throw "Installed DLL differs from the distribution: $Stage" }
        if ($Stage -in @('upgrade', 'restore-hidden') -and (Get-FileHash -LiteralPath $OldInstalledDll).Hash -ne (Get-FileHash -LiteralPath $OldProductDll).Hash) { throw 'Legacy DLL was not preserved during upgrade.' }
        $PhaseDir = Join-Path $ResultDir $Stage
        New-Item -ItemType Directory -Path $PhaseDir | Out-Null
        $env:LBM_BASELINE_RESULT_DIR = $PhaseDir
        $env:LBM_BASELINE_PHASE = $Phase
        $Process = Start-Process -FilePath $ObsExe -WorkingDirectory $BinaryDir -ArgumentList @('--disable-updater', '--multi') -WindowStyle Hidden -PassThru
        if (!$Process.WaitForExit($TimeoutSeconds * 1000)) { throw "OBS baseline probe timed out: $Phase" }
        if ($Process.ExitCode -ne 0) { throw "OBS exited with code $($Process.ExitCode): $Phase" }
        $ResultPath = Join-Path $PhaseDir 'result.json'
        if (!(Test-Path -LiteralPath $ResultPath)) { throw "Probe result missing: $Phase" }
        $Result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
        if ($Result.passed -ne $true -or $Result.obs -ne '33.0.1' -or $Result.phase -ne $Phase) { throw "Probe failed: $Phase $($Result.reason)" }
        if (!$Result.product_binary_path -or [IO.Path]::GetFullPath($Result.product_binary_path) -ne [IO.Path]::GetFullPath($ExpectedDll)) { throw "Unexpected loaded product DLL: $($Result.product_binary_path)" }
        if (!(Test-Path -LiteralPath (Join-Path $PhaseDir 'dock.png'))) { throw "Dock screenshot missing: $Phase" }
        $Logs = @(Get-ChildItem -LiteralPath (Join-Path $ConfigDir 'logs') -Filter '*.txt')
        if (!$Logs.Count) { throw 'OBS log missing.' }
        $LogText = ($Logs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
        foreach ($Required in @('Portable mode: false', 'plugin loaded successfully', 'Dock registered successfully', "LBM_BASELINE PASS phase=$Phase")) {
            if (!$LogText.Contains($Required)) { throw "Required log marker missing: $Required" }
        }
        if ($Phase -ne 'setup' -and !$LogText.Contains('Restored OBS dock state after registration')) {
            throw 'Dock restoration log missing.'
        }
        $SettingsPath = Join-Path $ConfigDir 'plugin_config/loudness-balance-monitor/settings.json'
        $Settings = Get-Content -LiteralPath $SettingsPath -Raw | ConvertFrom-Json
        if ($Settings.voice_source -ne 'LBM baseline voice' -or
            @($Settings.bgm_sources.name) -notcontains 'LBM baseline bgm' -or
            $Settings.vad_threshold -ne -35 -or $Settings.balance_target -ne 8 -or $Settings.mix_preset -ne 1) {
            throw "Saved product settings mismatch: $Phase"
        }
        Copy-Item -LiteralPath $ConfigDir -Destination (Join-Path $PhaseDir 'saved-config') -Recurse
        Write-Output "OBS 33.0.1 normal installation verification passed: $Stage"
    }
} finally {
    if ($Process -and !$Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
    $env:LBM_BASELINE_RESULT_DIR = $PreviousResultDir
    $env:LBM_BASELINE_PHASE = $PreviousPhase
    $LogDir = Join-Path $ConfigDir 'logs'
    if (Test-Path -LiteralPath $LogDir) { Copy-Item -LiteralPath $LogDir -Destination (Join-Path $ResultDir 'obs-logs') -Recurse }
    @{
        runtime_version = '33.0.1'
        runtime_archive_sha256 = $ActualHash
        package_sha256 = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
        old_package_sha256 = $OldPackageHash
        product_sha256 = (Get-FileHash -LiteralPath $ProductDll -Algorithm SHA256).Hash
        product_installed_path = $InstalledDll
        config_directory = $ConfigDir
        probe_sha256 = (Get-FileHash -LiteralPath $ProbeDll -Algorithm SHA256).Hash
        process_exit_code = $(if ($Process -and $Process.HasExited) { $Process.ExitCode } else { $null })
        runner_image = $env:ImageVersion
        architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ResultDir 'environment.json') -Encoding utf8
}
