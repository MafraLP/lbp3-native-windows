# Compile and test locally, recording any failure for the next investigation.
$ErrorActionPreference = 'Continue'
$projectRoot = Split-Path $PSScriptRoot -Parent
Push-Location -LiteralPath $projectRoot
try {
    Import-Module 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll' -ErrorAction Stop
    Enter-VsDevShell -VsInstallPath 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools' -SkipAutomaticLocation -DevCmdArguments '-arch=amd64 -host_arch=amd64' -ErrorAction Stop
    foreach ($stage in @('build', 'test')) {
        if ($stage -eq 'build') {
            $result = & cmake --build --preset win-clang-debug 2>&1
        } else {
            $result = & ctest --preset win-clang-debug --output-on-failure 2>&1
        }
        $stageExit = $LASTEXITCODE
        $result | ForEach-Object { Write-Output $_ }
        if ($stageExit -ne 0) {
            $lines = @('', "## Validation failure $(Get-Date -Format o)", '', "Stage: $stage; exit code: $stageExit", '', '```text')
            $lines += $result | ForEach-Object { $_.ToString() }
            $lines += @('```', '', 'Status: investigate before continuing dependent execution.')
            Add-Content -LiteralPath 'analysis/IMPLEMENTATION_LOG.md' -Value $lines -Encoding UTF8
            exit $stageExit
        }
    }
} catch {
    Add-Content -LiteralPath 'analysis/IMPLEMENTATION_LOG.md' -Encoding UTF8 -Value @('', "## Validation setup failure $(Get-Date -Format o)", '', $_.ToString())
    Write-Error $_
    exit 1
} finally {
    Pop-Location
}
