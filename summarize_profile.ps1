param([int]$Top = 25)
$L = "$env:USERPROFILE\Documents\My Games\Sid Meier's Civilization 5\Logs"
$rows = Get-Content "$L\civ5_lua_profile.tsv" | Where-Object { $_ -notmatch '^#' } | ForEach-Object {
  $p = $_ -split "`t", 3
  $frames = $p[2].TrimEnd(';') -split ';'
  [pscustomobject]@{ Count = [int]$p[0]; VM = $p[1]; Leaf = $frames[0]; Stack = $p[2] }
}
$total = ($rows | Measure-Object Count -Sum).Sum
Get-Content "$L\civ5_lua_profile.tsv" -TotalCount 1
"`n== VM state (N=native/JIT trace, I=interp, C=in host C/C++ call, G=GC, J=JIT compile)"
$rows | Group-Object VM | ForEach-Object { [pscustomobject]@{VM=$_.Name; Samples=($_.Group|Measure-Object Count -Sum).Sum} } | Sort-Object Samples -Descending | ForEach-Object { "{0,-3} {1,8} {2,5:P1}" -f $_.VM,$_.Samples,($_.Samples/$total) }
function Agg($label, $keyFn) {
  "`n== $label"
  $rows | Group-Object $keyFn | ForEach-Object { [pscustomobject]@{K=$_.Name; S=($_.Group|Measure-Object Count -Sum).Sum; C=($_.Group|Where-Object VM -eq 'C'|Measure-Object Count -Sum).Sum} } |
    Sort-Object S -Descending | Select-Object -First $Top | ForEach-Object { "{0,7} {1,5:P1}  (in host C: {2,6})  {3}" -f $_.S,($_.S/$total),$_.C,$_.K }
}
Agg "Self time by leaf function" { $_.Leaf }
Agg "Self time by file" { ($_.Leaf -split ':')[0] }
"`n== Inclusive time by function (any frame in top 6)"
$inc = @{}
foreach ($r in $rows) { foreach ($f in ($r.Stack.TrimEnd(';') -split ';' | Select-Object -Unique)) { $inc[$f] += $r.Count } }
$inc.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top | ForEach-Object { "{0,7} {1,5:P1}  {2}" -f $_.Value,($_.Value/$total),$_.Key }

$E = "$L\civ5_lua_profile_entries.tsv"
if (Test-Path $E) {
  "`n== Host->Lua entry points (outermost calls; inclusive of host C++ calls made from Lua)"
  Get-Content $E -TotalCount 1
  Get-Content $E | Where-Object { $_ -notmatch '^#' } | ForEach-Object {
    $p = $_ -split "`t", 4
    [pscustomobject]@{ Calls=[int]$p[0]; Total=[double]$p[1]; Max=[double]$p[2]; Name=$p[3] }
  } | Sort-Object Total -Descending | Select-Object -First $Top | ForEach-Object {
    "{0,9:N1} ms  {1,8} calls  avg {2,7:N3}  max {3,8:N1}  {4}" -f $_.Total,$_.Calls,($_.Total/[math]::Max(1,$_.Calls)),$_.Max,$_.Name }
}
