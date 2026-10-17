param([Parameter(Mandatory = $true)][string]$Executable)

$ErrorActionPreference = 'Stop'
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$fixturePath = Join-Path ([System.IO.Path]::GetTempPath()) ("lbp3-elf-" + [guid]::NewGuid() + '.bin')
$moduleDirectory = $fixturePath + '.modules'

function Put16([byte[]]$Data, [int]$Offset, [uint16]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Data, $Offset)
}
function Put32([byte[]]$Data, [int]$Offset, [uint32]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Data, $Offset)
}
function Put64([byte[]]$Data, [int]$Offset, [uint64]$Value) {
    [BitConverter]::GetBytes($Value).CopyTo($Data, $Offset)
}

function Check([string]$Name, [byte[]]$Data, [int]$ExpectedExit, [string]$ExpectedText, [string]$Mode = '') {
    [System.IO.File]::WriteAllBytes($fixturePath, $Data)
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.Arguments = $Mode + ' "' + $fixturePath + '"'
    if ($Mode -eq '--map-set') {
        [void][System.IO.Directory]::CreateDirectory($moduleDirectory)
        [System.IO.File]::WriteAllBytes((Join-Path $moduleDirectory 'a.elf'), $Data)
        [System.IO.File]::WriteAllBytes((Join-Path $moduleDirectory 'b.elf'), $Data)
        $start.Arguments = '--map-set "' + $moduleDirectory + '"'
    }
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(10000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "${Name}: inspector timed out"
        }
        $text = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne $ExpectedExit -or $text -notmatch $ExpectedText) {
            throw "${Name}: exit=$($process.ExitCode), expected=$ExpectedExit; output=$text"
        }
        Write-Output "PASS: $Name"
    } finally {
        $process.Dispose()
    }
}

try {
    if (-not [BitConverter]::IsLittleEndian) { throw 'Fixtures require a little-endian host.' }
    $valid = New-Object byte[] 256
    $valid[0] = 0x7f; $valid[1] = 0x45; $valid[2] = 0x4c; $valid[3] = 0x46
    $valid[4] = 2; $valid[5] = 1; $valid[6] = 1
    Put16 $valid 16 2
    Put16 $valid 18 62
    Put32 $valid 20 1
    Put64 $valid 24 0x400000
    Put64 $valid 32 64
    Put16 $valid 52 64
    Put16 $valid 54 56
    Put16 $valid 56 2
    Put32 $valid 64 1
    Put32 $valid 68 5
    Put64 $valid 80 0x400000
    Put64 $valid 96 256
    Put64 $valid 104 512
    Put64 $valid 112 4096
    # Unknown platform headers must retain their numeric type in the report.
    Put32 $valid 120 0x61000000
    Check 'ELF64 and RX segment' $valid 0 'PT_LOAD.*R-X'
    Check 'unknown program type' $valid 0 'OS/PROC/UNKNOWN \(0x61000000\)'
    Check 'entry point' $valid 0 'Entry: 0x400000'

    $mapped = $valid.Clone(); Put16 $mapped 16 0xfe10
    Check 'map relocatable RX and zero BSS' $mapped 0 'copied=256 BSS=256 bytes; verified' '--map'
    $data = $mapped.Clone(); Put64 $data 24 0x400100
    Check 'reject entry in BSS' $data 1 'Entry is outside' '--map'
    $data = $mapped.Clone(); Put32 $data 68 7
    Check 'reject RWX' $data 1 'Unsupported PT_LOAD permissions' '--map'
    $data = $mapped.Clone(); Put32 $data 120 1
    Put32 $data 124 6; Put64 $data 136 0x400000
    Put64 $data 160 128; Put64 $data 168 4096
    Check 'reject overlapping mappings' $data 1 'overlap or share host pages' '--map'
    $data = $mapped.Clone(); Put64 $data 104 0x40001000
    Check 'reject excessive reservation' $data 1 'mapping budget' '--map'
    $shared = $mapped.Clone(); Put16 $shared 16 0xfe18; Put64 $shared 24 0
    Check 'PRX rejected as main executable' $shared 1 'Unsupported ELF type'
    Check 'two shared modules simultaneously, no executable entry required' $shared 0 '2 images resident simultaneously' '--map-set'
    $data = $mapped.Clone(); Put64 $data 24 0
    Check 'main entry still validated in module set' $data 1 'Entry is outside' '--map-set'

    foreach ($type in @(3, 0xfe00, 0xfe10)) {
        $data = $valid.Clone(); Put16 $data 16 $type
        Check "accepted executable type $type" $data 0 'Program headers: 2'
    }
    $data = $valid.Clone(); $data[0] = 0
    Check 'bad magic' $data 1 'Invalid ELF magic'
    $data = $valid.Clone(); Put32 $data 0 0x1d3d154f
    Check 'SELF container' $data 1 'SELF container detected'
    $data = $valid.Clone(); $data[4] = 1
    Check 'ELF32' $data 1 'ELFCLASS64'
    $data = $valid.Clone(); $data[5] = 2
    Check 'big endian' $data 1 'ELFDATA2LSB'
    $data = $valid.Clone(); Put16 $data 18 183
    Check 'non x86-64' $data 1 'EM_X86_64'
    $data = $valid.Clone(); Put32 $data 20 0
    Check 'bad version' $data 1 'Unsupported ELF version'
    $data = $valid.Clone(); Put16 $data 16 1
    Check 'relocatable object' $data 1 'Unsupported ELF type'
    $data = $valid.Clone(); Put16 $data 52 63
    Check 'bad ELF header size' $data 1 'e_ehsize'
    $data = $valid.Clone(); Put16 $data 54 55
    Check 'bad program entry size' $data 1 'e_phentsize'
    $data = $valid.Clone(); Put16 $data 56 0
    Check 'missing program headers' $data 1 'no program headers'
    $data = $valid.Clone(); Put16 $data 56 0xffff
    Check 'extended numbering' $data 1 'PN_XNUM'
    $data = $valid.Clone(); Put64 $data 32 ([uint64]::MaxValue)
    Check 'table offset overflow' $data 1 'table is outside'
    $data = $valid.Clone(); Put16 $data 56 4
    Check 'table exceeds EOF' $data 1 'table is outside'
    $data = $valid.Clone(); Put64 $data 96 257
    Check 'segment exceeds EOF' $data 1 'file range exceeds EOF'
    $data = $valid.Clone(); Put64 $data 104 128
    Check 'filesz greater than memsz' $data 1 'p_filesz exceeds p_memsz'
    $data = $valid.Clone(); Put64 $data 80 ([uint64]::MaxValue)
    Check 'virtual address overflow' $data 1 'virtual address range overflows'
    $data = $valid.Clone(); Put64 $data 112 3
    Check 'invalid alignment' $data 1 'power of two'
    $data = $valid.Clone(); Put64 $data 80 0x400001
    Check 'incongruent alignment' $data 1 'not congruent'
    Check 'truncated ELF header' ([byte[]]$valid[0..31]) 1 'Read range exceeds'
    Check 'empty file' ([byte[]]@()) 1 'Read range exceeds'
} finally {
    # Delete only this script's uniquely named temporary file.
    if ([System.IO.File]::Exists($fixturePath)) { [System.IO.File]::Delete($fixturePath) }
    if ([System.IO.Directory]::Exists($moduleDirectory)) {
        [System.IO.File]::Delete((Join-Path $moduleDirectory 'a.elf'))
        [System.IO.File]::Delete((Join-Path $moduleDirectory 'b.elf'))
        [System.IO.Directory]::Delete($moduleDirectory)
    }
}
