# Make an exFAT camera card bootable for Magic Lantern: writes BOOTDISK (0x7A)
# and EOS_DEVELOP (0x82) into the main and backup boot regions and recomputes
# the exFAT boot checksum. Nothing else is written (no SCRIPT flag).
# Run in an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File make-card-bootable.ps1 -Drive P
param([Parameter(Mandatory)][string]$Drive)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace Ml -Name Vol -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool DeviceIoControl(Microsoft.Win32.SafeHandles.SafeFileHandle h, uint code, IntPtr inBuf, uint inSize, IntPtr outBuf, uint outSize, out uint ret, IntPtr ov);
'@
$letter = $Drive.TrimEnd(':').ToUpper()
$vol = Get-Volume -DriveLetter $letter
if ($vol.DriveType -ne 'Removable' -or $vol.FileSystemType -notin 'exFAT','FAT32') { throw "$($letter): is not a removable exFAT or FAT32 volume. Nothing written." }

if ($vol.FileSystemType -eq 'FAT32') {
    # FAT32 (added 2026-11-08 for a small test card): EOS_DEVELOP at 0x47 and
    # BOOTDISK at 0x5C (same offsets as xpan_update_card.ps1 checks), in the
    # boot sector and its backup (sector number at offset 0x32). No checksum.
    $h = [Ml.Vol]::CreateFile("\\.\$($letter):", [uint32]3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    if ($h.IsInvalid) { throw "Cannot open $($letter): (run PowerShell as administrator)." }
    $fs = New-Object System.IO.FileStream($h, [System.IO.FileAccess]::ReadWrite)
    try {
        $r = 0
        if (-not [Ml.Vol]::DeviceIoControl($h, 0x90018, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$r, [IntPtr]::Zero)) { throw "Cannot lock the volume: close Explorer windows and programs using the card." }
        [void][Ml.Vol]::DeviceIoControl($h, 0x90020, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$r, [IntPtr]::Zero)   # dismount
        $b = New-Object byte[] 512
        if ($fs.Read($b, 0, 512) -ne 512) { throw "short read" }
        $a = [Text.Encoding]::ASCII
        if ($b[0x1FE] -ne 0x55 -or $b[0x1FF] -ne 0xAA -or $a.GetString($b, 0x52, 8) -ne 'FAT32   ' -or [BitConverter]::ToUInt16($b, 11) -ne 512) { throw "Sector 0 is not a FAT32 boot sector with 512-byte sectors. Nothing written." }
        $backup = [BitConverter]::ToUInt16($b, 0x32)
        $sectors = @(0)
        if ($backup -gt 0 -and $backup -lt 32) { $sectors += $backup }
        foreach ($s in $sectors) {
            $fs.Position = $s * 512
            $sec = New-Object byte[] 512
            if ($fs.Read($sec, 0, 512) -ne 512) { throw "short read at sector $s" }
            if ($sec[0x1FE] -ne 0x55 -or $sec[0x1FF] -ne 0xAA -or $a.GetString($sec, 0x52, 8) -ne 'FAT32   ') { throw "Sector $s is not a FAT32 boot sector. Nothing written." }
            [Array]::Copy($a.GetBytes('EOS_DEVELOP'), 0, $sec, 0x47, 11)
            [Array]::Copy($a.GetBytes('BOOTDISK'), 0, $sec, 0x5C, 8)
            $fs.Position = $s * 512
            $fs.Write($sec, 0, 512)
            "FAT32 boot sector $($s): markers written"
        }
        $fs.Flush()
        "Done: $($letter): is bootable."
    } finally { $fs.Close() }
    return
}

# GENERIC_READ|GENERIC_WRITE (0xC0000000) in decimal: PowerShell 5.1 reads the hex literal as a negative Int32.
$h = [Ml.Vol]::CreateFile("\\.\$($letter):", [uint32]3221225472, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
if ($h.IsInvalid) { throw "Cannot open $($letter): (run PowerShell as administrator)." }
$fs = New-Object System.IO.FileStream($h, [System.IO.FileAccess]::ReadWrite)
try {
    $r = 0
    if (-not [Ml.Vol]::DeviceIoControl($h, 0x90018, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$r, [IntPtr]::Zero)) { throw "Cannot lock the volume: close Explorer windows and programs using the card." }
    [void][Ml.Vol]::DeviceIoControl($h, 0x90020, [IntPtr]::Zero, 0, [IntPtr]::Zero, 0, [ref]$r, [IntPtr]::Zero)   # dismount

    $b = New-Object byte[] 12288        # main boot region (sectors 0-11) + backup (12-23)
    if ($fs.Read($b, 0, 12288) -ne 12288) { throw "short read" }
    $a = [Text.Encoding]::ASCII
    foreach ($base in 0, 6144) {
        if ($b[$base] -ne 0xEB -or $b[$base+1] -ne 0x76 -or $b[$base+2] -ne 0x90 -or $a.GetString($b, $base + 3, 8) -ne 'EXFAT   ') { throw "Boot region at $base is not a valid exFAT boot sector. Nothing written." }
        if ($b[$base + 0x1FE] -ne 0x55 -or $b[$base + 0x1FF] -ne 0xAA -or $b[$base + 108] -ne 9) { throw "Unexpected boot sector at $base. Nothing written." }
        [Array]::Copy($a.GetBytes('BOOTDISK'), 0, $b, $base + 0x7A, 8)
        [Array]::Copy($a.GetBytes('EOS_DEVELOP'), 0, $b, $base + 0x82, 11)
        # exFAT boot checksum: sectors 0-10, skipping VolumeFlags (106,107) and PercentInUse (112)
        [uint32]$sum = 0
        for ($i = 0; $i -lt 5632; $i++) {
            if ($i -eq 106 -or $i -eq 107 -or $i -eq 112) { continue }
            $sum = [uint32]((((([uint64]$sum -shr 1) -bor (([uint64]$sum -band 1) -shl 31)) + $b[$base + $i])) -band [uint64]4294967295)
        }
        $word = [BitConverter]::GetBytes($sum)
        for ($k = 0; $k -lt 512; $k += 4) { [Array]::Copy($word, 0, $b, $base + 5632 + $k, 4) }
        "Boot region at {0}: markers written, checksum {1:X8}" -f $base, $sum
    }
    $fs.Position = 0
    $fs.Write($b, 0, 12288)
    $fs.Flush()
    "Done: $($letter): is bootable."
} finally { $fs.Close() }
