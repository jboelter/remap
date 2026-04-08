[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutputDir,

    [Parameter(Mandatory = $true)]
    [string] $VersionTag,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string] $Architecture,

    [Parameter(Mandatory = $true)]
    [string] $ConfigurationLower,

    [string] $RunnerTemp = $env:RUNNER_TEMP,
    [string] $OutputFile = $env:GITHUB_OUTPUT
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($RunnerTemp)) {
    throw 'Runner temp directory is required.'
}

$archiveBaseName = "remap-$VersionTag-windows-$Architecture-$ConfigurationLower"
$stagingDir = Join-Path $RunnerTemp $archiveBaseName
$archiveDir = Join-Path $RunnerTemp 'release-assets'
$archivePath = Join-Path $archiveDir "$archiveBaseName.zip"

Remove-Item -Recurse -Force $stagingDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null

$files = @('remap.exe', 'tap-timer.exe', 'remap.pdb', 'tap-timer.pdb')
foreach ($file in $files) {
    Copy-Item (Join-Path $OutputDir $file) -Destination $stagingDir
}

$previousProgressPreference = $ProgressPreference
try {
    $ProgressPreference = 'SilentlyContinue'
    Compress-Archive -Path (Join-Path $stagingDir '*') -DestinationPath $archivePath -Force
}
finally {
    $ProgressPreference = $previousProgressPreference
}

if (-not [string]::IsNullOrWhiteSpace($OutputFile)) {
    Add-Content -Path $OutputFile -Value "archive_name=$archiveBaseName"
    Add-Content -Path $OutputFile -Value "archive_path=$archivePath"
}

[pscustomobject]@{
    ArchiveName = $archiveBaseName
    ArchivePath = $archivePath
}
