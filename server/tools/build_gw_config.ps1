# Đóng gói phần mềm cấu hình gateway thành 1 file .exe (docs/SERVER.md §14.4)
#   powershell -ExecutionPolicy Bypass -File server\tools\build_gw_config.ps1
# Kết quả: server\tools\dist\GatewayConfig.exe (chép sang máy khác chạy luôn, không cần cài Python)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$venv = Join-Path $here "build\venv"

# venv riêng để không cài PyInstaller vào Python chung của máy
if (-not (Test-Path "$venv\Scripts\python.exe")) { python -m venv $venv }
& "$venv\Scripts\python.exe" -m pip install --quiet --disable-pip-version-check pyinstaller pyserial

& "$venv\Scripts\pyinstaller.exe" --noconfirm --clean --onefile --windowed `
    --name GatewayConfig `
    --icon (Join-Path $here "..\static\favicon.ico") `
    --distpath (Join-Path $here "dist") `
    --workpath (Join-Path $here "build\pyinstaller") `
    --specpath (Join-Path $here "build") `
    (Join-Path $here "gw_config.py")

Write-Host "Xong: $(Join-Path $here 'dist\GatewayConfig.exe')"
