@echo off
rem 動画ファイルをこのファイルにドラッグ&ドロップ、または ダブルクリックで動画を選んで編集画面を開く
start "" /b pythonw "%~dp0vedit_main.py" gui %*
