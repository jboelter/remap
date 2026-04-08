[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $OutputDir,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedVersionTag,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedGitHash,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string] $Architecture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Invoke-CheckedCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [string[]] $Arguments = @()
    )

    & $Path @Arguments
    if ($LASTEXITCODE -ne 0) {
        $renderedArgs = if ($Arguments.Count -gt 0) { $Arguments -join ' ' } else { '' }
        throw "Command failed with exit code ${LASTEXITCODE}: $Path $renderedArgs"
    }
}

function Get-PeMachine {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset + 4
        $machine = $reader.ReadUInt16()
    }
    finally {
        if ($null -ne $reader) {
            $reader.Dispose()
        }
        $stream.Dispose()
    }

    switch ($machine) {
        0x8664 { return 'x64' }
        0xaa64 { return 'arm64' }
        default { return ('unknown-0x{0:X4}' -f $machine) }
    }
}

$expectedFiles = @(
    (Join-Path $OutputDir 'remap.exe'),
    (Join-Path $OutputDir 'tap-timer.exe'),
    (Join-Path $OutputDir 'remap.pdb'),
    (Join-Path $OutputDir 'tap-timer.pdb')
)

foreach ($path in $expectedFiles) {
    if (-not (Test-Path $path)) {
        throw "Missing expected output: $path"
    }
}

$executables = @(
    (Join-Path $OutputDir 'remap.exe'),
    (Join-Path $OutputDir 'tap-timer.exe')
)

foreach ($path in $executables) {
    $versionInfo = (Get-Item $path).VersionInfo

    if ($versionInfo.ProductVersion -ne $ExpectedVersionTag) {
        throw "Unexpected ProductVersion for ${path}: '$($versionInfo.ProductVersion)'"
    }

    if ($versionInfo.Comments -ne $ExpectedGitHash) {
        throw "Unexpected Comments value for ${path}: '$($versionInfo.Comments)'"
    }

    $machineType = Get-PeMachine -Path $path
    if ($machineType -ne $Architecture) {
        throw "Unexpected PE machine type for ${path}: '$machineType'"
    }
}

if ($Architecture -eq 'x64') {
    Invoke-CheckedCommand -Path (Join-Path $OutputDir 'remap.exe') -Arguments @('--version')
    Invoke-CheckedCommand -Path (Join-Path $OutputDir 'remap.exe') -Arguments @('--help')
    Invoke-CheckedCommand -Path (Join-Path $OutputDir 'tap-timer.exe') -Arguments @('--version')
    Invoke-CheckedCommand -Path (Join-Path $OutputDir 'tap-timer.exe') -Arguments @('--help')
}
