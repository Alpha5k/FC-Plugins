param(
    [Parameter(Mandatory = $true)]
    [string] $OutputDir,
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 9)]
    [int] $ShellSlot,
    [Parameter(Mandatory = $true)]
    [ValidateRange(0, 9)]
    [int] $GameSlot,
    [string] $ModToolsDir = 'C:\BF2_ModTools'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Mod Tools sometimes report compilation errors with a successful exit code.
function Invoke-MungeTool {
    param([string] $ToolPath, [string[]] $Arguments)
    $messages = & $ToolPath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $messages | Write-Output
    if ($exitCode -ne 0 -or $messages -match '^\s*(?:\*+\s*)?ERROR\b') {
        throw "Munge tool failed: $ToolPath (exit code $exitCode)"
    }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
if ($outputRoot.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputRoot.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Munge output must be outside the fc-plugins source tree.'
}

$scriptMunge = Join-Path $ModToolsDir 'ToolsFL\bin\ScriptMunge.exe'
$levelPack = Join-Path $ModToolsDir 'ToolsFL\bin\LevelPack.exe'
$compiledDir = Join-Path $outputRoot 'munged'
$entryDir = Join-Path $outputRoot 'entry'
$addonDir = Join-Path $outputRoot 'GameData\addon\FCSocial\data\_lvl_pc'
$entryOutput = Join-Path $outputRoot 'GameData\data\_lvl_pc'
foreach ($directory in @($compiledDir, $entryDir, $addonDir, $entryOutput)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

$scripts = @(
    'addons\social\lua\fc_social.lua',
    'addons\social\lua\fc_voice_options.lua',
    'addons\social\lua\fc_voice_lobby.lua',
    'addons\social\lua\fc_buddies.lua'
)
$entries = @("custom_gc_$ShellSlot", "user_script_$GameSlot")
Push-Location $outputRoot
try {
    foreach ($script in $scripts) {
        $source = Join-Path $repoRoot $script
        Invoke-MungeTool $scriptMunge @('-inputfile', [IO.Path]::GetFileName($source), '-platform', 'PC',
            '-sourcedir', [IO.Path]::GetDirectoryName($source), '-outputdir', "$compiledDir\")
    }
    Invoke-MungeTool $levelPack @('-inputfile', 'fc_social.req', '-platform', 'PC', '-sourcedir', $PSScriptRoot,
        '-inputdir', $compiledDir, '-outputdir', "$addonDir\")

    # Each entry's script resource must have the same name as its numbered LVL.
    foreach ($entry in $entries) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'entry.lua') -Destination (Join-Path $entryDir "$entry.lua") -Force
        $request = "ucft`n{`n    REQN`n    {`n        `"script`"`n        `"$entry`"`n    }`n}`n"
        [IO.File]::WriteAllText((Join-Path $entryDir "$entry.req"), $request, [Text.UTF8Encoding]::new($false))
        Invoke-MungeTool $scriptMunge @('-inputfile', "$entry.lua", '-platform', 'PC', '-sourcedir', $entryDir,
            '-outputdir', "$compiledDir\")
        Invoke-MungeTool $levelPack @('-inputfile', "$entry.req", '-platform', 'PC', '-sourcedir', $entryDir,
            '-inputdir', $compiledDir, '-outputdir', "$entryOutput\")
    }
}
finally {
    Pop-Location
}

foreach ($artifact in @((Join-Path $addonDir 'fc_social.lvl')) + @($entries | ForEach-Object { Join-Path $entryOutput "$_.lvl" })) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "Munge did not produce the expected LVL: $artifact"
    }
    Write-Output $artifact
}
