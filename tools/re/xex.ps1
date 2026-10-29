# Research: an Xbox 360 game's DEFAULT.XEX -> its memory image (what the console maps at its load address), for Ghidra
# (ghidra\import.ps1 x360) and string searches. Retail encryption (AES-128: the image key is the retail key's
# decryption of the file key, then CBC over the data) and "basic" compression (runs of data and of zeros) only -
# this game's XEX uses both; LZX ("normal") compression is refused.
#   xex.ps1 <DEFAULT.XEX> <out.bin>
param([Parameter(Mandatory = $true)][string]$Xex, [Parameter(Mandatory = $true)][string]$Out)
$b = [IO.File]::ReadAllBytes($Xex)
function BE32([int64]$o) { [uint32](([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor $b[$o + 3]) }
function BE16([int64]$o) { [uint16](([uint16]$b[$o] -shl 8) -bor $b[$o + 1]) }
if ([Text.Encoding]::ASCII.GetString($b, 0, 4) -ne "XEX2") { throw "not an XEX2 file" }
$dataAt = BE32 8; $security = BE32 16; $count = BE32 20
$format = 0
for ($i = 0; $i -lt $count; $i++) { if ((BE32 (24 + 8 * $i)) -eq 0x3FF) { $format = BE32 (28 + 8 * $i) } }
if (-not $format) { throw "no file format header" }
$infoSize = BE32 $format; $encryption = BE16 ($format + 4); $compression = BE16 ($format + 6)
if ($compression -ne 1) { throw "compression $compression - only basic (1) is handled" }
$imageSize = BE32 ($security + 4)
$data = New-Object byte[] ($b.Length - $dataAt)
[Array]::Copy($b, $dataAt, $data, 0, $data.Length)
if ($encryption -eq 1) {
    $aes = [Security.Cryptography.Aes]::Create(); $aes.Padding = 'None'
    $aes.Mode = 'ECB'; $aes.Key = [byte[]](0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91)
    $key = $aes.CreateDecryptor().TransformFinalBlock($b, $security + 0x150, 16)
    $aes.Mode = 'CBC'; $aes.Key = $key; $aes.IV = New-Object byte[] 16
    $data = $aes.CreateDecryptor().TransformFinalBlock($data, 0, $data.Length - $data.Length % 16)
}
$img = New-Object byte[] $imageSize
$src = 0; $dst = 0
for ($o = $format + 8; $o -lt $format + $infoSize; $o += 8) {
    $n = BE32 $o; $zeros = BE32 ($o + 4)
    [Array]::Copy($data, $src, $img, $dst, $n)
    $src += $n; $dst += $n + $zeros
}
if ([char]$img[0] -ne 'M' -or [char]$img[1] -ne 'Z') { throw "the result isn't an image (wrong key?)" }
[IO.File]::WriteAllBytes($Out, $img)
"$Out - {0:x} bytes, load address {1:x}" -f $imageSize, (BE32 ($security + 0x110))
