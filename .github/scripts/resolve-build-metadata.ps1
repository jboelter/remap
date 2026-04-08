[CmdletBinding()]
param(
    [string] $EventName = $env:GITHUB_EVENT_NAME,
    [string] $RefType = $env:GITHUB_REF_TYPE,
    [string] $RefName = $env:GITHUB_REF_NAME,
    [string] $GitSha = $env:GITHUB_SHA,
    [string] $EventPath = $env:GITHUB_EVENT_PATH,
    [string] $OutputFile = $env:GITHUB_OUTPUT
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($GitSha)) {
    throw 'Git SHA is required.'
}

$shortSha = $GitSha.Substring(0, [Math]::Min(7, $GitSha.Length))
$fullSha = $GitSha

if ($EventName -eq 'pull_request') {
    if ([string]::IsNullOrWhiteSpace($EventPath) -or -not (Test-Path $EventPath)) {
        throw "GitHub event payload not found at '${EventPath}'."
    }

    $event = Get-Content $EventPath -Raw | ConvertFrom-Json
    $versionTag = "pr-$($event.number)-$shortSha"
}
elseif ($RefType -eq 'branch') {
    $versionTag = "$RefName-$shortSha"
}
else {
    $versionTag = $RefName
}

if ([string]::IsNullOrWhiteSpace($versionTag)) {
    throw 'Resolved version tag is empty.'
}

if (-not [string]::IsNullOrWhiteSpace($OutputFile)) {
    Add-Content -Path $OutputFile -Value "version_tag=$versionTag"
    Add-Content -Path $OutputFile -Value "git_hash=$fullSha"
}

[pscustomobject]@{
    VersionTag = $versionTag
    GitHash = $fullSha
}
