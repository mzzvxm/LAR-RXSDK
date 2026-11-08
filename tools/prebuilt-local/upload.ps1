<#
.SYNOPSIS
    Publishes the prebuilt SDKs of one commit as the release sdk-<12 digits> of mzzvxm/LAR-RXSDK, the way the
    workflow's "prepare", build and "manifest" jobs do (Windows PowerShell 5.1 and the GitHub CLI, gh).

.DESCRIPTION
    This writes to GitHub: it makes a release (a pre-release, not the latest one) and uploads files to it. Run it
    on purpose, after build-windows.ps1 / build-linux.sh and manifest.py. Use -DryRun first to see what it would do.

    Order, as in the workflow:
      1. the commit must exist on GitHub (the release's tag is made on it)
      2. the release sdk-<12> is made if it is not there (title "Prebuilt SDK <12>", the workflow's text,
         pre-release, not latest); if it is there, an old sdk-prebuilt-<12>.json is deleted first, because that file is
         what tells the launcher that a release is complete
      3. the archives and their .sha256 files are uploaded (--clobber replaces files of an earlier try)
      4. sdk-prebuilt-<12>.json is uploaded LAST
      5. the release is read back and the asset names and sizes compared with the local files

    Before anything is written the files are checked: the json's entries against the archives (size, SHA-256, the
    .sha256 files, the commit, the url the launcher will use).

.PARAMETER Commit
    The full 40-digit hash of the commit the SDKs were built from (or anything GitHub's commits API resolves).

.PARAMETER Out
    The folder with the archives and sdk-prebuilt-<12>.json (the -Out of build-windows.ps1, the <outdir> of
    build-linux.sh; both can write to the same folder, which is what manifest.py reads).

.PARAMETER Repo
    The repository that holds the release. Default mzzvxm/LAR-RXSDK, where the launcher looks.

.PARAMETER DryRun
    Show the commands that write, and run only the ones that read.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File upload.ps1 -Commit f0f34904c579e4b81537cef2fce46afb716388e0 -Out C:\mcv\pb\f0f34904c579 -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Commit,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Repo = 'mzzvxm/LAR-RXSDK',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Resolve-Absolute([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path (Get-Location).ProviderPath $Path))
}

# Runs gh and returns its output lines (stdout only) and exit code. Nothing is shown. (stderr lines are dropped: in
# Windows PowerShell 5.1 they would become errors.)
function Get-GhText {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& gh @Arguments 2>$null) } finally { $ErrorActionPreference = $savedPreference }
    return [pscustomobject]@{ Output = $lines; ExitCode = $LASTEXITCODE }
}

# Runs gh to write something: shown, stops the script on a failure; with -DryRun it only prints the command.
function Invoke-GhWrite {
    param([Parameter(Mandatory = $true)][string[]]$Arguments, [Parameter(Mandatory = $true)][string]$What)
    if ($DryRun) {
        Write-Host ('[dry run] gh ' + ($Arguments -join ' '))
        return
    }
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & gh @Arguments | Out-Host } finally { $ErrorActionPreference = $savedPreference }
    if ($LASTEXITCODE -ne 0) { throw ('{0} failed (exit code {1}).' -f $What, $LASTEXITCODE) }
}

