@echo off
rem ===================================================================
rem  PresetToolKit launcher - edit the paths below, then double-click this file.
rem  Keep this .bat next to PresetToolKitCLI.exe (it is copied to Release\ on build).
rem  Leave a value empty (set "X=") to skip that option.
rem ===================================================================

rem --- Required ------------------------------------------------------
set "PRESET=%~dp0data\Imogen Boorman.json"
set "HEAD_GLB=C:\path\to\h0_000_pwa_c__basehead.morphtarget.glb"

rem --- Fallout 4 region names ---------------------------------------
rem  Leave empty: HumanRaceFacialBoneRegionsFemale.txt / ...Male.txt in the
rem  data folder is picked automatically from the preset's gender.
set "FO4_REGIONS="

rem --- Optional: keep the .mesh in sync and build the .archive -------
set "HEAD_MESH_GLB="
set "TEMPLATE_MORPHTARGET="
set "TEMPLATE_MESH="
set "WOLVENKIT="

set "OUT=%~dp0PresetToolKit_out"
rem ===================================================================

set "ARGS=--preset "%PRESET%" --head "%HEAD_GLB%" --out "%OUT%""
if defined FO4_REGIONS if exist "%FO4_REGIONS%" set "ARGS=%ARGS% --fo4-regions "%FO4_REGIONS%""
if defined HEAD_MESH_GLB set "ARGS=%ARGS% --head-mesh "%HEAD_MESH_GLB%""
if defined TEMPLATE_MORPHTARGET set "ARGS=%ARGS% --template-morphtarget "%TEMPLATE_MORPHTARGET%""
if defined TEMPLATE_MESH set "ARGS=%ARGS% --template-mesh "%TEMPLATE_MESH%""
if defined WOLVENKIT set "ARGS=%ARGS% --wolvenkit "%WOLVENKIT%""

"%~dp0PresetToolKitCLI.exe" %ARGS%
echo.
echo Finished with exit code %ERRORLEVEL%. Report: "%OUT%\report.txt"
pause
