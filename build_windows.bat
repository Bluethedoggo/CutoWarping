@echo off
REM CutoWarping - local Windows build
REM Created by DogSide Productions & Bluethedoggo
REM
REM Requirements (free):
REM   - Visual Studio 2022 Community with "Desktop development with C++"
REM   - CMake (https://cmake.org/download/) and Git
REM Run this file from the CutoWarping folder (double click is fine).

cmake -B build -A x64 || goto :error
cmake --build build --config Release --target CutoWarping_VST3 CutoWarping_Standalone --parallel || goto :error

echo.
echo ==================================================================
echo  Done!  Your plugin is here:
echo    build\CutoWarping_artefacts\Release\VST3\CutoWarping.vst3
echo.
echo  Copy the whole "CutoWarping.vst3" folder to:
echo    C:\Program Files\Common Files\VST3\
echo  Then in FL Studio: Options ^> Manage plugins ^> Find installed plugins.
echo ==================================================================
pause
exit /b 0

:error
echo.
echo Build failed. Check the messages above.
pause
exit /b 1
