param(
    [string]$Executable = (Join-Path (Split-Path -Parent $PSScriptRoot) 'emergency_hospital.exe')
)

if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Application executable not found: $Executable"
}

$dataDirectory = Join-Path (Get-Location) 'data'
$databaseFiles = @('hospital.db', 'hospital.db-journal', 'hospital.db-wal', 'hospital.db-shm') |
    ForEach-Object { Join-Path (Get-Location) $_ }
if (($databaseFiles | Where-Object { Test-Path -LiteralPath $_ }).Count -gt 0 -or
    (Test-Path -LiteralPath (Join-Path $dataDirectory 'patients.txt')) -or
    (Test-Path -LiteralPath (Join-Path $dataDirectory 'doctors.txt')) -or
    (Test-Path -LiteralPath (Join-Path $dataDirectory 'treatments.txt')) -or
    (Test-Path -LiteralPath (Join-Path $dataDirectory 'users.txt'))) {
    throw 'Run this smoke test from a fresh, isolated working directory without existing data.'
}

$isolatedProject = Join-Path (Get-Location) ('smoke_project_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $isolatedProject 'include') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $isolatedProject 'data') -Force | Out-Null
Set-Content -LiteralPath (Join-Path $isolatedProject 'CMakeLists.txt') -Value '# isolated smoke fixture'
Set-Content -LiteralPath (Join-Path $isolatedProject 'include\DatabaseManager.h') -Value '// isolated smoke fixture'
$isolatedExecutable = Join-Path $isolatedProject 'emergency_hospital.exe'
Copy-Item -LiteralPath $Executable -Destination $isolatedExecutable

try {
    # Invalid input plus patient, queue, doctor, and treatment operations.
    $workflow = @(
        'invalid',
        '1', '111', 'Ada', '30', 'Checkup', '1',
        '2', '112', 'Ben', '40', 'Injury', '5', '1',
        '3', '113', 'Cal', '20', 'Cold', '2',
        '4', '5', '6', '111',
        '7', '777', 'Dr Demo', 'General Physician',
        '8', '9', '10', '112', '777', '11', '112',
        '12', '112', '777', 'DX-ALPHA', 'TX-BETA',
        '13', '14', '15'
    ) -join "`n"

    $output = $workflow | & $isolatedExecutable --initialize-empty 2>&1
    $exitCode = $LASTEXITCODE
    $text = $output -join "`n"
    if ($exitCode -ne 0) { throw "Menu workflow exited with code $exitCode.`n$text" }
    if (-not $text.Contains('Initialized an empty SQLite database by explicit request.')) {
        throw "Explicit new-install initialization did not run.`n$text"
    }
    foreach ($expected in @(
        'Invalid number. Please try again.',
        'Patient registered in WAITING status.',
        'Doctor assigned; patient is ASSIGNED.',
        'Treatment completed.',
        'Data saved. Exiting.',
        'Diagnosis: DX-ALPHA | Treatment: TX-BETA | Completed',
        'Goodbye.'
    )) {
        if (-not $text.Contains($expected)) { throw "Menu output did not contain '$expected'.`n$text" }
    }
    if ($text.Contains('Operation failed:')) { throw "A menu action failed.`n$text" }

    # A second process registers another patient and exits; a third verifies the committed database state.
    $addWorkflow = @('3', '114', 'Persisted Patient', '40', 'Persistent', '2', '0') -join "`n"
    $addOutput = $addWorkflow | & $isolatedExecutable 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Second-process registration failed.`n$($addOutput -join "`n")" }

    $reloadWorkflow = @('16', '5', '13', '14', '4', '0') -join "`n"
    $reloadOutput = $reloadWorkflow | & $isolatedExecutable 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Reload workflow failed.`n$($reloadOutput -join "`n")" }
    $reloadText = $reloadOutput -join "`n"
    if (-not $reloadText.Contains('Data loaded.') -or -not $reloadText.Contains('Goodbye.')) {
        throw "Load or exit option 0 did not finish cleanly.`n$reloadText"
    }
    if (-not $reloadText.Contains('Persisted Patient')) {
        throw "The second process's patient was not persisted in SQLite.`n$reloadText"
    }
    if ($reloadText.Contains('Operation failed:')) { throw "A reload menu action failed.`n$reloadText" }

    Write-Output 'Menu and SQLite restart smoke checks passed.'
} finally {
    foreach ($fileName in @('hospital.db', 'hospital.db-journal', 'hospital.db-wal', 'hospital.db-shm')) {
        Remove-Item -LiteralPath (Join-Path (Get-Location) $fileName) -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $isolatedProject -Recurse -Force -ErrorAction SilentlyContinue
}
