$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$overlay=Get-Content -LiteralPath (Join-Path $repo 'app/src/cloudgenshin_stream_overlay.cpp') -Raw
$view=Get-Content -LiteralPath (Join-Path $repo 'app/src/cloudgenshin_stream_view.cpp') -Raw
if($overlay -match 'Native gamepad \| (cloud confirmed|awaiting cloud)') {
    throw 'Retired lower-left native-pad badge is still drawn'
}
if($overlay -notmatch 'const bool can_open=mapping_profile_!="hk4e"\s*&&') {
    throw 'Genshin LB/RB menu shortcut capture must stay disabled'
}
if($overlay -notmatch 'floating_ball_\.update\(touch_down,finger,point,bounds\)\) show_quality_menu\(true\)') {
    throw 'Touch floating-ball menu was removed with the badge'
}
if($view -notmatch 'gyro_assist\.apply\(mapping,state,motion,now_ms,' -or
   $view -notmatch 'gyro_allowed=mapping_profile_=="hk4e" && !state\.l;') {
    throw 'Production Genshin input no longer uses the tested gyro/LB gate'
}
foreach($name in @('INPUT_LB_B_SAMPLE_TICKS','INPUT_LB_X_SAMPLE_TICKS','INPUT_LB_B_SEND_TICKS','INPUT_LB_X_SEND_TICKS')) {
    if(!$view.Contains($name)) {throw "Missing chord counter: $name"}
}
Write-Output 'PASS gyro integration guard: no pad badge, touch menu retained, LB gate and bounded chord counters wired'
