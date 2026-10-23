@echo off
setlocal EnableExtensions EnableDelayedExpansion

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

findstr /I /C:"Connection: keep-alive" "%TEMP_DIR%\get-headers.txt" >nul || (
    echo TEST FAILED: keep-alive response header was missing.
    goto :fail
)
findstr /I /C:"Keep-Alive:" "%TEMP_DIR%\get-headers.txt" >nul || (
    echo TEST FAILED: Keep-Alive response header was missing.
    goto :fail
)

call :check_status "%BASE_URL%/missing-file.fakaapache" 404 missing
if errorlevel 1 goto :fail

curl.exe -sS --path-as-is -D "%TEMP_DIR%\traversal-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/%%2e%%2e/siteconfig.fakaapache" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="403" (
    echo TEST FAILED: traversal request returned HTTP %STATUS% instead of 403.
    goto :fail
)

curl.exe -sS --path-as-is -D "%TEMP_DIR%\malformed-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/%%zz" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="400" (
    echo TEST FAILED: malformed URL returned HTTP %STATUS% instead of 400.
    goto :fail
)

curl.exe -sS -X OPTIONS -D "%TEMP_DIR%\options-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="405" (
    echo TEST FAILED: OPTIONS returned HTTP %STATUS% instead of 405.
    goto :fail
)
findstr /I /C:"Allow: GET, HEAD, POST" "%TEMP_DIR%\options-headers.txt" >nul || (
    echo TEST FAILED: unsupported-method response had the wrong Allow header.
    goto :fail
)

curl.exe -sS -X POST -D "%TEMP_DIR%\post-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="405" (
    echo TEST FAILED: static POST returned HTTP %STATUS% instead of 405.
    goto :fail
)

curl.exe -sS -H "Range: bytes=999999999-" -D "%TEMP_DIR%\bad-range-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
if errorlevel 1 goto :curl_fail
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="416" (
    echo TEST FAILED: unsatisfiable range returned HTTP %STATUS% instead of 416.
    goto :fail
)
findstr /I /C:"Content-Range: bytes */" "%TEMP_DIR%\bad-range-headers.txt" >nul || (
    echo TEST FAILED: 416 response did not include Content-Range.
    goto :fail
)

curl.exe -sS -H "Accept-Encoding: gzip;q=0" -D "%TEMP_DIR%\no-gzip-headers.txt" -o nul "%BASE_URL%/" >nul
if errorlevel 1 goto :curl_fail
findstr /I /C:"Content-Encoding: gzip" "%TEMP_DIR%\no-gzip-headers.txt" >nul && (
    echo TEST FAILED: gzip was used despite gzip;q=0.
    goto :fail
)

if defined FAKAAPACHE_AUTH_USER if defined FAKAAPACHE_AUTH_PASSWORD (
    curl.exe -sS -D "%TEMP_DIR%\auth-fail-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
    if errorlevel 1 goto :curl_fail
    set /p STATUS=<"%TEMP_DIR%\status.txt"
    if not "!STATUS!"=="401" (
        echo TEST FAILED: unauthenticated request returned HTTP %STATUS% instead of 401.
        goto :fail
    )
    curl.exe -sS -u "%FAKAAPACHE_AUTH_USER%:%FAKAAPACHE_AUTH_PASSWORD%" -D "%TEMP_DIR%\auth-ok-headers.txt" -o nul -w "%%{http_code}" "%BASE_URL%/" > "%TEMP_DIR%\status.txt"
    if errorlevel 1 goto :curl_fail
    set /p STATUS=<"%TEMP_DIR%\status.txt"
    if not "!STATUS!"=="200" (
        echo TEST FAILED: authenticated request returned HTTP %STATUS% instead of 200.
        goto :fail
    )
) else (
    echo Skipping Basic Authentication checks; set FAKAAPACHE_AUTH_USER and FAKAAPACHE_AUTH_PASSWORD to enable them.
)

echo Command Prompt integration tests passed.
rmdir /s /q "%TEMP_DIR%" >nul 2>&1
exit /b 0

:curl_fail
echo TEST FAILED: curl.exe could not connect to %BASE_URL%.
goto :fail

:check_status
curl.exe -sS -D "%TEMP_DIR%\%~3-headers.txt" -o nul -w "%%{http_code}" "%~1" > "%TEMP_DIR%\status.txt"
if errorlevel 1 exit /b 1
set /p STATUS=<"%TEMP_DIR%\status.txt"
if not "%STATUS%"=="%~2" (
    echo TEST FAILED: %~3 returned HTTP %STATUS% instead of %~2.
    exit /b 1
)
exit /b 0

:fail
echo Logs and response files were retained at %TEMP_DIR%.
exit /b 1
