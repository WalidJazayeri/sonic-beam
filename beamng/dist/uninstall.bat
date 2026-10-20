@echo off
rem Removes Sonic Beam and puts back the files the installer replaced.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" -Uninstall
pause
