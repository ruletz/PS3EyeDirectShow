# Republished MSBuild errors as GitHub check annotations.
#
# The raw step log needs a signed-in read; annotations can be fetched from the
# public API, so a failed cloud build can be diagnosed without a token.
param(
    [Parameter(Mandatory = $true)][string] $LogPath,
    [Parameter(Mandatory = $true)][string] $Label
)

$ErrorActionPreference = 'Continue'

if (-not (Test-Path $LogPath)) {
    Write-Host "::warning::no log written for ${Label}"
    exit 0
}

$lines = Get-Content -Path $LogPath

# "src\file.c(12,3): error C2065: msg" or "error MSB4019: msg"
$errors = @($lines | Where-Object { $_ -match ':\s*(fatal\s+)?error\s+[A-Z]+[0-9]+' })

if ($errors.Count -eq 0) {
    Write-Host "::warning::${Label}: no error lines found in log"
    exit 0
}

foreach ($e in ($errors | Select-Object -First 10)) {
    $msg = ($e -replace '%', '%25' -replace "`r", '' -replace "`n", '%0A').Trim()
    if ($msg.Length -gt 900) { $msg = $msg.Substring(0, 900) + '...' }
    Write-Host "::error::${Label}: $msg"
}

if ($errors.Count -gt 10) {
    Write-Host "::error::${Label}: $($errors.Count - 10) more error lines; see the uploaded logs"
}
