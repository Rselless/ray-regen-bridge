# Compiles the Ray Regen Bridge (FSRD) compute shaders and writes <name>_Shader.h next to them, in the same format
# as OptiScaler/shaders/shader_tools/create_header.py.
#
#   tools\build_shaders.ps1                    all FSRD shaders
#   tools\build_shaders.ps1 fsrd_compose ...   only these

$repo = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $repo 'OptiScaler\shaders\fsrd\precompile'
$dxc = Join-Path $repo 'OptiScaler\shaders\shader_tools\dxc.exe'

$names = if ($args.Count -gt 0) { $args } else {
    Get-ChildItem $dir -Filter '*.hlsl' | ForEach-Object { $_.BaseName }
}

foreach ($name in $names) {
    $hlsl = Join-Path $dir "$name.hlsl"
    $cso = Join-Path $dir "$name.cso"
    & $dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect $hlsl -Fo $cso
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $name"; exit 1 }

    $bytes = [System.IO.File]::ReadAllBytes($cso)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append("#pragma once`n`ninline static const unsigned char ${name}_cso[] = {`n    ")

    for ($i = 0; $i -lt $bytes.Length; $i++) {
        [void]$sb.Append(('0x{0:x2}' -f $bytes[$i]))
        if ($i -lt $bytes.Length - 1) { [void]$sb.Append(', ') }
        if ((($i + 1) % 12) -eq 0) { [void]$sb.Append("`n    ") }
    }

    [void]$sb.Append("`n};`n")
    [System.IO.File]::WriteAllText((Join-Path $dir "${name}_Shader.h"), $sb.ToString().Replace("`n", "`r`n"))
    Remove-Item $cso
    Write-Host "OK $name ($($bytes.Length) bytes)"
}
