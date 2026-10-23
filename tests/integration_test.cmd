@echo off
setlocal EnableExtensions

rem Command Prompt smoke tests. Start FakaApache separately before running this file.
set "BASE_URL=%~1"
if not defined BASE_URL set "BASE_URL=http://127.0.0.1:8080"
set "TEMP_DIR=%TEMP%\fakaapache-cmd-tests-%RANDOM%"
if exist "%TEMP_DIR%" rmdir /s /q "%TEMP_DIR%"
mkdir "%TEMP_DIR%" >nul 2>&1

where curl.exe >nul 2>&1
if errorlevel 1 (
    echo TEST FAILED: curl.exe was not found in PATH.
    goto :fail
)

echo Testing %BASE_URL%

curl.exe -sS -D "%TEMP_DIR%\get-headers.txt" -o "%TEMP_DIR%\get-body.txt" -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="200" (
    echo TEST FAILED: GET returned HTTP %STATUS%.
    goto :fail
)
findstr /C:"FakaApache Example Site" "%TEMP_DIR%\get-body.txt" >nul || (
    echo TEST FAILED: GET returned the wrong page.
    goto :fail
)

curl.exe -sS --head -D "%TEMP_DIR%\head-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="200" (
    echo TEST FAILED: HEAD returned HTTP %STATUS%.
    goto :fail
)
curl.exe -sS -H "Range: bytes=0-9" -D "%TEMP_DIR%\range-headers.txt" -o "%TEMP_DIR%\range-body.txt" -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="206" (
    echo TEST FAILED: Range returned HTTP %STATUS%.
    goto :fail
)
findstr /I /C:"Content-Range: bytes 0-9/" "%TEMP_DIR%\range-headers.txt" >nul || (
    echo TEST FAILED: Range response did not include Content-Range.
    goto :fail
)
for %%F in ("%TEMP_DIR%\range-body.txt") do if %%~zF neq 10 (
    echo TEST FAILED: Range response was not 10 bytes.
    goto :fail
)

curl.exe -sS -H "Accept-Encoding: gzip" -D "%TEMP_DIR%\gzip-headers.txt" -o "%TEMP_DIR%\gzip-body.bin" "%BASE_URL%/" >nul
if errorlevel 1 goto :curl_fail
findstr /I /C:"Content-Encoding: gzip" "%TEMP_DIR%\gzip-headers.txt" >nul || (
    echo TEST FAILED: gzip negotiation did not return Content-Encoding: gzip.
    goto :fail
)

echo Command Prompt integration tests passed.
rmdir /s /q "%TEMP_DIR%" >nul 2>&1
exit /b 0

:curl_fail
echo TEST FAILED: curl.exe could not connect to %BASE_URL%.
goto :fail

:fail
echo Logs and response files were retained at %TEMP_DIR%.
exit /b 1
