param([string]$ProjectRoot = $PSScriptRoot, [switch]$Force)

$ErrorActionPreference = 'Stop'
$assetRoot = Join-Path $ProjectRoot 'Assets'

function Write-CubeMesh
{
    $path = Join-Path $assetRoot 'Models/UnitCube.gltf'
    if ((Test-Path -LiteralPath $path) -and -not $Force) { return }
    $positions = [Collections.Generic.List[float]]::new()
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
        $base = [uint16]($positions.Count / 3)
        foreach ($corner in 0..3)
        {
            foreach ($value in $face.P[$corner]) { $positions.Add([float]$value) }
            $uv = switch ($corner) { 0 {@(0,0)} 1 {@(1,0)} 2 {@(1,1)} 3 {@(0,1)} }
            $uvs.Add([float]$uv[0]); $uvs.Add([float]$uv[1])
            foreach ($value in $face.N) { $normals.Add([float]$value) }
            foreach ($value in $face.T) { $tangents.Add([float]$value) }
            $tangents.Add([float]1)
        }
        foreach ($index in @($base, ($base + 1), ($base + 2), $base, ($base + 2), ($base + 3)))
            { $indices.Add([uint16]$index) }
    }
    $buffer = [Collections.Generic.List[byte]]::new()
    foreach ($value in $positions) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
    foreach ($value in $uvs) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
    foreach ($value in $normals) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
    foreach ($value in $tangents) { $buffer.AddRange([BitConverter]::GetBytes([float]$value)) }
    foreach ($value in $indices) { $buffer.AddRange([BitConverter]::GetBytes([uint16]$value)) }
    $positionsBytes = $positions.Count * 4
    $uvBytes = $uvs.Count * 4
    $normalBytes = $normals.Count * 4
    $tangentBytes = $tangents.Count * 4
    $base64 = [Convert]::ToBase64String($buffer.ToArray())
    $gltf = @"
{
  "asset": { "version": "2.0", "generator": "PulseForge procedural transparency validation asset" },
  "buffers": [{ "byteLength": $($buffer.Count), "uri": "data:application/octet-stream;base64,$base64" }],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": $positionsBytes },
    { "buffer": 0, "byteOffset": $positionsBytes, "byteLength": $uvBytes },
    { "buffer": 0, "byteOffset": $($positionsBytes + $uvBytes), "byteLength": $normalBytes },
    { "buffer": 0, "byteOffset": $($positionsBytes + $uvBytes + $normalBytes), "byteLength": $tangentBytes },
    { "buffer": 0, "byteOffset": $($positionsBytes + $uvBytes + $normalBytes + $tangentBytes), "byteLength": $($indices.Count * 2) }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3" },
    { "bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC2" },
    { "bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC3" },
    { "bufferView": 3, "componentType": 5126, "count": 24, "type": "VEC4" },
    { "bufferView": 4, "componentType": 5123, "count": 36, "type": "SCALAR" }
  ],
  "meshes": [{ "name": "Unit cube", "primitives": [{ "attributes": { "POSITION": 0, "TEXCOORD_0": 1, "NORMAL": 2, "TANGENT": 3 }, "indices": 4, "mode": 4 }] }]
}
"@
    [IO.File]::WriteAllText($path, $gltf, [Text.UTF8Encoding]::new($false))
}

function Write-FenceTexture
{
    $path = Join-Path $assetRoot 'Textures/FenceGrid.png'
    if ((Test-Path -LiteralPath $path) -and -not $Force) { return }
    Add-Type -AssemblyName System.Drawing
    $bitmap = [Drawing.Bitmap]::new(128, 128, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try
    {
        for ($y = 0; $y -lt 128; ++$y)
        {
            for ($x = 0; $x -lt 128; ++$x)
            {
                $vertical = (($x + 8) % 32) -lt 5
                $horizontal = (($y + 8) % 32) -lt 5
                $alpha = if ($vertical -or $horizontal) { 255 } else { 0 }
                $bitmap.SetPixel($x, $y, [Drawing.Color]::FromArgb($alpha, 235, 229, 205))
            }
        }
        $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally { $bitmap.Dispose() }
}

Write-CubeMesh
Write-FenceTexture