$OutRoot = (Resolve-Absolute $Out).TrimEnd('\')
if (-not (Test-Path -LiteralPath $OutRoot)) { throw "$OutRoot does not exist." }
if ($null -eq (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'The GitHub CLI (gh) is not on PATH.' }
$env:GH_REPO = $Repo
$env:GH_PROMPT_DISABLED = '1'

# ---- Who is acting, and may they write to the repository ----
$auth = Get-GhText -Arguments @('auth', 'status')
if ($auth.ExitCode -ne 0) { throw 'gh is not signed in (gh auth login).' }
$login = (Get-GhText -Arguments @('api', 'user', '--jq', '.login')).Output
$canPush = (Get-GhText -Arguments @('api', "repos/$Repo", '--jq', '.permissions.push')).Output
Write-Host ("gh user: {0}; push access to {1}: {2}" -f ($login -join ''), $Repo, ($canPush -join ''))
if (($canPush -join '') -ne 'true') {
    if ($DryRun) { Write-Warning "No push access to ${Repo}: a real run would stop here." }
    else { throw "No push access to $Repo (a release needs it)." }
}

# ---- The commit ----
$shaReply = Get-GhText -Arguments @('api', "repos/$Repo/commits/$Commit", '--jq', '.sha')
$sha = ($shaReply.Output -join '').Trim().ToLowerInvariant()
if ($shaReply.ExitCode -ne 0 -or $sha -notmatch '^[0-9a-f]{40}$') {
    throw "The commit $Commit is not in $Repo on GitHub. Push the commit first: the release's tag is made on it."
}
if ($Commit -match '^[0-9a-f]{40}$' -and $Commit.ToLowerInvariant() -ne $sha) { throw "GitHub resolved $Commit to $sha." }
$short = $sha.Substring(0, 12)
$tag = "sdk-$short"
$manifestName = "sdk-prebuilt-$short.json"
$manifestPath = Join-Path $OutRoot $manifestName
Write-Host "Commit $sha -> release $tag of $Repo"

# ---- The files: the json, and every archive it names, checked against each other ----
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "$manifestPath is missing: run manifest.py $sha $OutRoot first." }
$manifest = [IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
$entries = @()
foreach ($key in 'sdkPrebuilt', 'sdkPrebuiltLinux') {
    $entry = $manifest.$key
    if ($null -ne $entry) { $entries += [pscustomobject]@{ Key = $key; Entry = $entry } }
}
if ($entries.Count -eq 0) { throw "$manifestName has no entry." }

$assetFiles = @()
foreach ($item in $entries) {
    $entry = $item.Entry
    $archive = Join-Path $OutRoot $entry.asset
    $shaFile = $archive + '.sha256'
    foreach ($needed in @($archive, $shaFile)) {
        if (-not (Test-Path -LiteralPath $needed)) { throw "$needed is missing ($($item.Key) names it)." }
    }
    $actualSha = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    $actualSize = (Get-Item -LiteralPath $archive).Length
    $shaFileText = ([IO.File]::ReadAllText($shaFile)).Trim()
    $expectedUrl = "https://github.com/$Repo/releases/download/$tag/$($entry.asset)"
    $problems = @()
    if ($actualSha -ne [string]$entry.sha256) { $problems += "SHA-256 is $actualSha, the json says $($entry.sha256)" }
    if ($actualSize -ne [int64]$entry.size) { $problems += "size is $actualSize, the json says $($entry.size)" }
    if (-not $shaFileText.StartsWith($actualSha)) { $problems += "the .sha256 file does not start with $actualSha" }
    if ([string]$entry.sdkCommit -ne $sha) { $problems += "the json says commit $($entry.sdkCommit)" }
    if ([string]$entry.url -ne $expectedUrl) { $problems += "the url is $($entry.url), expected $expectedUrl" }
    if (-not $entry.asset.StartsWith("lar-rxsdk-prebuilt-$short-") -or -not $entry.asset.Contains($entry.toolchain)) { $problems += 'the asset name does not carry the commit and the toolchain' }
    if ($problems.Count -gt 0) { throw ("$($entry.asset) does not agree with $manifestName`:`n  " + ($problems -join "`n  ")) }
    Write-Host ('{0}: {1}  {2:N1} MB  sha256 {3}  toolchain {4}' -f $item.Key, $entry.asset, ($actualSize / 1MB), $actualSha, $entry.toolchain)
    $assetFiles += [pscustomobject]@{ Name = $entry.asset; Path = $archive; Size = $actualSize }
    $assetFiles += [pscustomobject]@{ Name = $entry.asset + '.sha256'; Path = $shaFile; Size = (Get-Item -LiteralPath $shaFile).Length }
}
$manifestFile = [pscustomobject]@{ Name = $manifestName; Path = $manifestPath; Size = (Get-Item -LiteralPath $manifestPath).Length }

# ---- The release (the workflow's "Make the release if it is not there") ----
$view = Get-GhText -Arguments @('release', 'view', $tag, '--json', 'assets')
if ($view.ExitCode -eq 0) {
    Write-Host "The release $tag exists; this replaces its files."
    # The json tells the launcher that the release is complete and is written last: take the old one away now.
    $existing = ($view.Output -join "`n") | ConvertFrom-Json
    $existingNames = @($existing.assets | ForEach-Object { $_.name })
    if ($existingNames -contains $manifestName) {
        Invoke-GhWrite -What 'gh release delete-asset' -Arguments @('release', 'delete-asset', $tag, $manifestName, '--yes')
    }
} else {
    # The workflow's release text, word for word.
    $notes = (@(
            "Prebuilt LAR-RXSDK, built from commit $sha.",
            '',
            "The LARecomp launcher downloads this SDK instead of building it on the player's PC. It is not meant to be downloaded by hand.",
            '',
            "Windows: built with llvm-mingw (a .zip). Linux: built with the launcher's Linux build kit (a .tar.br). Each file has a .sha256 beside it.",
            '',
            "$manifestName lists the files with their SHA-256 and size. It is added when the builds are done: a release without it is not finished yet.") -join "`n")
    $notesFile = Join-Path ([IO.Path]::GetTempPath()) ("lar-release-notes-$short.txt")
    [IO.File]::WriteAllText($notesFile, $notes, (New-Object Text.UTF8Encoding($false)))
    try {
        # --target makes the new tag sdk-<12> point at the commit.
        $createArguments = @('release', 'create', $tag, '--target', $sha, '--title', "Prebuilt SDK $short", '--notes-file', $notesFile, '--prerelease', '--latest=false')
        if ($DryRun) {
            Write-Host ('[dry run] gh ' + ($createArguments -join ' '))
        } else {
            $savedPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try { & gh @createArguments | Out-Host } finally { $ErrorActionPreference = $savedPreference }
            if ($LASTEXITCODE -ne 0) {
                # Another run may have made it a moment ago: it is fine if it is there now.
                if ((Get-GhText -Arguments @('release', 'view', $tag, '--json', 'tagName')).ExitCode -ne 0) { throw "gh release create $tag failed." }
            }
        }
    } finally { Remove-Item -LiteralPath $notesFile -Force -ErrorAction SilentlyContinue }
}

# ---- The archives and their .sha256 files, then the json last ----
$archiveArguments = @('release', 'upload', $tag)
foreach ($file in $assetFiles) { $archiveArguments += $file.Path }
$archiveArguments += '--clobber'
Invoke-GhWrite -What 'Uploading the archives' -Arguments $archiveArguments
Invoke-GhWrite -What "Uploading $manifestName" -Arguments @('release', 'upload', $tag, $manifestPath, '--clobber')

# ---- Read it back ----
if ($DryRun) {
    Write-Host '[dry run] nothing was written; the release was not read back.'
    exit 0
}
$back = Get-GhText -Arguments @('release', 'view', $tag, '--json', 'assets,isPrerelease,url')
if ($back.ExitCode -ne 0) { throw "Could not read the release $tag back." }
$release = ($back.Output -join "`n") | ConvertFrom-Json
$listed = @{}
foreach ($asset in $release.assets) { $listed[$asset.name] = [int64]$asset.size }
$missing = @()
foreach ($file in ($assetFiles + $manifestFile)) {
    if (-not $listed.ContainsKey($file.Name)) { $missing += "$($file.Name) is not on the release" }
    elseif ($listed[$file.Name] -ne [int64]$file.Size) { $missing += "$($file.Name) is $($listed[$file.Name]) bytes on the release, $($file.Size) here" }
}
if ($missing.Count -gt 0) { throw ("The release does not match the local files:`n  " + ($missing -join "`n  ")) }
Write-Host ''
Write-Host "Done: $($release.url)  (pre-release: $($release.isPrerelease))"
foreach ($file in ($assetFiles + $manifestFile)) { Write-Host ('  {0,12:N0}  {1}' -f $file.Size, $file.Name) }
Write-Host "The launcher finds it by the tag $tag and the file $manifestName."
