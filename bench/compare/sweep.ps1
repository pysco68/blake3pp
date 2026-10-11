# sweep.sh for Windows: same protocol, same digest check, against
# whichever cap binaries cross-windows.sh built for this architecture.
#
#   .\sweep.ps1 [-Size <MiB>] [-Reps <N>] [-Threads <N>] [-Pin <cpu>]
param([int]$Size = 0, [int]$Reps = 5, [int]$Threads = 0, [int]$Pin = -1)
$ErrorActionPreference = 'Stop'

# 512 MiB, or a quarter of physical memory where that is less: the
# default blake3pp_bench uses, so both hash the same size.
if ($Size -le 0) {
    $ramMib = [int]((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1MB)
    $Size = [math]::Min(512, [math]::Max(1, [int]($ramMib / 4)))
}

# The crate dispatches at runtime but is capped at compile time, so each
# backend is a separate binary. On aarch64 NEON is its only backend,
# which makes cap_no_neon and portable the same code and the pair a
# consistency check rather than two data points.
$exes = @(Get-ChildItem (Join-Path $PSScriptRoot 'blake3-rayon-bench-*.exe') |
          Sort-Object { if ($_.BaseName -eq 'blake3-rayon-bench-default') { 0 } else { 1 } }, Name)

$benchArgs = @('--size', $Size, '--reps', $Reps)
if ($Threads -gt 0) { $benchArgs += @('--threads', $Threads) }
if ($Pin -ge 0) { $benchArgs += @('--pin', $Pin) }

$digests = @()
foreach ($exe in $exes) {
    Write-Host "== $($exe.BaseName -replace '^blake3-rayon-bench-', '')"
    $out = & $exe.FullName @benchArgs
    $out | ForEach-Object { Write-Host $_ }
    $m = [regex]::Match(($out -join "`n"), 'digest ([0-9a-f]+)')
    if ($m.Success) { $digests += $m.Groups[1].Value }
}

# Every cap hashed the same bytes, so a differing digest means one of
# these backends is not computing BLAKE3 and its throughput row is
# meaningless.
$unique = @($digests | Sort-Object -Unique)
if ($unique.Count -ne 1) {
    Write-Host "DIGESTS DIFFER between caps: $($digests -join ' ')"
    exit 1
}
Write-Host "all caps agree on the digest: $($unique[0])"
