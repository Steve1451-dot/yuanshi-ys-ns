$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$view=Get-Content -LiteralPath (Join-Path $repo 'app/src/cloudgenshin_stream_view.cpp') -Raw
$policy=Get-Content -LiteralPath (Join-Path $repo 'app/src/mihoyo/launcher_input_policy.hpp') -Raw
# A source guard complements executable tap/wire tests: old runtime marker
# files must not silently re-enable a third input route after an update.
if($view -match 'cloudgenshin-autotest\.(entry-mix|touch-first|touch-initial-12|key-title-probe)') {
    throw 'Retired entry marker can still select another route'
}
if($view -match 'LauncherEntryDevice|TestLauncherPadFallback|kAutoEntryTouchYs|auto_entry_move_due' -or
   $policy -match 'LauncherEntryDevice|TestLauncherPadFallback') {throw 'Mixed entry experiment survived cleanup'}
if($view -notmatch 'entry_click_enabled=mapping_profile_!="hk4e"\s*&&') {
    throw 'Legacy click probe can still replace the Genshin baseline'
}
$begin=$view.IndexOf('if(auto_entry_enabled && !entry_click_enabled &&')
$end=$view.IndexOf('if(input::ManualContinueOwnsPointer',$begin)
if($begin -lt 0 -or $end -le $begin) {throw 'Missing bounded automatic entry block'}
$entry=$view.Substring($begin,$end-$begin)
if($entry -match 'send_keyboard|kTouchActionMove|press\.a=true') {throw 'Automatic mobile entry mixes another input device'}
if($entry -notmatch 'if\(launcher_touch_\)' -or $entry -notmatch 'auto_entry_tap\.sent' -or
   $entry -notmatch 'launcher_entered_ && !auto_entry_stopped') {throw 'Baseline ownership or stop handling missing'}
if($view -notmatch 'autotest_game_enter_ms_\.load\(\)>0\);') {throw 'Manual pad test lost the game-entry guard'}
Write-Output 'PASS Genshin entry baseline: retired markers inert, no mixed-device/coordinate/Move experiments'
