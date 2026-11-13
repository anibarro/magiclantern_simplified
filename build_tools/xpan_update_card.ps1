# Copy a verified XPan package to the camera card and check the result.
#   powershell -ExecutionPolicy Bypass -File build_tools\xpan_update_card.ps1 -Cam M50.110
#   ... -Cam M50.110 -Build platform\M50.110\xpan_builds\20261108-101500-3ce4fe41a9c2   (put back an archived build)
#
# Checks, in order; any failure stops before or after copying with an error:
#  1. The card is a removable drive whose ML\MODULES holds this camera's .sym
#     (EOSM_202.sym, 6D_116.sym, M50_110.sym): a build never goes on the other
#     camera's card.
#  2. The package passed build_xpan.sh (it is the build output or an archive).
#  3. After copying, every file on the card matches the package byte for byte.
#  4. The boot sector carries EOS_DEVELOP and BOOTDISK at the offsets for its
#     file system, the exFAT boot checksum is right, and the first bytes are
#     intact (EOScard once wrote "SCRIPT" over them on an exFAT card).
#  5. No stray .FIR is left at the card root. The package's boot-flag
#     installer (BOOT_M50.FIR, ML-SETUP.FIR) is for a first install from the
#     zip, so it is never copied to the working card.
# Each update is recorded in platform\<Cam>\xpan_builds\card-history.log.
param(
    [Parameter(Mandatory)][ValidateSet('EOSM.202', '6D.116', 'M50.110')][string]$Cam,
    [string]$Build,
    [string]$Drive
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$sym = @{ 'EOSM.202' = 'EOSM_202.sym'; '6D.116' = '6D_116.sym'; 'M50.110' = 'M50_110.sym' }[$Cam]
function Fail($msg) { Write-Host "CARD CHECK FAILED: $msg" -ForegroundColor Red; exit 1 }

# 2. Package
if (-not $Build) { $Build = Join-Path $repo "platform\$Cam\build_xpan" }
$zip = Join-Path $Build 'magiclantern.zip'
if (-not (Test-Path $zip)) { Fail "no magiclantern.zip in $Build (run build_tools/build_xpan.sh first)" }

# 1. Card
$cards = Get-CimInstance Win32_LogicalDisk -Filter 'DriveType=2' | Where-Object { $_.Size } |
    Where-Object { Test-Path "$($_.DeviceID)\ML\MODULES\$sym" }
if ($Drive) { $cards = $cards | Where-Object { $_.DeviceID -eq "$($Drive.TrimEnd(':')):" } }
$cards = @($cards)
if ($cards.Count -eq 0) { Fail "no card with ML\MODULES\$sym inserted" }
if ($cards.Count -gt 1) { Fail "more than one $Cam card inserted; pass -Drive" }
$card = $cards[0].DeviceID
"card: $card ($($cards[0].FileSystem), $Cam)"

# Copy
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("xpan_card_" + [guid]::NewGuid().ToString('N'))
Expand-Archive -Path $zip -DestinationPath $tmp
try {
    Get-ChildItem $tmp -File -Filter '*.FIR' | Remove-Item -Force   # installer: zip only
    Copy-Item -Path (Join-Path $tmp '*') -Destination "$card\" -Recurse -Force
    # 3. Byte-for-byte
    $bad = @()
    $files = Get-ChildItem $tmp -Recurse -File
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($tmp.Length + 1)
        $dst = Join-Path "$card\" $rel
        if (-not (Test-Path $dst) -or (Get-FileHash $dst).Hash -ne (Get-FileHash $f.FullName).Hash) { $bad += $rel }
    }
    if ($bad.Count) { Fail ("files differ on the card: " + ($bad -join ', ')) }
    "ok: $($files.Count) files on the card match the package"
} finally { Remove-Item -Recurse -Force $tmp }

# 4. Boot sector
$fs = [IO.File]::Open("\\.\$card", 'Open', 'Read', 'ReadWrite')
$b = New-Object byte[] 6144
[void]$fs.Read($b, 0, 6144)
$fs.Close()
$a = [Text.Encoding]::ASCII
$type = $cards[0].FileSystem
$off = @{ 'exFAT' = @(0x82, 0x7A); 'FAT32' = @(0x47, 0x5C); 'FAT' = @(0x2B, 0x40) }[$type]
if (-not $off) { Fail "unknown file system $type" }
if ($a.GetString($b, $off[0], 11) -ne 'EOS_DEVELOP' -or $a.GetString($b, $off[1], 8) -ne 'BOOTDISK') {
    Fail "boot markers missing ($type): the camera will not load ML from this card"
}
if ($b[0x1FE] -ne 0x55 -or $b[0x1FF] -ne 0xAA) { Fail "boot sector signature missing" }
if ($type -eq 'exFAT') {
    if ($b[0] -ne 0xEB -or $b[1] -ne 0x76 -or $b[2] -ne 0x90 -or $a.GetString($b, 3, 8) -ne 'EXFAT   ') { Fail "exFAT boot sector header damaged" }
    [uint32]$sum = 0
    for ($i = 0; $i -lt 5632; $i++) {
        if ($i -eq 106 -or $i -eq 107 -or $i -eq 112) { continue }
        $sum = [uint32]((((([uint64]$sum -shr 1) -bor (([uint64]$sum -band 1) -shl 31)) + $b[$i])) -band [uint64]4294967295)
    }
    if ([BitConverter]::ToUInt32($b, 5632) -ne $sum) { Fail "exFAT boot checksum wrong" }
}
"ok: boot markers and boot sector ($type)"

# 5. Stray firmware files
$fir = @(Get-ChildItem "$card\*.FIR" -ErrorAction SilentlyContinue)
if ($fir.Count) { Fail ("remove " + ($fir.Name -join ', ') + " from the card root") }
"ok: no .FIR at the card root"

$hash = (Get-FileHash "$card\autoexec.bin").Hash.Substring(0, 12).ToLower()
$hist = Join-Path $repo "platform\$Cam\xpan_builds"
New-Item -ItemType Directory -Force $hist | Out-Null
Add-Content (Join-Path $hist 'card-history.log') ("{0}  {1}  autoexec {2}  from {3}" -f (Get-Date -Format s), $card, $hash, $Build)
"CARD UPDATED ($Cam, autoexec $hash)"
