@echo off
setlocal

echo ========================================
echo GameCube ISO Extractor for GCRecompiler
echo ========================================

set /p ISO_PATH="Enter the path to your GameCube ISO: "

if not exist "%ISO_PATH%" (
    echo [ERROR] ISO file not found: %ISO_PATH%
    pause
    exit /b 1
)

if not exist "tools\DolphinTool.exe" (
    echo [ERROR] DolphinTool.exe not found in tools folder.
    pause
    exit /b 1
)

if not exist "iso" (
    mkdir "iso"
)

echo [INFO] Extracting ISO... This may take a minute...
"tools\DolphinTool.exe" extract -i "%ISO_PATH%" -o "iso"

if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] ISO extracted to the 'iso' folder.
    echo [INFO] You can now run gcrecomp.exe on your dol file located in the 'iso' folder.
) else (
    echo [ERROR] Extraction failed.
)

pause
