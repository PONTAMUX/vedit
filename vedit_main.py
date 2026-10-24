import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vedit.cli import main
if __name__ == "__main__":  # 音声処理の並列(別プロセス)で読み直されたときに GUI 等を二重に起動しない
    main()
