@echo off
rem vimg(ブイイメージ)画像編集: ダブルクリックで新規/開く画面、画像や .vimg.json をドラッグ&ドロップでそのファイルを開く
start "" /b pythonw "%~dp0vedit_main.py" vimg %*
