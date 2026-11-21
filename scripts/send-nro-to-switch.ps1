param(
    [Parameter(Position=0)][string]$NroPath,
    [switch]$CheckOnly,
    [ValidateRange(15,180)][int]$TimeoutSeconds=90,
    [ValidateRange(0,60)][int]$WaitForDeviceSeconds=15,
    [switch]$LibraryOnly
)
$ErrorActionPreference='Stop'

# Public program files only. Never enumerate/copy accounts or configurations,
# remove a device file, or overwrite an older NRO. Shell MTP has no drive letter.
function Get-DeployFileDigest([string]$Path) {
    $stream=[IO.File]::Open($Path,'Open','Read','Read')
    $sha=[Security.Cryptography.SHA256]::Create()
    try {
        [pscustomobject]@{Length=$stream.Length;Sha256=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}
    } finally {$sha.Dispose();$stream.Dispose()}
}

function Get-DeployNroInfo([string]$Path) {
    $file=Get-Item -LiteralPath $Path
    if($file.PSIsContainer -or $file.Name -notmatch '^yuanshi-launcher-(\d+\.\d+\.\d+)\.nro$') {
        throw 'Source must be a versioned yuanshi-launcher NRO.'
    }
    $expectedVersion=$Matches[1]
    $stream=[IO.File]::Open($file.FullName,'Open','Read','Read')
    $reader=[IO.BinaryReader]::new($stream)
    try {
        if($stream.Length -lt 128 -or $stream.Length -gt 512MB) {throw 'Invalid source NRO size.'}
        $stream.Position=0x10
        if([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -cne 'NRO0') {throw 'Source is not an NRO.'}
        $stream.Position=0x18;$asset=[long]$reader.ReadUInt32()
        if($asset -lt 128 -or $asset+56 -gt $stream.Length) {throw 'Invalid NRO assets.'}
        $stream.Position=$asset
        if([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -cne 'ASET') {throw 'Missing NRO assets.'}
        $stream.Position=$asset+24
        $nacp=$reader.ReadUInt64();$nacpSize=$reader.ReadUInt64()
        if($nacpSize -lt 0x4000 -or $nacp -gt $stream.Length-$asset -or
           $nacpSize -gt $stream.Length-$asset-[long]$nacp) {throw 'Invalid NACP bounds.'}
        $stream.Position=$asset+[long]$nacp
        $name=[Text.Encoding]::UTF8.GetString($reader.ReadBytes(512)).Split([char]0)[0]
        $stream.Position=$asset+[long]$nacp+0x3060
        $version=[Text.Encoding]::ASCII.GetString($reader.ReadBytes(16)).Split([char]0)[0]
        if($name -cne 'yuanshi launcher' -or $version -cne $expectedVersion) {
            throw 'NRO product/version does not match the source filename.'
        }
    } finally {$reader.Dispose()}
    $manifest=$file.FullName+'.sha256.txt'
    if(!(Test-Path -LiteralPath $manifest -PathType Leaf)) {throw 'Missing NRO SHA256 manifest; package the build first.'}
    $text=([IO.File]::ReadAllText($manifest)).Trim()
    if($text -notmatch '^([a-fA-F0-9]{64})[ \t]+\*?([^\r\n]+)$') {throw 'Invalid SHA256 manifest.'}
    $expectedHash=$Matches[1].ToUpperInvariant();$manifestName=$Matches[2]
    if($manifestName -cne $file.Name) {throw 'SHA256 manifest filename mismatch.'}
    $hash=(Get-DeployFileDigest $file.FullName).Sha256
    if($hash -cne $expectedHash) {throw 'Source SHA256 mismatch; nothing was sent.'}
    [pscustomobject]@{Path=$file.FullName;Name=$file.Name;Version=$version;Length=$file.Length;Sha256=$hash}
}

function Find-DeployNro([string]$ExplicitPath,[string]$DistPath) {
    if($ExplicitPath) {return Get-DeployNroInfo $ExplicitPath}
    $candidates=@(Get-ChildItem -LiteralPath $DistPath -File | ForEach-Object {
        if($_.Name -match '^yuanshi-launcher-(\d+\.\d+\.\d+)\.nro$') {
            [pscustomobject]@{Path=$_.FullName;Version=[Version]$Matches[1]}
        }
    } | Sort-Object Version -Descending)
    if(!$candidates.Count) {throw 'No packaged versioned NRO found in dist.'}
    # Fail closed if the newest package is incomplete; do not silently install an older one.
    Get-DeployNroInfo $candidates[0].Path
}

function Get-CollisionNroName([string]$Name,[string]$Hash) {
    if($Name -notmatch '^yuanshi-launcher-\d+\.\d+\.\d+\.nro$' -or $Hash -notmatch '^[a-fA-F0-9]{64}$') {
        throw 'Invalid collision filename input.'
    }
    [IO.Path]::GetFileNameWithoutExtension($Name)+'-'+$Hash.Substring(0,8).ToLowerInvariant()+'.nro'
}

function Find-MtpChild($Folder,[string]$Name,[bool]$IsFolder) {
    $items=@($Folder.Items() | Where-Object {$_.Name -ieq $Name})
    if($items.Count -gt 1) {throw ('Ambiguous MTP item: '+$Name)}
    if(!$items.Count) {return $null}
    if([bool]$items[0].IsFolder -ne $IsFolder) {throw ('Unexpected MTP item type: '+$Name)}
    $items[0]
}

function Read-MtpNro($Shell,$Item,$Context,[datetime]$Deadline) {
    $folder=Join-Path $Context.Root ('read-'+$Context.Directories.Count)
    New-Item -ItemType Directory -Path $folder | Out-Null
    $Context.Directories.Add($folder)
    $local=$Shell.Namespace((Get-Item -LiteralPath $folder).FullName)
    if(!$local) {throw 'Could not open local verification folder.'}
    # Silent + rename-on-collision + no-confirm-mkdir + no-error-UI. Never overwrite.
    $local.CopyHere($Item,1548)
    $path=Join-Path $folder $Item.Name
    $Context.Files.Add($path)
    $last=''
    while([datetime]::UtcNow -lt $Deadline) {
        if(Test-Path -LiteralPath $path -PathType Leaf) {
            try {
                # FileShare.Read denies concurrent writers: do not hash a partial MTP copy.
                $digest=Get-DeployFileDigest $path
                $length=$digest.Length;$hash=$digest.Sha256
                $signature=[string]$length+':'+$hash
                if($signature -ceq $last) {return [pscustomobject]@{Path=$path;Length=$length;Sha256=$hash}}
                $last=$signature
            } catch [IO.IOException] {$last=''}
        }
        Start-Sleep -Milliseconds 400
    }
    throw 'Timed out reading the NRO back from Switch; transfer is NOT verified.'
}

function Invoke-SwitchNroDeploy {
    $repo=Split-Path $PSScriptRoot -Parent
    $source=Find-DeployNro $NroPath (Join-Path $repo 'dist')
    Write-Host ('Source: '+$source.Path)
    Write-Host ('Version: '+$source.Version+'    SHA256: '+$source.Sha256)
    $shell=New-Object -ComObject Shell.Application
    $deviceDeadline=[datetime]::UtcNow.AddSeconds($WaitForDeviceSeconds)
    $waitingReported=$false
    do {
        $devices=@($shell.Namespace(17).Items() | Where-Object {$_.IsFolder -and $_.Name -in @('Nintendo Switch','Switch')})
        if($devices.Count -gt 0 -or [datetime]::UtcNow -ge $deviceDeadline) {break}
        if(!$waitingReported) {Write-Host ('Waiting up to '+$WaitForDeviceSeconds+' seconds for Switch USB MTP...');$waitingReported=$true}
        Start-Sleep -Milliseconds 800
    } while($true)
    if($devices.Count -ne 1) {throw 'Connect exactly one Switch and open its USB MTP mode first.'}
    $device=$devices[0].GetFolder
    $storage=@($device.Items() | Where-Object {$_.IsFolder -and $_.Name -in @('microSD card','1: SD Card','SD Card')})
    if($storage.Count -ne 1) {throw 'Switch microSD storage is not uniquely available.'}
    $sd=$storage[0].GetFolder
    $switchItem=Find-MtpChild $sd 'switch' $true
    if(!$switchItem) {throw 'microSD card/switch folder is missing; nothing was sent.'}
    $target=$switchItem.GetFolder
    Write-Host ('Target: '+$devices[0].Name+'/'+$storage[0].Name+'/switch/'+$source.Name)
    if($CheckOnly) {Write-Host 'CHECK ONLY: source verified, MTP destination found; no device writes.';return}
    $deadline=[datetime]::UtcNow.AddSeconds($TimeoutSeconds)
    $taskRoot=Join-Path ([IO.Path]::GetTempPath()) ('yuanshi-mtp-'+[Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $taskRoot | Out-Null
    $taskRoot=(Get-Item -LiteralPath $taskRoot).FullName
    $context=[pscustomobject]@{Root=$taskRoot;Directories=[Collections.Generic.List[string]]::new();Files=[Collections.Generic.List[string]]::new()}
    try {
        $name=$source.Name
        $existing=Find-MtpChild $target $name $false
        if($existing) {
            Write-Host 'Same filename exists; reading back before making any changes...'
            $copy=Read-MtpNro $shell $existing $context $deadline
            if($copy.Sha256 -ceq $source.Sha256) {Write-Host 'OK: this exact NRO is already on Switch. No files changed.';return}
            $name=Get-CollisionNroName $source.Name $source.Sha256
            Write-Host ('Existing NRO preserved. New filename: '+$name)
            $existing=Find-MtpChild $target $name $false
            if($existing) {
                $copy=Read-MtpNro $shell $existing $context $deadline
                if($copy.Sha256 -ceq $source.Sha256) {Write-Host 'OK: the hash-suffixed NRO is already on Switch. No files changed.';return}
                throw 'Hash-suffixed filename also exists with different data; all device files preserved.'
            }
        }
        $upload=$source.Path
        if($name -cne $source.Name) {
            $upload=Join-Path $taskRoot $name
            Copy-Item -LiteralPath $source.Path -Destination $upload
            $context.Files.Add($upload)
        }
        # Race-safe: if another file appears meanwhile, Shell must rename, not overwrite it.
        if(Find-MtpChild $target $name $false) {throw 'Destination appeared during preparation; rerun to recheck it.'}
        Write-Host 'Copying only the new NRO. All older programs, accounts and settings are preserved...'
        $target.CopyHere($upload,1548)
        $remote=$null
        while(!$remote -and [datetime]::UtcNow -lt $deadline) {
            $remote=Find-MtpChild $target $name $false
            if(!$remote) {Start-Sleep -Milliseconds 400}
        }
        if(!$remote) {throw 'New NRO did not appear on Switch before the deadline.'}
        for($attempt=0;$attempt -lt 3 -and [datetime]::UtcNow -lt $deadline;$attempt++) {
            Write-Host ('Reading back for SHA256 verification (attempt '+($attempt+1)+')...')
            $remote=Find-MtpChild $target $name $false
            $copy=Read-MtpNro $shell $remote $context $deadline
            if($copy.Length -eq $source.Length -and $copy.Sha256 -ceq $source.Sha256) {
                Write-Host ('OK: switch/'+$name)
                Write-Host ('READ-BACK SHA256 MATCH: '+$source.Sha256)
                return
            }
            Start-Sleep -Milliseconds 500
        }
        throw 'Read-back did not match. Do not run the new file; old files have NOT been removed.'
    } finally {
        # Remove only local, exact files created by this invocation; never an MTP object.
        $prefix=$taskRoot+[IO.Path]::DirectorySeparatorChar
        foreach($file in $context.Files) {
            $full=[IO.Path]::GetFullPath($file)
            if(!$full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {throw 'Unsafe temporary file cleanup refused.'}
            if(Test-Path -LiteralPath $full -PathType Leaf) {Remove-Item -LiteralPath $full}
        }
        foreach($directory in $context.Directories) {
            if(!(Get-ChildItem -LiteralPath $directory -Force)) {Remove-Item -LiteralPath $directory}
        }
        if(!(Get-ChildItem -LiteralPath $taskRoot -Force)) {Remove-Item -LiteralPath $taskRoot}
    }
}

if($LibraryOnly) {return}
try {Invoke-SwitchNroDeploy}
catch {Write-Host ('FAILED: '+$_.Exception.Message) -ForegroundColor Red;exit 1}
