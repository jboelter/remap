[CmdletBinding()]
param(
    [string] $GitSha = $env:GITHUB_SHA,
    [string] $RefName = $env:GITHUB_REF_NAME,
    [string] $RemoteName = 'origin',
    [string] $BranchName = 'main'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

if ([string]::IsNullOrWhiteSpace($GitSha)) {
    throw 'Git SHA is required.'
}

git fetch --no-tags $RemoteName $BranchName
if ($LASTEXITCODE -ne 0) {
    throw "git fetch failed with exit code ${LASTEXITCODE}."
}

$mainRef = "$RemoteName/$BranchName"
git merge-base --is-ancestor $GitSha $mainRef
if ($LASTEXITCODE -eq 0) {
    return
}

if ($LASTEXITCODE -eq 1) {
    throw "Tag $RefName (commit $GitSha) is not on $mainRef."
}

throw "git merge-base failed with exit code ${LASTEXITCODE}."
