param([string]$ProjectRoot = $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$assetRoot = Join-Path $ProjectRoot 'Assets'

function Write-P6Texture([string]$Name, [int]$Width, [int]$Height, [scriptblock]$Pixel)
{
    $path = Join-Path $assetRoot ("Textures/{0}.ppm" -f $Name)
    if (Test-Path -LiteralPath $path)
    {
        return
    }

    $header = [Text.Encoding]::ASCII.GetBytes("P6`n$Width $Height`n255`n")
    $rgb = [byte[]]::new($Width * $Height * 3)
    for ($y = 0; $y -lt $Height; ++$y)
    {
        for ($x = 0; $x -lt $Width; ++$x)
        {
            $color = & $Pixel $x $y
            $offset = ($y * $Width + $x) * 3
            $rgb[$offset] = [byte]$color[0]
            $rgb[$offset + 1] = [byte]$color[1]
            $rgb[$offset + 2] = [byte]$color[2]
        }
    }
    $bytes = [byte[]]::new($header.Length + $rgb.Length)
    [Array]::Copy($header, 0, $bytes, 0, $header.Length)
    [Array]::Copy($rgb, 0, $bytes, $header.Length, $rgb.Length)
    [IO.File]::WriteAllBytes($path, $bytes)
}

Write-P6Texture 'AlbedoTiles' 128 128 {
    param($x, $y)
    if (($x % 32) -lt 2 -or ($y % 32) -lt 2) { return @(100, 91, 82) }
    $variation = [int](($x / 32 + $y / 32) % 2) * 14
    return @( [Math]::Min(178, 143 + $variation), [Math]::Min(118, 72 + $variation), [Math]::Min(93, 48 + $variation) )
}

Write-P6Texture 'MetallicRoughness' 128 128 {
    param($x, $y)
    $roughness = 36 + [int](180 * $y / 127)
    $metallic = if (($x % 64) -lt 6) { 18 } elseif ($x -ge 64) { 230 } else { 45 }
    return @(255, $roughness, $metallic)
}

Write-P6Texture 'NormalBumps' 128 128 {
    param($x, $y)
    $nx = [Math]::Sin($x * [Math]::PI / 12.0) * 0.25
    $ny = [Math]::Cos($y * [Math]::PI / 16.0) * 0.25
    $nz = [Math]::Sqrt([Math]::Max(0.0, 1.0 - $nx * $nx - $ny * $ny))
    return @([Math]::Round(127.5 + 127.5 * $nx), [Math]::Round(127.5 + 127.5 * $ny), [Math]::Round(127.5 + 127.5 * $nz))
}

Write-P6Texture 'OcclusionGrid' 128 128 {
    param($x, $y)
    $value = if (($x % 32) -lt 4 -or ($y % 32) -lt 4) { 70 } else { 245 }
    return @($value, 255, 255)
}

Write-P6Texture 'EmissiveStripes' 128 128 {
    param($x, $y)
    if (($x % 32) -lt 8) { return @(80, 190, 255) }
    if (($y % 64) -lt 5) { return @(255, 96, 28) }
    return @(0, 0, 0)
}

# The model source is a flat-shaded six-face cube with authored UVs, normals, and tangent handedness.
$vertices = [Collections.Generic.List[float]]::new()
$uvs = [Collections.Generic.List[float]]::new()
$normals = [Collections.Generic.List[float]]::new()
$tangents = [Collections.Generic.List[float]]::new()
$indices = [Collections.Generic.List[uint16]]::new()
$faces = @(
    @{ P=@(@(-.5,-.5,.5),@(.5,-.5,.5),@(.5,.5,.5),@(-.5,.5,.5)); N=@(0,0,1); T=@(1,0,0) },
    @{ P=@(@(.5,-.5,-.5),@(-.5,-.5,-.5),@(-.5,.5,-.5),@(.5,.5,-.5)); N=@(0,0,-1); T=@(-1,0,0) },
    @{ P=@(@(.5,-.5,.5),@(.5,-.5,-.5),@(.5,.5,-.5),@(.5,.5,.5)); N=@(1,0,0); T=@(0,0,-1) },
    @{ P=@(@(-.5,-.5,-.5),@(-.5,-.5,.5),@(-.5,.5,.5),@(-.5,.5,-.5)); N=@(-1,0,0); T=@(0,0,1) },
    @{ P=@(@(-.5,.5,-.5),@(-.5,.5,.5),@(.5,.5,.5),@(.5,.5,-.5)); N=@(0,1,0); T=@(0,0,1) },
    @{ P=@(@(-.5,-.5,.5),@(-.5,-.5,-.5),@(.5,-.5,-.5),@(.5,-.5,.5)); N=@(0,-1,0); T=@(0,0,-1) }
)
foreach ($face in $faces)
{
    $base = [uint16]($vertices.Count / 3)
    foreach ($corner in 0..3)
    {
        foreach ($component in $face.P[$corner]) { $vertices.Add([float]$component) }
        $uv = switch ($corner) { 0 {@(0,0)} 1 {@(1,0)} 2 {@(1,1)} 3 {@(0,1)} }
        $uvs.Add([float]$uv[0]); $uvs.Add([float]$uv[1])
        foreach ($component in $face.N) { $normals.Add([float]$component) }
        foreach ($component in $face.T) { $tangents.Add([float]$component) }
        $tangents.Add([float]1)
    }
    foreach ($index in @($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3))) { $indices.Add([uint16]$index) }
}

$buffer = [Collections.Generic.List[byte]]::new()
foreach ($value in $vertices) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
foreach ($value in $uvs) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
foreach ($value in $normals) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
foreach ($value in $tangents) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
foreach ($value in $indices) { $buffer.AddRange([BitConverter]::GetBytes([uint16]$value)) }
$base64 = [Convert]::ToBase64String($buffer.ToArray())
$gltfPath = Join-Path $assetRoot 'Models/PbrTestCube.gltf'
if (Test-Path -LiteralPath $gltfPath) { return }
$gltf = @"
{
  "asset": { "version": "2.0", "generator": "PulseForge procedural PBR validation asset" },
  "buffers": [{ "byteLength": $($buffer.Count), "uri": "data:application/octet-stream;base64,$base64" }],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": $($vertices.Count * 4) },
    { "buffer": 0, "byteOffset": $($vertices.Count * 4), "byteLength": $($uvs.Count * 4) },
    { "buffer": 0, "byteOffset": $($vertices.Count * 4 + $uvs.Count * 4), "byteLength": $($normals.Count * 4) },
    { "buffer": 0, "byteOffset": $($vertices.Count * 4 + $uvs.Count * 4 + $normals.Count * 4), "byteLength": $($tangents.Count * 4) },
    { "buffer": 0, "byteOffset": $($vertices.Count * 4 + $uvs.Count * 4 + $normals.Count * 4 + $tangents.Count * 4), "byteLength": $($indices.Count * 2) }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3" },
    { "bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC3" },
    { "bufferView": 3, "componentType": 5126, "count": 24, "type": "VEC4" },
    { "bufferView": 4, "componentType": 5123, "count": 36, "type": "SCALAR" }
  ],
  "meshes": [{ "name": "Flat-shaded material cube", "primitives": [{ "attributes": { "POSITION": 0, "TEXCOORD_0": 1, "NORMAL": 2, "TANGENT": 3 }, "indices": 4, "mode": 4 }] }]
}
"@
[IO.File]::WriteAllText($gltfPath, $gltf, [Text.UTF8Encoding]::new($false))
