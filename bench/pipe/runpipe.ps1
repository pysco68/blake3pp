# runpipe.ps1: run "producer | consumer" for several consumers, counterbalanced,
# and refuse any round that is not a real measurement. Windows twin of
# runpipe.sh; the two do the same checks in the same order and should be
# kept parallel.
#
#   powershell -ExecutionPolicy Bypass -File runpipe.ps1 -Producer 'PRODUCER' `
#     [-Rounds 6] [-Shape NAME] [-Reference 'CMD'] [-ToolDir DIR] LABEL=CONSUMER ...
#
# The pipe is cmd.exe's: it is the one a Windows user's `x | blake3ppsum -`
# meets, and it behaves as CreatePipe(0) does. A PowerShell pipeline
# between two native programs carries the bytes through PowerShell, and
# WSL's and Git Bash's pipes are different pipes. Each round's
# pipeline is written into a .cmd file and run by cmd.exe, so no command
# line passes through PowerShell's native-argument quoting.
#
# Each side runs under rux:
#
#   rux -o P -- PRODUCER | rux -o C -- CONSUMER > D
#
# so both exit codes and both processes' CPU land in files. A round is
# refused, and the run stops with exit code 1, when:
#   1. either side's exit code is not 0, or its report is missing;
#   2. the consumer's digest differs from the reference, taken once before
#      the rotation;
#   3. the round is more than 1.5 times faster than the drain ceiling, the
#      best of four drains (two runs each) over the same producer measured
#      before the rotation; faster than the ceiling within that margin is
#      printed with a warning. The margin absorbs noise (see runpipe.sh).
# The arm order reverses every round. Rows are printed in run order, then
# the per-arm medians. Context switches read `na`: Windows has no
# per-process counter for them (see rux.cpp).

