param(
    [Parameter(Mandatory = $true)]
    [string] $OutputDir,
    [Parameter(Mandatory = $true)]
    [ValidateScript({ $_ -ge 0 })]
    [int] $EntrySlot,
    [string] $ModToolsDir = 'C:\BF2_ModTools'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Run a Mod Tools program and reject process failures or reported compilation errors.
function Invoke-MungeTool {
    param([string] $ToolPath, [string[]] $Arguments)
    $toolOutput = & $ToolPath @Arguments 2>&1
    $toolExit = $LASTEXITCODE
    $toolOutput | Write-Output
    if ($toolExit -ne 0) {
        throw "Munge tool failed with exit code ${toolExit}: $ToolPath"
    }

    # Mod Tools can report compilation errors while returning a successful exit code.
    if ($toolOutput -match '^\s*(?:\*+\s*)?ERROR\b') {
        throw "Munge tool reported an error: $ToolPath"
    }
}

if ($EntrySlot -eq 10) {
    throw 'Entry slot 10 belongs to the prerequisite loader. Choose a free reachable slot.'
}

$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot '..\..'))
$outputRoot = [IO.Path]::GetFullPath($OutputDir)
if ($outputRoot.Equals($repoRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $outputRoot.StartsWith($repoRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Munge output must be outside the fc-plugins source tree.'
}

$scriptMunge = Join-Path $ModToolsDir 'ToolsFL\bin\ScriptMunge.exe'
$configMunge = Join-Path $ModToolsDir 'ToolsFL\bin\ConfigMunge.exe'
$levelPack = Join-Path $ModToolsDir 'ToolsFL\bin\LevelPack.exe'
foreach ($tool in @($scriptMunge, $configMunge, $levelPack)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Mod Tools program is missing: $tool"
    }
}

$compiledDir = Join-Path $outputRoot 'munged'
$entryDir = Join-Path $outputRoot 'entry'
$addonDir = Join-Path $outputRoot 'GameData\addon\MPGC\data\_lvl_pc'
$entryOutput = Join-Path $outputRoot 'GameData\data\_lvl_pc'
foreach ($directory in @($outputRoot, $compiledDir, $entryDir, $addonDir, $entryOutput)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

# Both the numbered script resource and LVL must match the chosen loader slot.
$entryName = "custom_gc_$EntrySlot"
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'custom_gc_entry.lua') -Destination (Join-Path $entryDir "$entryName.lua") -Force
$entryReq = "ucft`n{`n    REQN`n    {`n        `"script`"`n        `"$entryName`"`n    }`n}`n"
[IO.File]::WriteAllText((Join-Path $entryDir "$entryName.req"), $entryReq, [Text.UTF8Encoding]::new($false))

Push-Location $outputRoot
try {
    foreach ($script in @('ifs_freeform_net_shell.lua', 'ifs_freeform_net_menus.lua', 'ifs_freeform_net_browser.lua', 'ifs_freeform_net_campaign.lua', 'ifs_freeform_net_screens.lua', 'ifs_freeform_net_controls.lua', 'ifs_freeform_net_presentation.lua', 'ifs_freeform_net_battle.lua', 'ifs_freeform_net_inventory.lua', 'ifs_freeform_net_actions.lua', 'ifs_freeform_net_decisions.lua', 'ifs_freeform_net_chat.lua')) {
        Invoke-MungeTool $scriptMunge @('-inputfile', $script, '-platform', 'PC', '-sourcedir', (Join-Path $sourceRoot 'lua'), '-outputdir', "$compiledDir\")
    }
    Invoke-MungeTool $scriptMunge @('-inputfile', "$entryName.lua", '-platform', 'PC', '-sourcedir', $entryDir, '-outputdir', "$compiledDir\")
    Invoke-MungeTool $configMunge @('-inputfile', 'mpgc_chat.hud', '-platform', 'PC', '-sourcedir', (Join-Path $sourceRoot 'hud'), '-outputdir', "$compiledDir\")
    Invoke-MungeTool $levelPack @('-inputfile', 'mpgc_chat.req', '-platform', 'PC', '-sourcedir', $PSScriptRoot, '-inputdir', $compiledDir, '-outputdir', "$addonDir\")
    Invoke-MungeTool $levelPack @('-inputfile', 'mpgc_shell.req', '-platform', 'PC', '-sourcedir', $PSScriptRoot, '-inputdir', $compiledDir, '-outputdir', "$addonDir\")
    Invoke-MungeTool $levelPack @('-inputfile', "$entryName.req", '-platform', 'PC', '-sourcedir', $entryDir, '-inputdir', $compiledDir, '-outputdir', "$entryOutput\")
}
finally {
    Pop-Location
}

foreach ($artifact in @((Join-Path $addonDir 'mpgc_shell.lvl'), (Join-Path $addonDir 'mpgc_chat.lvl'), (Join-Path $entryOutput "$entryName.lvl"))) {
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "Munge did not produce the expected LVL: $artifact"
    }
    Write-Output $artifact
}
