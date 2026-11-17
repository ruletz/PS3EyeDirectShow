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

# GitHub allows 10 annotations per step, so pack several error lines into
# each one: 10 annotations x ~900 chars still shows the whole first wave.
$escaped = $errors | ForEach-Object { ($_ -replace '%', '%25' -replace "`r", '' -replace "`n", '%0A').Trim() }

$batch = New-Object System.Collections.Generic.List[string]
$length = 0
$sent = 0
foreach ($e in $escaped) {
    if (($length + $e.Length) -gt 800 -and $batch.Count -gt 0) {
        Write-Host "::error::${Label}: $($batch -join ' | ')"
        $sent++
        if ($sent -ge 10) { break }
        $batch.Clear()
        $length = 0
    }
    $batch.Add($e)
    $length += $e.Length
}
if ($sent -lt 10 -and $batch.Count -gt 0) {
    Write-Host "::error::${Label}: $($batch -join ' | ')"
}

if ($errors.Count -gt 60) {
    Write-Host "::warning::${Label}: $($errors.Count) error lines total; report truncated"
}
