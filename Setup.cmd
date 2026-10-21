@echo off
rem Builds Link's Awakening DX for every console from your ROM, with Docker.
rem Safe to rerun: finished work is skipped.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" %*
if not defined RECOMP_LAUNCHER pause
