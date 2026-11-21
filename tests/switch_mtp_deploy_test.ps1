$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../scripts/send-nro-to-switch.ps1') -LibraryOnly
$taskRoot=Join-Path ([IO.Path]::GetTempPath()) ('yuanshi-mtp-policy-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskRoot | Out-Null
$taskRoot=(Get-Item -LiteralPath $taskRoot).FullName
$owned=[Collections.Generic.List[string]]::new()
function Assert-Deploy([bool]$Condition,[string]$Message) {if(!$Condition) {throw $Message}}
function Expect-Rejection([scriptblock]$Action) {
    $rejected=$false
    try {& $Action|Out-Null} catch {$rejected=$true}
    Assert-Deploy $rejected 'Unsafe/incomplete package was accepted.'
}
function New-TestNro([string]$Version,[string]$EmbeddedVersion=$Version) {
    $name='yuanshi-launcher-'+$Version+'.nro'
    $path=Join-Path $taskRoot $name
    $stream=[IO.File]::Open($path,'CreateNew','Write','None')
    $owned.Add($path)
    $writer=[IO.BinaryWriter]::new($stream)
    try {
        $stream.SetLength(0x80+0x38+0x4000)
        $stream.Position=0x10;$writer.Write([Text.Encoding]::ASCII.GetBytes('NRO0'))
        $stream.Position=0x18;$writer.Write([uint32]0x80)
        $stream.Position=0x80;$writer.Write([Text.Encoding]::ASCII.GetBytes('ASET'))
        $stream.Position=0x80+24;$writer.Write([uint64]0x38);$writer.Write([uint64]0x4000)
        $stream.Position=0x80+0x38;$writer.Write([Text.Encoding]::UTF8.GetBytes('yuanshi launcher'))
        $stream.Position=0x80+0x38+0x3060;$writer.Write([Text.Encoding]::ASCII.GetBytes($EmbeddedVersion))
    } finally {$writer.Dispose()}
    $manifest=$path+'.sha256.txt';$owned.Add($manifest)
    [IO.File]::WriteAllText($manifest,(Get-DeployFileDigest $path).Sha256+'  '+$name+"`n")
    $path
}
try {
    $abc=Join-Path $taskRoot 'digest.bin';$owned.Add($abc);[IO.File]::WriteAllBytes($abc,[Text.Encoding]::ASCII.GetBytes('abc'))
    Assert-Deploy ((Get-DeployFileDigest $abc).Sha256 -ceq 'BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD') 'SHA256 known-answer failed.'
    $old=New-TestNro '0.2.9'
    $new=New-TestNro '0.2.10'
    Assert-Deploy ((Find-DeployNro '' $taskRoot).Version -ceq '0.2.10') 'Version sorting was lexical, not numeric.'
    Assert-Deploy ((Find-DeployNro $old $taskRoot).Version -ceq '0.2.9') 'Explicit version selection failed.'
    $hash=(Get-DeployFileDigest $new).Sha256
    Assert-Deploy ((Get-CollisionNroName 'yuanshi-launcher-0.2.10.nro' $hash) -ceq
        ('yuanshi-launcher-0.2.10-'+$hash.Substring(0,8).ToLowerInvariant()+'.nro')) 'Collision suffix failed.'
    Expect-Rejection {Get-CollisionNroName '../other.nro' $hash}
    Expect-Rejection {Get-CollisionNroName 'yuanshi-launcher-0.2.10.nro' 'bad'}
    $wrong=New-TestNro '0.2.11' '0.2.10'
    Expect-Rejection {Find-DeployNro '' $taskRoot} # No silent downgrade to .10.
    Expect-Rejection {Get-DeployNroInfo $wrong}
    [IO.File]::WriteAllText($new+'.sha256.txt',(('0'*64)+'  '+[IO.Path]::GetFileName($new)))
    Expect-Rejection {Get-DeployNroInfo $new}
    [IO.File]::WriteAllText($old+'.sha256.txt',((Get-DeployFileDigest $old).Sha256+'  another.nro'))
    Expect-Rejection {Get-DeployNroInfo $old}
    Write-Host 'PASS MTP deploy: known SHA256, NRO/NACP/version bounds, semantic latest, manifest rejection and non-overwrite collision naming; no device writes.'
} finally {
    $prefix=$taskRoot+[IO.Path]::DirectorySeparatorChar
    foreach($path in $owned) {
        $full=[IO.Path]::GetFullPath($path)
        if(!$full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {throw 'Unsafe fixture cleanup target.'}
        if(Test-Path -LiteralPath $full -PathType Leaf) {Remove-Item -LiteralPath $full}
    }
    if(!(Get-ChildItem -LiteralPath $taskRoot -Force)) {Remove-Item -LiteralPath $taskRoot}
}
