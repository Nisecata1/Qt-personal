param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Exit-WithError {
    param(
        [string]$Message,
        [int]$Code = 1
    )
    Write-Host ""
    Write-Host "[ERROR] $Message" -ForegroundColor Red
    exit $Code
}

function Assert-LastExitCode {
    param([string]$StepName)
    if ($LASTEXITCODE -ne 0) {
        Exit-WithError "$StepName failed with exit code $LASTEXITCODE."
    }
}

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

Write-Host "[INFO] Repository root: $repoRoot"
Write-Host "[INFO] Starting Python-side deployment..."

# 1) Check python availability
$pythonCmd = Get-Command python -ErrorAction SilentlyContinue
if (-not $pythonCmd) {
    Exit-WithError "python command not found. Install Python 3.10/3.11 and ensure PATH is configured."
}

& python --version *> $null
if ($LASTEXITCODE -ne 0) {
    Exit-WithError "python exists but is not runnable. Install Python 3.10/3.11 from python.org and re-open terminal."
}

# 2) Create/reuse virtual environment
$venvDir = Join-Path $repoRoot ".venv"
$venvPython = Join-Path $venvDir "Scripts\python.exe"

if (-not (Test-Path $venvPython)) {
    Write-Host "[INFO] Creating virtual environment at .venv ..."
    & python -m venv $venvDir
    Assert-LastExitCode "Create virtual environment"
}
else {
    Write-Host "[INFO] Reusing existing .venv"
}

if (-not (Test-Path $venvPython)) {
    Exit-WithError "Virtual environment python not found at $venvPython"
}

# 3) Upgrade pip toolchain
Write-Host "[INFO] Upgrading pip/setuptools/wheel ..."
& $venvPython -m pip install --upgrade pip setuptools wheel
Assert-LastExitCode "Upgrade pip toolchain"

# 4) Install GPU torch (CUDA wheel index)
$torchSpec = "torch>=2.2,<3"
$torchIndex = "https://download.pytorch.org/whl/cu121"
Write-Host "[INFO] Installing $torchSpec from CUDA index: $torchIndex ..."
& $venvPython -m pip install --index-url $torchIndex $torchSpec
Assert-LastExitCode "Install Torch (CUDA)"

# 5) Install requirements
$requirementsPath = Join-Path $repoRoot "requirements.txt"
if (-not (Test-Path $requirementsPath)) {
    Exit-WithError "requirements.txt not found at repository root."
}

Write-Host "[INFO] Installing dependencies from requirements.txt ..."
& $venvPython -m pip install -r $requirementsPath --extra-index-url $torchIndex
Assert-LastExitCode "Install requirements"

# 6) Validate key files
$mainScript = Join-Path $repoRoot "model\mouse-proxy-yoloai-pc.py"
$engineModel = Join-Path $repoRoot "model\kalabiqiu v8.engine"
$ptModel = Join-Path $repoRoot "model\kalabiqiu v8.pt"

if (-not (Test-Path $mainScript)) {
    Exit-WithError "Main script missing: model/mouse-proxy-yoloai-pc.py"
}

if ((-not (Test-Path $engineModel)) -and (-not (Test-Path $ptModel))) {
    Exit-WithError "Model file missing: require model/kalabiqiu v8.engine or model/kalabiqiu v8.pt"
}

Write-Host "[INFO] Key files check passed."

# 7) Launch main script
Write-Host "[INFO] Launching model/mouse-proxy-yoloai-pc.py ..."
& $venvPython $mainScript
$code = $LASTEXITCODE

if ($code -ne 0) {
    Exit-WithError "Main script exited with code $code." $code
}

Write-Host "[INFO] Main script exited normally."
exit 0