# PositionalBinding off: every named parameter is named-only, so the
# LABEL=CONSUMER arguments all land in $Arms rather than the first one
# binding to whichever parameter comes next in declaration order.
[CmdletBinding(PositionalBinding = $false)]
param(
  [Parameter(Mandatory = $true)][string]$Producer,
  [int]$Rounds = 6,
  [string]$Shape = '-',
  [string]$Reference = '',
  [string]$ToolDir = '',
  [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arms
)

$ErrorActionPreference = 'Stop'
# Figures in the invariant culture: under a locale whose decimal mark is a
# comma, -f writes "26,4" and every row of the CSV gains a column.
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture

function Fail([string]$why) {
  [Console]::Error.WriteLine("runpipe.ps1: REFUSED: $why")
  exit 1
}

if (-not $Arms -or $Arms.Count -lt 1) { Fail 'at least one LABEL=CONSUMER is required' }
foreach ($a in $Arms) { if ($a -notmatch '=') { Fail "'$a' is not LABEL=CONSUMER" } }

function Find-Tool([string]$name) {
  foreach ($d in @($ToolDir, $PSScriptRoot)) {
    if ($d -and (Test-Path (Join-Path $d "$name.exe"))) { return (Join-Path $d "$name.exe") }
  }
  $c = Get-Command "$name.exe" -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  Fail "$name.exe not found (use -ToolDir)"
}
$Rux = Find-Tool 'rux'
$Drain = Find-Tool 'drain'

$Work = Join-Path ([IO.Path]::GetTempPath()) ("runpipe-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $Work | Out-Null

# One field of a rux report.
function Field([string]$file, [string]$name) {
  $line = Get-Content -LiteralPath $file -Raw
  if ($line -match " $name=(\S+)") { return $Matches[1] }
  return ''
}

# One pipeline run; returns the wall clock in microseconds and leaves the
# reports in $Work\p and $Work\c, stdout in $Work\d and stderr in $Work\e.
function Run-Once([string]$consumer, [string]$what) {
  foreach ($f in 'p', 'c', 'd', 'e') { Remove-Item -LiteralPath (Join-Path $Work $f) -ErrorAction SilentlyContinue }
  # % is cmd.exe's expansion character, also inside a batch file.
  $prod = $Producer -replace '%', '%%'
  $cons = $consumer -replace '%', '%%'
  $line = "@`"$Rux`" -o `"$Work\p`" -l prod -- $prod | `"$Rux`" -o `"$Work\c`" -l cons -- $cons > `"$Work\d`" 2> `"$Work\e`""
  $script = Join-Path $Work 'round.cmd'
  # cmd.exe reads a .cmd file in the OEM code page. ASCII would turn a
  # non-ASCII character of %TEMP% into '?' and the round would name a path
  # that does not exist.
  $oem = [System.Text.Encoding]::GetEncoding([System.Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage)
  [System.IO.File]::WriteAllText($script, $line + "`r`n", $oem)
  & cmd.exe /d /c $script
  foreach ($side in 'c', 'p') {
    $f = Join-Path $Work $side
    if (-not (Test-Path $f) -or (Get-Item $f).Length -eq 0) { Fail "${what}: no report from the $side side (it did not run)" }
    $code = Field $f 'exit'
    if ($code -ne '0') {
      $err = ''
      if (Test-Path "$Work\e") { $err = Get-Content -LiteralPath "$Work\e" -Raw }
      Fail "${what}: the $side side exited ${code}: $err"
    }
  }
  return [math]::Max([int64](Field "$Work\p" 'wall_us'), [int64](Field "$Work\c" 'wall_us'))
}

function Digest-Of() {
  $out = ''
  if (Test-Path "$Work\d") { $out = Get-Content -LiteralPath "$Work\d" -Raw }
  if (-not $out) { return '' }
  return ($out.Trim() -split '\s+')[0]
}

try {
  Write-Output "producer: $Producer"

  if (-not $Reference) { $Reference = $Arms[0].Substring($Arms[0].IndexOf('=') + 1) }
  [void](Run-Once $Reference 'reference')
  $RefDigest = Digest-Of
  if (-not $RefDigest) { Fail "the reference '$Reference' printed no digest" }
  Write-Output "reference: $Reference -> $RefDigest"

  # The drain ceiling: the best of four drains over this producer, and the
  # stream's length.
  $Ceiling = [int64]0; $CeilingBy = ''; $Bytes = [int64]-1
  foreach ($d in '--buf 64', '--buf 64', '--buf 64 --threads 2', '--buf 64 --threads 2',
                 '--buf 1024', '--buf 1024', '--buf 4096 --threads 2', '--buf 4096 --threads 2') {
    $w = Run-Once "`"$Drain`" $d" "drain $d"
    $e = Get-Content -LiteralPath "$Work\e" -Raw
    if ($e -notmatch 'drain: bytes=(\d+)') { Fail "drain $d reported no byte count" }
    $b = [int64]$Matches[1]
    if ($Bytes -ge 0 -and $b -ne $Bytes) { Fail "drain $d read $b bytes, an earlier drain read $Bytes" }
    $Bytes = $b
    if ($Ceiling -eq 0 -or $w -lt $Ceiling) { $Ceiling = $w; $CeilingBy = $d }
  }
  if ($Bytes -le 0) { Fail 'the producer wrote nothing' }
  Write-Output ("stream: {0} bytes; drain ceiling {1:F1} MiB/s ({2:F1} ms, drain {3})" -f `
      $Bytes, ($Bytes / 1048576.0 / ($Ceiling / 1e6)), ($Ceiling / 1000.0), $CeilingBy)

  Write-Output 'shape,round,arm,wall_ms,MiBps,cons_user_ms,cons_sys_ms,cons_vcsw,cons_ivcsw,prod_user_ms,prod_sys_ms,prod_vcsw,prod_ivcsw,total_cpu_ms,digest'
  $Rows = @()
  $n = $Arms.Count
  for ($r = 1; $r -le $Rounds; $r++) {
    for ($i = 0; $i -lt $n; $i++) {
      # Forward on odd rounds, reversed on even ones.
      if ($r % 2 -eq 1) { $k = $i } else { $k = $n - 1 - $i }
      $arm = $Arms[$k]
      $label = $arm.Substring(0, $arm.IndexOf('='))
      $cmd = $arm.Substring($arm.IndexOf('=') + 1)
      $w = Run-Once $cmd "round $r, $label"
      $digest = Digest-Of
      if ($digest -ne $RefDigest) { Fail "round $r, ${label}: digest $digest, reference $RefDigest" }
      # More than 1.5 times faster than the best drain: not a measurement.
      if ($w * 3 -lt $Ceiling * 2) { Fail "round $r, ${label}: $w us is over 1.5x faster than the drain ceiling of $Ceiling us" }
      if ($w -lt $Ceiling) { [Console]::Error.WriteLine("runpipe.ps1: warning: round $r, ${label}: $w us beats the drain ceiling of $Ceiling us") }
      $cu = [int64](Field "$Work\c" 'user_us'); $cs = [int64](Field "$Work\c" 'sys_us')
      $pu = [int64](Field "$Work\p" 'user_us'); $ps = [int64](Field "$Work\p" 'sys_us')
      $row = [pscustomobject]@{
        Arm = $label; Wall = $w / 1000.0; MiBps = $Bytes / 1048576.0 / ($w / 1e6)
        Total = ($cu + $cs + $pu + $ps) / 1000.0
      }
      $Rows += $row
      Write-Output ("{0},{1},{2},{3:F1},{4:F1},{5:F1},{6:F1},{7},{8},{9:F1},{10:F1},{11},{12},{13:F1},{14}" -f `
          $Shape, $r, $label, $row.Wall, $row.MiBps, ($cu / 1000.0), ($cs / 1000.0),
          (Field "$Work\c" 'vcsw'), (Field "$Work\c" 'ivcsw'), ($pu / 1000.0), ($ps / 1000.0),
          (Field "$Work\p" 'vcsw'), (Field "$Work\p" 'ivcsw'), $row.Total, $digest.Substring(0, [Math]::Min(16, $digest.Length)))
    }
  }

  function Median([double[]]$v) {
    $s = $v | Sort-Object
    $m = $s.Count
    if ($m % 2 -eq 1) { return $s[($m - 1) / 2] }
    return ($s[$m / 2 - 1] + $s[$m / 2]) / 2
  }
  Write-Output 'medians: arm,wall_ms,MiBps,total_cpu_ms'
  foreach ($arm in $Arms) {
    $label = $arm.Substring(0, $arm.IndexOf('='))
    $mine = $Rows | Where-Object { $_.Arm -eq $label }
    Write-Output ("  {0},{1:F1},{2:F1},{3:F1}" -f $label, (Median ($mine | ForEach-Object Wall)),
        (Median ($mine | ForEach-Object MiBps)), (Median ($mine | ForEach-Object Total)))
  }
} finally {
  Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue
}
