from .cli import main
if __name__ == "__main__":  # 音声処理の並列(別プロセス)で読み直されたときに二重に起動しない
    main()
