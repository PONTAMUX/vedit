# vedit

動画編集に特化した軽量ツール(試作)。音声整え → カット → 映像差し込み → テロップ → トピックバー → BGM を、
`project.json` 1枚で定義して ffmpeg で一発書き出し。人もAIも同じ形式で編集する。

## 必要なもの
- Windows / Python 3.10+
- ffmpeg・ffprobe(PATH、または環境変数 `VEDIT_FFMPEG` / `VEDIT_FFPROBE`)
- `setup.cmd` で Pedalboard(VST3ホスト)を入れる

## 編集画面(GUI)
`vedit-gui.cmd` をダブルクリック(または動画ファイルをドラッグ&ドロップ)。Edge/Chrome のアプリ窓で開く軽量UI。
タイムライン(V2/トピック/テロップ/V1/音声/BGM)、カットツール(C)、リップルトリム、範囲選択、J/K/L シャトル、スタイル編集、音声チェーン、音量メーター、画像編集、書き出しが使える。
- クリップのドラッグ移動、Delete(空白を残す)/Shift+Delete(詰める)、上下まとめての範囲選択、E/Ctrl+K でロック以外の全トラックを分割
- 🔗 リンク(Ctrl+L): オフにすると映像/音声を別々に選択・削除・音声だけ移動
- 音声タブ(Audition 相当): De-click・ローカット・ノイズゲート・コンプ・EQ・リミッター・ラウドネス正規化(-14 LUFS など)、「声のおすすめ設定」
- 右端の L/R メーター(Premiere 風、ピークホールド・クリップランプ)

## 画像編集 vimg(ブイイメージ)
「🎨 画像編集」でテロップ画像・差し込み画像を作る/直す軽量エディタ **vimg**(Photoshop の代わり)が別窓で開く。
動画なしでも使える: `vimg.cmd` をダブルクリック(新規作成 / ファイルを開く)、または画像・`.vimg.json` をドラッグ&ドロップ。
`python vedit_main.py vimg [ファイル|フォルダ]` でも同じ。動画編集画面の「🎨 画像編集」→「画像編集(vimg)を起動」からも開ける。
画像・テキスト(縁取り複数・影・グラデ)・図形のレイヤー、移動/拡大/回転、切り抜き、明るさ等の補正、元に戻す、Photoshop 風ショートカット。
保存すると `*.vimg.json`(編集データ) と透過PNG ができ、動画側に自動で反映される。`python vedit_main.py image x.vimg.json` で PNG だけ書き出せる。

## 使い方
`AGENTS.md` を参照。最短:
```
python vedit_main.py init work\proj.json 素材.mp4
python vedit_main.py silence work\proj.json --apply
python vedit_main.py render work\proj.json --draft
```
AIに頼むときは「`vedit` の `AGENTS.md` を読んで、この動画を〜してください」でよい。サンプル: `examples\sample_project.json`

## 社内配布メモ
- ffmpeg は同梱せず各自導入(同梱するとライセンス表記・ソース提供の義務が発生)。
- 使う VST3 プラグイン、フォント(テロップ既定 HGP創英角ゴシックUB = Office 付属)、BGM素材のライセンスは各自の環境で確認。
- 絶対パスは持たない設計(プロジェクト内は相対パス)。フォルダごと渡せば動く。
