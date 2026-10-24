@echo off
rem 初回セットアップ: Python 3.10+ と ffmpeg が必要
python -m pip install -r "%~dp0requirements.txt"
where ffmpeg >nul 2>nul || echo [警告] ffmpeg が見つかりません。PATH に追加するか VEDIT_FFMPEG / VEDIT_FFPROBE を設定してください
