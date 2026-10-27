# Checks whether an FH2_ExtraOptions.ini is accepted by this mod. Exit code 0 = ok, 1 = would be refused.
param([string]$Path)
$allowed = @{
  'global'=@('enabled'); 'general'=@('enabled');
  'content'=@('unlockdlc','offlinevip','unlockstormislandcars','unlocktreasuremap');
  'frontend'=@('skipintro','carsnexttohome','menuturbointervalms','allrimscategory','enablemarketplace','enablesocial','enableclubs','enablerivals','getincarfrompausemenu','hideonlinetabs','snaptoroadoncarchange','skipcarmeetonstartup','spawnatfestival','skipineligiblecardialogue','allengineswaps');
  'compatibility'=@('disableparentallocks','disableazimuthsavechecksums');
  'profile'=@('customgamerpic'); 'photomode'=@('freecamera','samples'); 'files'=@('enablemediaoverrides');
  'autodrive'=@('enabled','cinematic'); 'livery'=@('enable4k','intermediatemsaa','extrasupersampling');
  'graphics'=@('smoothsunshadows','forceanisofiltering','motionblursamples'); 'hud'=@('minimapfollowscamera');
  'language'=@('text','audio'); 'camera'=@('driftcamera','driftcamerastiffness','firstpersondriftcamerastiffness');
  'experiments'=@('localdrivatarliveries','maxdrivatarliveries','allengineswaps','alltransmissionswaps','unlockaerotuning');
  'gameplay'=@('unlockaerotuning'); 'bindings'=@()
}
try { $bytes = [IO.File]::ReadAllBytes($Path) } catch { exit 0 }
if ($bytes.Length -ge 2 -and (($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or ($bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF))) { exit 1 }   # UTF-16
if ($bytes -contains 0) { exit 1 }
if ($bytes.Length -gt 8192) { exit 1 }   # the mod reads at most 8 KB
$text = [Text.Encoding]::UTF8.GetString($bytes)
$section = ''
foreach ($raw in ($text -split "`n")) {
  $line = ($raw -replace '[;#].*$','').Trim()
  if ($line -eq '') { continue }
  if ($line.StartsWith('[')) {
    if (-not $line.EndsWith(']')) { exit 1 }
    $section = $line.Substring(1, $line.Length-2).Trim().ToLower()
    if (-not $allowed.ContainsKey($section)) { exit 1 }
    continue
  }
  $i = $line.IndexOf('=')
  if ($i -lt 0 -or $section -eq '') { exit 1 }
  $key = $line.Substring(0,$i).Trim().ToLower()
  if ($section -eq 'bindings') { continue }
  if ($allowed[$section] -notcontains $key) { exit 1 }
}
exit 0
