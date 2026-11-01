@echo off
rem Demon's Crest SNES Recomp: uninstall this folder.
rem Asks "Delete save files?" first. No keeps build\saves, build\save-backups
rem and the settings (build\*.ini) in place, so a later install can import them.
setlocal EnableExtensions
if /I "%~1"=="--run" goto run
set "SELF=%TEMP%\demonscrest-uninstall-%RANDOM%.bat"
copy /y "%~f0" "%SELF%" >nul || (echo Could not copy the uninstaller to %%TEMP%%. & pause & exit /b 1)
"%SELF%" --run "%~dp0."
exit /b

:run
set "DIR=%~f2"
if not exist "%DIR%\DemonsCrestSNESRecomp.exe" goto notours
if not exist "%DIR%\CMakeLists.txt" goto notours
echo.
echo Uninstall Demon's Crest SNES Recomp from:
echo   %DIR%
echo.
echo Your saves are in %DIR%\build\saves  (save.srm = in-game save,
echo save1.sav ... save10.sav = save states) and settings in build\*.ini.
echo.
choice /C YN /M "Delete save files"
if errorlevel 2 (set "KEEP=1") else (set "KEEP=0")
if "%KEEP%"=="1" (echo Saves will be KEPT.) else (echo Saves will be DELETED.)
choice /C YN /M "Uninstall now"
if errorlevel 2 (echo Cancelled. & pause & goto selfdel)
cd /d "%TEMP%"
if "%KEEP%"=="0" (
  rmdir /s /q "%DIR%"
) else (
  for /d %%D in ("%DIR%\*") do if /I not "%%~nxD"=="build" rmdir /s /q "%%D"
  for %%F in ("%DIR%\*") do del /f /q "%%F"
  for /d %%D in ("%DIR%\build\*") do if /I not "%%~nxD"=="saves" if /I not "%%~nxD"=="save-backups" rmdir /s /q "%%D"
  for %%F in ("%DIR%\build\*") do if /I not "%%~xF"==".ini" del /f /q "%%F"
  > "%DIR%\build\KEPT_SAVES.txt" echo Saves kept by the uninstaller. A new Demon's Crest SNES Recomp install next to this folder imports them.
  echo Kept: %DIR%\build\saves
)
echo Done.
pause
goto selfdel

:notours
echo This does not look like a Demon's Crest SNES Recomp folder:
echo   %DIR%
echo Nothing was removed.
pause

:selfdel
(goto) 2>nul & del "%~f0"
