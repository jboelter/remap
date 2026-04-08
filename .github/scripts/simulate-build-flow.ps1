[CmdletBinding()]
param(
    [ValidateSet('main', 'tag', 'pr')]
    [string] $Mode = 'main',

    [string] $RefName = '',

    [int] $PullRequestNumber = 1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repoRoot

$gitSha = (git rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($gitSha)) {
    throw 'Failed to resolve the current git commit SHA.'
}

$simulationRoot = Join-Path $repoRoot 'build\workflow-sim'
$runnerTemp = Join-Path $simulationRoot 'runner-temp'
Remove-Item -Recurse -Force $simulationRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $runnerTemp | Out-Null

switch ($Mode) {
    'main' {
        $metadata = & (Join-Path $PSScriptRoot 'resolve-build-metadata.ps1') `
            -EventName 'push' `
            -RefType 'branch' `
            -RefName ($(if ($RefName) { $RefName } else { 'main' })) `
            -GitSha $gitSha `
            -OutputFile ''
    }
    'tag' {
        $metadata = & (Join-Path $PSScriptRoot 'resolve-build-metadata.ps1') `
            -EventName 'push' `
            -RefType 'tag' `
            -RefName ($(if ($RefName) { $RefName } else { 'v0.0.0-local' })) `
            -GitSha $gitSha `
            -OutputFile ''
    }
    'pr' {
        $eventPath = Join-Path $simulationRoot 'pull_request_event.json'
        @{ number = $PullRequestNumber } | ConvertTo-Json | Set-Content -Path $eventPath -Encoding utf8
        $metadata = & (Join-Path $PSScriptRoot 'resolve-build-metadata.ps1') `
            -EventName 'pull_request' `
            -RefType 'branch' `
            -RefName ($(if ($RefName) { $RefName } else { 'workflow' })) `
            -GitSha $gitSha `
            -EventPath $eventPath `
            -OutputFile ''
    }
}

$matrix = @(
    @{ Architecture = 'x64'; Configuration = 'Debug'; ConfigurationLower = 'debug' }
    @{ Architecture = 'x64'; Configuration = 'Release'; ConfigurationLower = 'release' }
    @{ Architecture = 'arm64'; Configuration = 'Debug'; ConfigurationLower = 'debug' }
    @{ Architecture = 'arm64'; Configuration = 'Release'; ConfigurationLower = 'release' }
)

$archives = @()
foreach ($entry in $matrix) {
    Write-Host "=== Building $($entry.Architecture) $($entry.Configuration) ==="

    & (Join-Path $repoRoot 'build.bat') $entry.Configuration $entry.Architecture $metadata.VersionTag $metadata.GitHash
    if ($LASTEXITCODE -ne 0) {
        throw "build.bat failed for $($entry.Architecture) $($entry.Configuration) with exit code ${LASTEXITCODE}."
    }

    $outputDir = Join-Path $repoRoot "build\windows\$($entry.Architecture)\$($entry.ConfigurationLower)"
    & (Join-Path $PSScriptRoot 'validate-outputs.ps1') `
        -OutputDir $outputDir `
        -ExpectedVersionTag $metadata.VersionTag `
        -ExpectedGitHash $metadata.GitHash `
        -Architecture $entry.Architecture

    $archive = & (Join-Path $PSScriptRoot 'package-release-archive.ps1') `
        -OutputDir $outputDir `
        -VersionTag $metadata.VersionTag `
        -Architecture $entry.Architecture `
        -ConfigurationLower $entry.ConfigurationLower `
        -RunnerTemp $runnerTemp `
        -OutputFile ''
    $archives += $archive
}

Write-Host ''
Write-Host "Simulated mode : $Mode"
Write-Host "Version tag    : $($metadata.VersionTag)"
Write-Host "Git hash       : $($metadata.GitHash)"
Write-Host 'Archives:'
$archives | ForEach-Object { Write-Host "  $($_.ArchivePath)" }
