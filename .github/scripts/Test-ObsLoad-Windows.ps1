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
$ProductDll = Join-Path $PackageDir 'loudness-balance-monitor/bin/64bit/loudness-balance-monitor.dll'
$ProductData = Join-Path $PackageDir 'loudness-balance-monitor/data'
if (!(Test-Path -LiteralPath $ProductDll -PathType Leaf) -or !(Test-Path -LiteralPath $ProductData -PathType Container)) { throw 'Unexpected product package layout.' }
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
# OBS 33 retains this portable legacy module layout until OBS 34.
$PluginDir = Join-Path $Runtime 'obs-plugins/64bit'
New-Item -ItemType Directory -Path $PluginDir -Force | Out-Null
Copy-Item -LiteralPath $ProductDll, $ProbeDll -Destination $PluginDir
$DataDir = Join-Path $Runtime 'data/obs-plugins/loudness-balance-monitor'
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
Copy-Item -Path (Join-Path $ProductData '*') -Destination $DataDir -Recurse
$ConfigDir = Join-Path $Runtime 'config/obs-studio'
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
$Process = $null
try {
    $env:LBM_BASELINE_RESULT_DIR = $ResultDir
    $Process = Start-Process -FilePath $ObsExe -WorkingDirectory $BinaryDir -ArgumentList @('--portable', '--disable-updater', '--multi') -WindowStyle Hidden -PassThru
    if (!$Process.WaitForExit($TimeoutSeconds * 1000)) { throw 'OBS baseline probe timed out.' }
    if ($Process.ExitCode -ne 0) { throw "OBS exited with code $($Process.ExitCode)." }
    $ResultPath = Join-Path $ResultDir 'result.json'
    if (!(Test-Path -LiteralPath $ResultPath)) { throw 'Probe did not produce result.json.' }
    $Result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
    if ($Result.passed -ne $true -or $Result.obs -ne '33.0.1') { throw "Probe failed: $($Result.reason)" }
    if (!(Test-Path -LiteralPath (Join-Path $ResultDir 'dock.png'))) { throw 'Dock screenshot missing.' }
    $Logs = @(Get-ChildItem -LiteralPath (Join-Path $ConfigDir 'logs') -Filter '*.txt')
    if (!$Logs.Count) { throw 'OBS log missing.' }
    $LogText = ($Logs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
    foreach ($Required in @('Portable mode: true', 'plugin loaded successfully', 'Dock registered successfully', 'LBM_BASELINE PASS')) {
        if (!$LogText.Contains($Required)) { throw "Required log marker missing: $Required" }
    }
    Write-Output "OBS 33.0.1 baseline load passed. Evidence: $ResultDir"
} finally {
    if ($Process -and !$Process.HasExited) {
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
    $env:LBM_BASELINE_RESULT_DIR = $PreviousResultDir
    $LogDir = Join-Path $ConfigDir 'logs'
    if (Test-Path -LiteralPath $LogDir) { Copy-Item -LiteralPath $LogDir -Destination (Join-Path $ResultDir 'obs-logs') -Recurse }
    @{
        runtime_version = '33.0.1'
        runtime_archive_sha256 = $ActualHash
        package_sha256 = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
        product_sha256 = (Get-FileHash -LiteralPath $ProductDll -Algorithm SHA256).Hash
        probe_sha256 = (Get-FileHash -LiteralPath $ProbeDll -Algorithm SHA256).Hash
        process_exit_code = $(if ($Process -and $Process.HasExited) { $Process.ExitCode } else { $null })
        runner_image = $env:ImageVersion
        architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $ResultDir 'environment.json') -Encoding utf8
}
