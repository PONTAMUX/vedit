# vedit — AI向け操作ガイド

画面上の名前は **V-edit**(タイトル・ロゴ)。コマンド・パッケージ・フォルダ・設定の置き場所は `vedit` のまま。アイコンは `vedit/web/icon.png`・`icon.ico`(作り直しは `python vedit/web/make_icon.py`)。

動画編集特化ツール。Audition(音声) + Premiere(カット/差し込み/テロップ) + Photoshop(テロップ画像・内蔵の画像編集) の代わり。
**編集内容はすべて `project.json` に書く。書き出しは `vedit render` 1回。** JSONを直接編集してもCLIで編集してもよい。
実行: `python vedit_main.py <コマンド>`(または `vedit.cmd`)。出力はJSON。

## 標準フロー
```
vedit init   work/proj.json 素材.mp4     # プロジェクト作成
vedit audio  work/proj.json              # VST3→ゲイン→ノーマライズ(結果は proj.cache/audio.wav)
vedit silence work/proj.json --apply     # 無音カット候補を cuts に追加(--min 0.7 --keep-before 0.2 --keep-after 0.35)
(cuts / telops / topics / overlays / sfx / bgm を project.json に追記)
vedit validate work/proj.json            # 書き出し前チェック
vedit check work/proj.json [--output 書き出し.mp4]  # 見落としチェック(画像の効果音抜け・カットで消える効果音・同じトラックの重なり・音声処理の古さ。--output で音量/ピーク/長さも)。全編書き出しの後は自動で実行され結果に checks が付く
vedit frame  work/proj.json 83.5         # 仕上がり確認用PNG(カット後の時刻) → 画像を見て確認すること
vedit render work/proj.json --draft      # 低画質で全体確認
vedit render work/proj.json              # 本番(NVENCがあれば自動使用)
```
`render` は音声処理が済んでいなければ自動で行う。結果はキャッシュされ、チェーン/素材が変わらなければ再処理しない。

既定の `audio.chain` は 自前 De-click(リップノイズ除去) → +18dB → ピーク-3dB。De-click は `{"type":"declick","params":{"threshold":8,"widen_ms":0.6,"max_ms":8}}`(thresholdを下げると強くかかる)。外部VST3(RX等)は `{"type":"vst3","path":...}` で差し替え可。

## project.json
| キー | 内容 |
|---|---|
| `source` | 元動画(プロジェクトからの相対 or 絶対パス) |
| `timebase` | `"source"`(既定): telops/topics/overlays の時刻は**元素材の時刻**。カットを足しても自動で追従。`"output"`: カット後の時刻 |
| `audio.chain` | 上から順に実行。`{"type":"vst3","path":"x.vst3","params":{}}` / `{"type":"plugin","name":"NoiseGate","params":{}}`(Pedalboard内蔵) / `{"type":"gain","db":18}` / `{"type":"normalize","peak_db":-3}` または `{"type":"normalize","lufs":-14,"peak_db":-1}`(ラウドネス基準、最後に置く)。GUI では HighpassFilter/NoiseGate/Compressor/Low・High ShelfFilter/PeakFilter/Limiter を選べる |
| `cuts` | 削除する範囲 `[[開始,終了],...]`(秒 or `"1:23.5"`) |
| `gaps` | 空白(黒+無音) `[[元素材の時刻, 長さ秒],...]`。その時刻の素材の直前に挿入(カット内の時刻はカットの終わりへ寄せる)。GUI の Delete(空白を残す)・V1/A1 クリップのドラッグ移動で作られる |
| `gap_at`(telops/topics/overlays/sfx の各要素) | 空白の中に置く素材(章の頭のドドン・章見出しなど)。値は空白の位置(`gaps` の元素材の時刻)で、`start`/`end` は空白の頭からの秒。空白が無くなると出ない。空白の中では gap_at の素材だけが出る(映像は黒)。詰め表示では空白の中に描かれ、ドラッグは空白の中だけで動く。詰め表示で空白の上をクリックすると空白の中で止まる(`gpos`)。重なりの自動整理・分割・Q/W の区切り・スナップの対象外 |
| `vmute` / `amute` | 映像だけ黒 / 音声だけ無音にする範囲 `[[開始,終了],...]`(元素材の時刻)。GUI でリンク解除中に V1 だけ・A1 だけ Delete すると作られる |
| `aslip` | 音声だけずらす `[{"a","b","d"}]`。元素材 a〜b の音声を出力位置 out(a)+d 秒で鳴らす(元の位置と移動先の元音声は無音)。リンク解除中の A1 ドラッグで作られる |
| `telops` | `{"start","end","text","style"?}` 改行は `\n` |
| `topics` | `{"start","end","text"}` 左上のトピックバー(赤い帯+白文字) |
| `overlays` | `{"file","start","end","pos"?,"scale"?,"fit"?,"in"?,"audio"?,"volume_db"?}` 画像/動画の差し込み。`pos`: `full`(全画面・既定。`scale`を付けると小窓) / `center` / `top-left` / `top-right` / `bottom-left` / `bottom-right` / `[x,y]`(1080p基準px)。`fit:"cover"`で全画面を埋める。`in`=差し込み動画の開始位置、`audio:true`で音も入れる |
| `overlays[].track` / `vtracks` | 差し込みを載せるビデオトラック番号(既定 2 = V2)。番号が大きいほど上に重なる(書き出しはトラック番号順→同じトラック内はリスト順)。`vtracks` はトラック名の配列で `vtracks[0]` が V2(例: `["画像","テロップ"]` → V2 画像 / V3 テロップ) |
| `hide` / `mute` | 出さない映像トラック(`v1` `v2`… `tb` `t`)/ 鳴らさない音声トラック(`a1` `se` `bgm`)の配列。GUI の目・M ボタン。**書き出しにも効く**(v1=黒、a1=本編無音) |
| `sfx` | 効果音 `{"file","start","end"?,"gain_db"?,"in"?}`。時刻の基準・カットの扱いは overlays と同じ(timebase に従う。全部カットに入った効果音は鳴らず、カットをまたぐものは残る部分だけ鳴る)。`end` 省略=音の長さ全部(付けるとそこで切る)、`gain_db`=音量(既定0)、`in`=効果音の頭から飛ばす秒。書き出しでは本編(音声処理・ノーマライズ済み)・BGM と最後にミックスする(効果音はノーマライズされない)。例: `{"file":"se/ぽペン.mp3","start":"1:23.5","gain_db":-5}`。CLI: `vedit sfx proj.json ぽペン.mp3,ピロンa.mp3 12.0,30.5,41 --gain -5`(ファイルをカンマ区切りにすると交互に置く。`--end`(開始1つの時だけ)・`--skip`) |
| `bgm` | `{"file","volume_db":-23.5,"duck":false}` 自動ループ・末尾フェード。`duck:true`で声の間だけBGMが下がる |
| `styles` | `telop` / `topic` の上書き。例: `{"telop":{"size":72,"outlines":[{"color":"#0B1F4B","width":9},{"color":"#2E8BFF","width":4}]}}` outlinesは外側→内側の二重縁 |
| `cut_fx` | カット継ぎ目の処理 `{"audio_fade_ms":8,"video":"none|dip-black|dip-white|トランジションの名前","dur":0.12}`(下の「エフェクト」) |
| 各要素の `anim` | telops: `fade|pop|slide-up` + `anim_dur`。`pos:[x,y]`(画面比0〜1)で位置指定。overlays: `in_anim`/`out_anim` = `fade|slide-left|slide-right|slide-up|slide-down` + `anim_dur`。ほかの動き(pop・stamp など)・fx・キーフレームは下の「エフェクト」 |
| `output` | `{"file":"out/final.mp4","fps":null}` |

## エフェクト(動き・合成・トランジション・プリセット)
**UI は無い。JSON か CLI で書く。** GUI のモニター・`vedit frame`・書き出しのどれにも同じ見た目で出る(計算は `fx.py` の1か所)。
使っていない project は今までと同じグラフで書き出す(エフェクトを書いた要素だけが新しい経路を通る)。乱数は `seed` で固定なので、何度書き出しても同じ絵になる。
動くものは1コマずつ描いて `proj.cache/fx/`(透明つき .mov / 止まっている所は .png)に焼き、ffmpeg で重ねる。同じ指定なら焼き直さない(消してもよい。次の書き出しで作り直す)。
`vedit render --draft` は小さく描いて速くする(見た目の確認用)。

```
vedit fx list [--md]                            # 全部の名前・説明・パラメータの既定(JSON。--md で下の表)。プリセットも出る
vedit fx preview shake -o pv.png                # 見本の映像にかけたコマを並べた画像(--params '{"amp":[20,10]}' --frames 10)
vedit fx preview text:pop / preset:impact       # 同じ名前が複数の種類にある時は 種類:名前
vedit expand proj.vedit.json [-o x.json]        # プリセットを展開した JSON(書き出しで使う形)を見る。元のファイルは変えない
vedit validate proj.vedit.json                  # 知らない名前・パラメータの型・キーフレームの順・親子の輪などを分かる言葉で出す
```

### どこに書くか
| 書く所 | 内容 |
|---|---|
| `overlays[].in_anim` / `out_anim` | 出入り。文字列(`"pop"`)か `{"type":"pop","dur":0.4,"seed":3,...}`。今までの `fade`/`slide-*` + `anim_dur` はそのまま(従来の経路) |
| `overlays[].fx` / `telops[].fx` | エフェクトの並び `[{"type":"glow","radius":30,"start":0.2,"end":1.0}, ...]`。`start`/`end` は要素の頭からの秒(省略=全体)。上から順にかかる |
| `overlays[]` の `kind` | `file` の代わりに生成要素: `{"kind":"speedlines","start":..,"end":..,"track":5,"params":{"count":150}}`。ほかに `adjust`(調整レイヤー)・`comp`(プリコンポーズ) |
| `v1_fx` | 背景(V1 の本編)にかける: `[{"type":"shake","start":184.5}, {"type":"mosaic","start":10,"end":14,"rect":[100,80,300,60]}]`。`end` は shake 以外は必須。zoom/drift は使えない |
| `transitions` | `{"gap":空白の時刻,"type":"wipe-diag","sides":"both|in|out"}`(空白の前後)か `{"at":継ぎ目の時刻,"type":"crossfade"}`。`dur` / `params` / `se:{"file","gain_db"}`(継ぎ目で鳴らす音) |
| `cut_fx.video` | 全部のカットの継ぎ目に同じトランジション(`"crossfade"` や `{"type":"whip","dur":0.2}`)。`transitions` に書いた継ぎ目はそちらが優先。`dip-black`/`dip-white` は従来どおり |
| `telops[].animator` | 文字アニメーター(1文字ずつ): `{"type":"pop","stagger":0.05}` か並び `[{"type":"rise"},{"type":"wave"}]` |
| `overlays[]` / `telops[]` の `preset` | プリセット(下)。`{"preset":"impact","text":"0LP","start":4.37,"end":6.36}` |

### 共通のキー(overlays・telops・comp の layers)
| キー | 内容 |
|---|---|
| `keys` | キーフレーム/式。対象: `x` `y`(1080p 基準 px のずれ) `scale` `scale_x` `scale_y` `rot`(度) `opacity` `anchor_x` `anchor_y`(拡大・回転の中心。中身の外枠に対する比、既定 0.5)、`"fx[0].radius"`(fx の i 番目のパラメータ)、`"glow.radius"`(type 名.パラメータ) |
| キーフレーム | `{"keys":[[秒, 値, "ease"?], ...], "loop":"cycle|pingpong"?}`。秒は要素の頭から(fx のパラメータは fx の頭から)。値は数・[数,...]・色。ease は次のキーへの補間: `linear` `smoothstep` `ease-in` `ease-out` `ease-in-out` `back` `elastic` `bounce` `hold` `cubic-bezier(a,b,c,d)` |
| 式 | `"=wiggle(2, 60)"` か `{"expr":"..."}`。変数 `t`(要素の頭からの秒) `dur` `value`(式を書いたキーの元の値) `fps` `frame` `pi` `e`。関数 `sin cos tan abs floor ceil round sqrt exp log pow min max clamp lerp step smooth`、`wiggle(毎秒の回数, 幅, octaves?, 減衰?)` `noise(x)` `loop(周期)` `rand(a?, b?)`(seed で固定)。fx のパラメータにも書ける(`"amount":"=0.5+0.5*sin(t*6)"`) |
| `mask` | `{"shape":"rect|ellipse|polygon","rect":[x,y,w,h]|"points":[[x,y],...],"feather":px,"invert":false,"mode":"add|subtract|intersect"}` かその並び(上から順に合成)。1080p 基準 px。rect/points はキーフレーム・式で動かせる |
| `matte` | 別の要素を型にして抜く。`"luma"` のようにモードだけ書くと**同じリストの次の要素**を型に使う(型の要素自体は出ない)。`{"src":"id","mode":"alpha"}` で id の要素を型に。モード `alpha` `luma` `alpha-inv` `luma-inv` |
| `parent` / `id` | `"parent":"A"` で `id:"A"` の要素の動き(位置・拡大・回転)について行く(輪になるとエラー) |
| `blend` | 合成モード `normal` `add` `screen` `multiply` `overlay` `darken` `lighten` `difference` |
| `region`(画面を変える fx のパラメータ) | `{"shape":"rect","rect":[...]}` などでその範囲だけにかける |

- **調整レイヤー** `{"kind":"adjust","start","end","track","fx":[...]}`: 下に重なっている全部(背景・下のトラック)に fx をかける。mask で範囲を絞れる。`fx` 必須。
- **プリコンポーズ** `{"kind":"comp","start","end","layers":[...]}`(layers は overlays と同じ書き方・時刻は comp の頭から)か `{"kind":"comp","project":"別.vedit.json"}`(別のプロジェクトを書き出して1つの絵として使う)。comp 自体に keys・fx・mask などをかけられる。

### プリセット(`vedit/presets/*.json`)
決まった演出の組を1行で書く。**書き出す時だけ展開する**(JSON はそのまま。GUI のタイムラインには1つのクリップで出る)。展開した中身は `vedit expand` で見る。
- `{"preset":"impact","text":"第1位\n100点","start":4.37,"end":6.36}` → 強調テロップ(pop)+ 白フラッシュ + 衝撃音(1→2→3 と回す)。`file` を渡すと絵を出す。
- `{"preset":"chapter","gap_at":14.297,"start":0,"end":5.0,"file":"gfx/章/1.png"}` → 空白の前後に wipe-diag + シーン切り替えの音、空白の中に章の絵 + 和太鼓。
- パラメータは表の既定を上書きできる(`"flash":false`、`"se_gain":"match:ぽペン-2"`、`"track":7` など)。自動で入る値: `start` `end` `len` `n`(同じプリセットの何回目か) `gap_at` `gap_len`。
- 新しいプリセットは `presets/名前.json` を足すだけ: `{"doc","params":{既定},"required":[...],"styles":{...},"items":[{"to":"overlays|telops|sfx|v1_fx|transitions","if":"param|!param", ...}]}`。値に `"$param"`(null ならキーごと消える)・`"$=start+0.2"`(式)・`{"$pick":[...]}`(n 回目で回す)。`styles` は project に同じ名前が無い時だけ足す。
- **効果音の音量 `gain_db: "match:ぽペン"`**: 基準の音(`se` などから探す。`VEDIT_SE_DIRS` で足せる)と同じピークになる dB(+1dB、-12〜+6 に収める)。`"match:ぽペン-3"` で更に -3dB。ピークは `%LOCALAPPDATA%/vedit/peaks.json` にキャッシュ。sfx・transitions の se・プリセットのどれにも書ける(CLI: `vedit sfx ... --gain match:ぽペン`)。

### 一覧(`vedit fx list` と同じ。px は 1080p 基準)

#### 出入り(in_anim / out_anim)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `fade` | 透明から出る(out は透明へ消える) | `dur`=0.3 |
| `slide-left` | 左から入る(out は左へ出る) | `dur`=0.3 |
| `slide-right` | 右から入る(out は右へ出る) | `dur`=0.3 |
| `slide-up` | 下から入る(out は下へ出る) | `dur`=0.3 |
| `slide-down` | 上から入る(out は上へ出る) | `dur`=0.3 |
| `pop` | 大きく出て縮む→少し揺れて止まる(強調テロップの出だし)。dur を12等分した1コマずつ scales の大きさ、amps の幅(1080p 基準 px)で揺れる。最初の2コマは半透明 | `dur`=0.4, `scales`=[1.9, 1.55, 1.28, 1.1, 0.95, 0.98, 1.02, 1.0], `amps`=[0, 0, 0, 0, 14, 12, 10, 8, 6, 4, 2, 0], `alphas`=[0.5, 0.75], `seed`=0 |
| `zoom-in` | 小さく(from 倍)から拡大して出る。fade=透明から、ease | `dur`=0.3, `from`=0.3, `ease`="ease-out", `fade`=true |
| `zoom-out` | 大きく(from 倍)から縮んで出る。fade=透明から、ease | `dur`=0.3, `from`=2.0, `ease`="ease-out", `fade`=true |
| `bounce` | 上から落ちてきて弾んで止まる。height=落ちる高さ(1080p 基準 px)、fade=最初の1/5で透明から | `dur`=0.6, `height`=220, `fade`=true |
| `stamp` | スタンプ: 大きく(from 倍)から hit(dur に対する比)で原寸にドンと当たり、残りの時間で小さく揺れて止まる。amp=揺れ幅(1080p 基準 px) | `dur`=0.45, `from`=2.4, `hit`=0.35, `amp`=10, `seed`=0 |
| `blur-in` | ぼかしから出る(透明から)。radius=最初のぼかし(1080p 基準 px) | `dur`=0.3, `radius`=30, `ease`="ease-out" |
| `blur-out` | ぼけながら消える(out_anim 用。in_anim に書くと blur-in と同じ)。radius=最後のぼかし(1080p 基準 px) | `dur`=0.3, `radius`=30, `ease`="ease-out" |
| `rotate-in` | 回りながら拡大して出る。angle=最初の角度(度。負=反時計回りから)、from=最初の大きさ | `dur`=0.4, `angle`=-180, `from`=0.2, `ease`="ease-out" |
| `wipe-left` | マスクで左から右へ現れる(out は同じ向きに消える)。feather=境目のぼかし(1080p 基準 px) | `dur`=0.3, `feather`=30, `ease`="ease-out" |
| `wipe-right` | マスクで右から左へ現れる(out は同じ向きに消える)。feather=境目のぼかし(1080p 基準 px) | `dur`=0.3, `feather`=30, `ease`="ease-out" |
| `wipe-up` | マスクで下から上へ現れる(out は同じ向きに消える)。feather=境目のぼかし(1080p 基準 px) | `dur`=0.3, `feather`=30, `ease`="ease-out" |
| `wipe-down` | マスクで上から下へ現れる(out は同じ向きに消える)。feather=境目のぼかし(1080p 基準 px) | `dur`=0.3, `feather`=30, `ease`="ease-out" |
| `shake-in` | 強く揺れながら出て、揺れがおさまる。amp=[横,縦](1080p 基準 px)、freq=[横,縦]、fade=透明から出る秒 | `dur`=0.4, `amp`=[30, 18], `freq`=[70, 55], `fade`=0.1 |
| `typewriter` | テロップを1文字ずつ出す(telops 専用。out は1文字ずつ消える)。interval=1文字の秒(dur は 文字数×interval になる) | `dur`=0, `interval`=0.06 |

#### エフェクト(fx / v1_fx)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `shake` | 揺れ(だんだん弱まる sin)。amp=[横,縦] 1080p 基準 px、freq=[横,縦] 角速度(rad/秒)、dur=揺れる秒、decay=linear/exp/none。overlays の fx、v1_fx(背景)に使える | `amp`=[12, 7], `freq`=[95, 77], `dur`=0.5, `decay`="linear" |
| `zoom` | ゆっくり寄る/引く(Ken Burns)。from→to の倍率へ dur 秒(省略=fx の終わりまで)で、center=[x,y](中身の外枠に対する比)を中心に。ease | `from`=1.0, `to`=1.15, `center`=[0.5, 0.5], `dur`=null, `ease`="linear" |
| `drift` | ゆっくり流れる。to=[横,縦] 動く量(1080p 基準 px)を dur 秒(省略=fx の終わりまで)で。ease | `to`=[60, 0], `dur`=null, `ease`="linear" |
| `mosaic` | モザイク(プレイヤー名を隠すなど)。rect=[x,y,w,h](1080p 基準 px。省略=全体)、shape=rect/ellipse/polygon(points)、size=ブロックの大きさ(px)、feather=境目のぼかし。rect/points はキーフレームで動かせる。region={shape,rect,...} を付けるとその範囲だけ | `rect`=null, `shape`="rect", `points`=null, `size`=24, `feather`=0, `invert`=false |
| `blur-region` | 範囲をぼかす(名前を隠すなど)。rect=[x,y,w,h](1080p 基準 px)、shape、points、radius=ぼかし(px)、feather。rect/points はキーフレームで動かせる。region={shape,rect,...} を付けるとその範囲だけ | `rect`=[760, 440, 400, 200], `shape`="rect", `points`=null, `radius`=20, `feather`=8, `invert`=false |
| `blur` | 全体をぼかす。radius=ぼかし(1080p 基準 px)。region={shape,rect,...} を付けるとその範囲だけ | `radius`=8 |
| `motion-blur` | 動きのぶれ。length=0 なら要素自身の動き(出入り・keys・揺れ)から作る(shutter=1コマのうち開いている割合、samples=重ねる数)。length>0 なら angle(度)の向きに length(1080p 基準 px)ぶれる。region={shape,rect,...} を付けるとその範囲だけ | `length`=0, `angle`=0, `shutter`=0.5, `samples`=8 |
| `glow` | 光る(明るい所がにじむ)。radius=にじむ幅(1080p 基準 px)、strength=強さ、threshold=光らせる明るさ(0〜1)、color=光の色(null=元の色)。region={shape,rect,...} を付けるとその範囲だけ | `radius`=20, `strength`=1.0, `threshold`=0.6, `color`=null |
| `shadow` | 影(要素の形の影を下に置く)。x,y=ずらす量(1080p 基準 px)、blur=影のぼかし、color、opacity。region={shape,rect,...} を付けるとその範囲だけ | `x`=8, `y`=8, `blur`=8, `color`="#000000", `opacity`=0.6 |
| `brightness` | 明るさ。amount=-1〜1(0=そのまま)。region={shape,rect,...} を付けるとその範囲だけ | `amount`=0.2 |
| `contrast` | コントラスト。amount=-1〜1(0=そのまま)。region={shape,rect,...} を付けるとその範囲だけ | `amount`=0.2 |
| `saturation` | 彩度。amount=-1〜1(-1=白黒、0=そのまま)。region={shape,rect,...} を付けるとその範囲だけ | `amount`=0.3 |
| `tint` | 色をつける(暗い所→black の色、明るい所→white の色)。amount=かかり具合。region={shape,rect,...} を付けるとその範囲だけ | `black`="#000000", `white`="#FFB040", `amount`=1.0 |
| `grayscale` | 白黒。amount=かかり具合。region={shape,rect,...} を付けるとその範囲だけ | `amount`=1.0 |
| `invert` | 色を反転。amount=かかり具合。region={shape,rect,...} を付けるとその範囲だけ | `amount`=1.0 |
| `vignette` | 周りを暗く(color に)する。amount=濃さ、radius=暗くし始める所(画面の対角の半分に対する比)、soft=暗くなる幅。region={shape,rect,...} を付けるとその範囲だけ | `amount`=0.6, `radius`=0.6, `soft`=0.5, `color`="#000000" |
| `chroma-key` | 色を抜く(グリーンバック)。color=抜く色、similarity=抜く近さ(0〜1)、smooth=境目の幅、spill=色かぶりを抑える強さ。region={shape,rect,...} を付けるとその範囲だけ | `color`="#00FF00", `similarity`=0.35, `smooth`=0.1, `spill`=0.5 |
| `rgb-split` | 色ずれ(赤と青を逆向きにずらす)。amount=ずらす量(1080p 基準 px)、angle=向き(度)。region={shape,rect,...} を付けるとその範囲だけ | `amount`=8, `angle`=0 |
| `glitch` | グリッチ(横の帯がずれて色がずれる)。strength=強さ、every=何コマごとにずれ方を変えるか、seed。region={shape,rect,...} を付けるとその範囲だけ | `strength`=1.0, `every`=2, `seed`=7 |
| `mirror` | 反転。axis=x(左右)/y(上下)、half=true で半分を折り返す(左半分を右へ写す)。region={shape,rect,...} を付けるとその範囲だけ | `axis`="x", `half`=false |
| `outline` | 縁取り(形のまわりに線)。width=太さ(1080p 基準 px)、color、opacity。region={shape,rect,...} を付けるとその範囲だけ | `width`=6, `color`="#000000", `opacity`=1.0 |

#### 生成要素(overlays の kind)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `speedlines` | 集中線(真ん中は空ける)。count=本数、inner=空ける半径(画面の対角の半分に対する比)、color、seed。animate=true で every コマごとに線を描き直す(ちらつく) | `count`=110, `inner`=0.42, `color`="#FFFFFF", `seed`=3, `blur`=1.2, `animate`=false, `every`=2 |
| `gloom` | ズーン: 画面を暗くして、上から縦線(ガーン線)を垂らす。dark=暗さ(0〜1)、bg=暗くする色、line_color、line_alpha(0〜255)、seed。grow=秒(0=最初から全部。>0 で線が上から伸びる) | `dark`=0.5, `bg`="#08061E", `line_color`="#968CFF", `line_alpha`=150, `seed`=5, `grow`=0 |
| `flash` | フラッシュ: 色が一瞬出てすぐ消える。dur=全体の秒(end を省くとこの長さ)、fade=最後に消えていく秒、opacity、color | `color`="#FFFFFF", `opacity`=0.6, `dur`=0.14, `fade`=0.1 |

#### トランジション(transitions / cut_fx.video)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `wipe-diag` | 斜めの帯: 赤い先端+黒の帯が左から画面を覆い(継ぎ目の前 dur 秒)、同じ向きに右へ抜ける(後 dur 秒)。angle=傾き(度)、edge_color/edge_w=先端の色と幅(1080p 基準 px)、color=帯の色、ease | `angle`=18, `edge_color`="#C8141B", `edge_w`=90, `color`="#0E0E12", `dur`=0.4, `ease`="smoothstep" |
| `dip-black` | 黒に溶けて、黒から出る(継ぎ目の前後 dur 秒ずつ) | `dur`=0.15, `color`="#000000", `ease`="linear" |
| `dip-white` | 白に溶けて、白から出る(継ぎ目の前後 dur 秒ずつ) | `dur`=0.15, `color`="#FFFFFF", `ease`="linear" |
| `flash` | 継ぎ目で白く光る(前後 dur 秒で強まって消える)。opacity=いちばん強い時の濃さ | `dur`=0.12, `color`="#FFFFFF", `opacity`=1.0, `ease`="ease-in" |
| `iris` | 丸く閉じて(継ぎ目で全部 color)、丸く開く。center=[x,y](画面比)、soft=縁のぼかし(1080p 基準 px) | `dur`=0.35, `color`="#000000", `center`=[0.5, 0.5], `soft`=3, `ease`="ease-in-out" |
| `crossfade` | 前の映像と後の映像を重ねて入れ替える(継ぎ目の前後 dur 秒ずつ。カットされた部分の映像を使う) | `dur`=0.25, `ease`="linear" |
| `push-left` | 前の映像を左へ押し出して、後の映像が入ってくる(継ぎ目の前後 dur 秒ずつ) | `dur`=0.3, `ease`="smoothstep" |
| `push-right` | 前の映像を右へ押し出して、後の映像が入ってくる(継ぎ目の前後 dur 秒ずつ) | `dur`=0.3, `ease`="smoothstep" |
| `push-up` | 前の映像を上へ押し出して、後の映像が入ってくる(継ぎ目の前後 dur 秒ずつ) | `dur`=0.3, `ease`="smoothstep" |
| `push-down` | 前の映像を下へ押し出して、後の映像が入ってくる(継ぎ目の前後 dur 秒ずつ) | `dur`=0.3, `ease`="smoothstep" |
| `zoom` | 前の映像に寄っていき(scale 倍まで)、後の映像は寄った所から引いて出る。blur=寄る勢いのぶれ(0〜) | `dur`=0.25, `scale`=1.8, `blur`=0.08 |
| `whip` | 速い横ブレ(ホイップパン): 前の映像が左へ流れ、後の映像が右から流れて止まる。dist=流れる距離(画面幅に対する比)、blur=ぶれの強さ | `dur`=0.2, `dist`=0.6, `blur`=0.12 |
| `glitch` | 継ぎ目の前後でノイズのように横ずれ・色ずれして切り替わる。strength=強さ、seed | `dur`=0.15, `strength`=1.0, `seed`=7 |

#### 文字アニメーター(telops の animator)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `pop` | 1文字ずつポンと出る(小さい→少し大きい→原寸)。overshoot=行き過ぎの強さ。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false, `overshoot`=2.2 |
| `fade` | 1文字ずつ透明から出る。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false |
| `rise` | 1文字ずつ下から浮かび上がる。dist=動く量(1080p 基準 px)。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false, `dist`=40 |
| `drop` | 1文字ずつ上から落ちて弾む。dist=落ちる高さ(1080p 基準 px)。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false, `dist`=80 |
| `wave` | 文字が波打つ(出た後もずっと)。amp=揺れ幅(px)、freq=1秒の波の数、phase=隣の文字との波のずれ。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false, `amp`=12, `freq`=1.2, `phase`=0.5 |
| `shake` | 文字がばらばらに震える(出た後もずっと)。amp=揺れ幅(px)、every=何コマごとに動かすか、seed。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward/backward/center/random、out=true で同じ順に消える | `stagger`=0.05, `dur`=0.3, `order`="forward", `out`=false, `amp`=4, `every`=2, `seed`=5 |

#### プリセット(overlays / telops の preset)
| 名前 | 内容 | パラメータ(既定) |
|---|---|---|
| `champ` | 絵(チャンピオンのカードなど)を大きく出して揺らし(pop)、消えるときはフェード + ニュッ | `file`=null, `gap_at`=null, `pos`="full", `scale`=null, `track`=6, `fade`=0.15, `se`="ニュッ1.mp3", `se_gain`="match:ぽペン" |
| `chapter` | 章の頭: 空白(gaps)の前後に wipe-diag + シーン切り替えの音、空白の中に章の絵(file)か文字(text)+ 和太鼓。overlays に {"preset": "chapter", "gap_at": 空白の時刻, "start": 0, "end": 空白の長さ, "text": "其の一 数字で見る…"} と書く | `text`=null, `file`=null, `gap_at`=null, `style`="chapter", `pos`=[0.5, 0.5], `track`=6, `wipe`="wipe-diag", `wipe_se`="シーン切り替え2.mp3", `wipe_gain`="match:ぽペン-3", `se`="和太鼓でドン.mp3", `se_gain`="match:ぽペン" |
| `gloom` | ズーン: 画面を暗くして上から縦線(gloom)+ 落ち込む音。フラッシュ・衝撃音は無し。text / file を渡すと pop で出す | `text`=null, `file`=null, `gap_at`=null, `style`="impact", `pos`=[0.5, 0.5], `track`=6, `gloom_track`=3, `fade`=0.3, `se`="落ち込む.mp3", `se_gain`="match:ぽペン" |
| `impact-strong` | 強い強調: impact + 集中線(最初の 1.6 秒)+ 背景(V1)の揺れ。フラッシュは少し長め | `text`=null, `file`=null, `gap_at`=null, `style`="impact", `pos`=[0.5, 0.5], `track`=6, `out_anim`="fade", `flash`=true, `flash_dur`=0.16, `flash_track`=8, `lines`=1.6, `lines_track`=5, `shake`=true, `se`={"$pick": ["文字表示の衝撃音2.mp3", "文字表示の衝撃音3.mp3", "文字表示の衝撃音1.mp3"]}, `se_gain`="match:ぽペン" |
| `impact` | 強調: 文字(text)か絵(file)を pop で出す + 白いフラッシュ + 衝撃音(1→2→3 の順に回す) | `text`=null, `file`=null, `gap_at`=null, `style`="impact", `pos`=[0.5, 0.5], `track`=6, `out_anim`=null, `flash`=true, `flash_dur`=0.14, `flash_track`=8, `se`={"$pick": ["文字表示の衝撃音1.mp3", "文字表示の衝撃音2.mp3", "文字表示の衝撃音3.mp3"]}, `se_gain`="match:ぽペン" |


## GUI(Premiere風の編集画面)
`vedit-gui.cmd`(動画をドラッグ&ドロップ or ダブルクリック)、または `python vedit_main.py gui <動画|project.json>`。
同じ `project.json` を編集するので、GUIを開いたままAIがJSON/CLIで編集しても2秒以内に画面へ反映される。

ショートカットは Premiere 準拠。`python vedit_main.py import-keys` で自分の Premiere のカスタム .kys を取り込める(`%APPDATA%/vedit/keymap.json` に保存、手で編集も可)。`splits`(分割点)は編集画面用の目印で、書き出しには影響しない。

主な操作(Premiere 準拠): ホイール=横スクロール / Ctrl・Shift+ホイール=縦 / Alt+ホイール=ズーム / トラック名列でホイール=全トラックの高さ / Alt・Ctrl+ホイールまたはトラック名の下端をドラッグ=そのトラックだけ(下端ダブルクリック・右クリックメニューで元に戻す)。
V1/A1 は分割点・カットで区切られたクリップ表示。クリップの端ドラッグ=リップルトリム、右クリック=メニュー。トラックの空きをドラッグ=範囲選択、Ctrl/Shift+クリック=追加選択。
J/K/L シャトル(押すたびに倍速・J は逆再生)、S=スナップ切替、Alt+←→=1フレーム移動(Shift併用で5)、Ctrl+C/V=コピー/再生ヘッドにペースト、Ctrl+A=全選択。「⇥ カット詰め」でカット後の並びで表示。
再生中にルーラー等をクリックしてシークすると再生は止まり、離しても止まったまま。素材の終わりがちょうど空白の位置なら、空白の直前で終わる(空白の中まで伸ばさない。`TimeMap.out_end` / app.js `outEnd`)。タイムライン右端(メーターの左)の縦スクロールバーでトラックを上下に送る(つまみのドラッグ・空いた所のクリックで1画面・ホイール)。上下に送ってもルーラー(シークバー)と再生ヘッドのつまみは上に残る(`stickRuler`)。クリップの端のつまみは位置で判定する(端から8px、短いクリップは幅の1/3まで。クリップの外側の空きでも端から6px以内ならつまめる。`edgeMode` / `edgeNear`)。右上に CPU / GPU / VRAM / メモリの使用率を2秒ごとに出す(`GET /api/sys`、`sysmon.py`。psutil と nvidia-smi。nvidia-smi は呼ぶたびに起動し常駐させない)。A1 の波形の高さは dB(-48〜0dB)で描く(リニアだと小さめの声がほぼ線になり無音に見えるため。`drawWave`)。再生ヘッドは、ルーラー(高さ36px。上のボタン列とは6px空けて押し間違えを防ぐ)か、トラックの空いた所で赤い線(±6px)をつまんで動かす。再生ヘッドのドラッグは、ルーラー左の「🧲 区切りで止める」がオンか Shift を押している間、区切り線(V1/A1 の分割点・カットの継ぎ目・空白、各トラックの白い分割線 = 前のクリップの終わりと次の頭がくっついている所)に吸着する(オン/オフは localStorage `vedit.seeksnap`)。Q/W(前/後ろへリップルトリム)は、いちばん近い区切り(分割点・カットの端・空白・ロックしていないトラックのクリップの始点/終点)で止まる。近さはカット後の時刻で比べる。
**同じトラックの同じ時間に素材は2つ重ねない**(V2…の各ビデオトラック・SE・帯・字幕。ユーザー指定 2026-10-10)。GUI では置いた/動かした/伸ばしたクリップが勝ち、同じトラックで重なった他のクリップは重なった部分を削る(丸ごと覆えば削除、内側に置けば前後に分割。`app.js` の `resolveOverlaps`、Undo で戻せる。右パネルの数値は入力し終えた時に適用)。JSON を直接書く時も重ねないこと。重ねたい素材は別のトラックへ。
再生ボタン横の時刻は「元素材」(黄)と「カット後」(青・総尺つき)の2つ。数字をクリックすると入力欄になり、`1:23.5` や `83.5`(全角も可)を入れて Enter でその時刻へ移動する(カット後の時刻は元素材の時刻に直して移動。Esc で取り消し)。⧉ でその時刻をコピーする。
プレビュー映像はプロキシ(`proj.cache/proxy.mp4`、720p・キーフレーム0.5秒おき、`proxy.py`。元動画のキーフレームが数秒おきだとカットを飛ぶシークで映像が止まるため)。GUI を開いた時に無ければ自動で作り(`/api/job` kind `proxy`、NVENC で50分の素材が約2分)、できたら切り替える。元動画が変わると作り直す。書き出し・`frame` は常に元動画。
差し込み画像のプレビューも同じ考え方: GUI は `/media?...&pv=1` で読み、サーバー(`preview_image`)がキャンバス(1920x1080)に収まる大きさに縮めた画像を `proj.cache/pvimg/` に作って返す(4K画像はデコードが重く、出るたびにかくつくため)。書き出しは元の画像。画面上の差し込み要素は、画像なら「トラック+ファイル」で使い回す(同じ画像が細切れで続く所で作り直さない)。
プレビュー再生は、サーバーがカット後の並びにつないだ試聴用音声(`proj.cache/preview_<hash>.wav`、`POST /api/preview_audio`、`audio.preview_cut`)を基準に鳴らし、映像を音に合わせる(削った音の漏れ・音の巻き戻りを防ぐ。カット変更後は作り直すまで従来の映像基準)。再生ヘッドをルーラーでつまんで端へ寄せるとタイムラインが自動で送られる。
「SE 効果音」レーン(A1 と BGM の間): 「+ 効果音」で選んだ音を再生ヘッドの位置に置く(音量は直前の効果音を引き継ぐ)。ドラッグ移動・端のトリム(左端は音の頭を削る=`in` が進む)・削除・Undo・コピー/ペースト・分割は他のクリップと同じ。右パネルで開始/終了・音量dB・音の開始(in)・「長さを音に合わせる」。プレビュー再生では効果音も試聴用音声に混ぜ込む(sfx もキャッシュキーに入る。作り直し中は Web Audio で先にデコードした音を個別に鳴らす)。自動の `end`(音の長さどおり)は保存時に省く。
トラック名の🔓でロック(選択・移動・削除不可)、👁で表示/非表示(映像トラック)、M でミュート(音声トラック)。Shift+クリックで同じ側(映像=V*/TB/T/V1、音声=A1/SE/BGM)のトラックを一括で切り替え。トラック名の右クリック=トラックのメニュー(ロック・表示/ミュート・クリップをすべて選択・名前を変更・上/下にビデオトラックを追加・削除・空のビデオトラックを削除・差し込みの追加先・すべて表示/ミュート解除・すべてロック解除・高さを戻す)。V トラック名のクリック=差し込みの追加先(青い線)、上下にドラッグ(左端の⠿が目印)=ビデオトラックの並べ替え(ドラッグ中はトラック名の写しがカーソルにつき、落とす位置をトラック名列とタイムラインに太い青線で出す。重なり順。差し込みの track・名前・目・ロックごと付け替え。メニューの「トラックを上へ/下へ移動」も同じ)。差し込みは上下ドラッグ・右クリック「上/下のビデオトラックへ移動」で別トラックへ。モニター上の差し込み画像はクリックで選択・ドラッグで移動・右下ハンドルで拡大縮小。
右パネルの「ヒストリー」タブ(Premiere のヒストリーパネル): 元に戻すの控え(`undoS`/`redoS`、最大200)を操作名つきで並べる。操作名は前後の状態を比べて自動で付ける(`histDescribe`: 移動/トリム/トラック移動/追加/削除/分割/カット/○○を変更。操作ごとに名前は持たせない)。何も変わらない控えは出さない。クリックでその時点へ一度に戻る/進む(`histJump`)、灰色はやり直せる操作で、戻った先から編集すると消える。外部の変更を取り込むとそこから始まる(いちばん上の行が「外部の変更を取り込み」)。ヒストリーを開いている間はクリップを選んでもタブを切り替えない。
「別名で保存…」(上部バー、Ctrl+Shift+S): 保存ダイアログ(既定 `<今の名前>_2.vedit.json`)で選んだファイルに今の内容を書き、以後の自動保存・外部変更の監視・キャッシュ(`<名前>.vedit.cache`)は新しいファイルに切り替わる(元のファイルは残る)。別フォルダなら source/overlays/sfx/bgm/VST3 の相対パスを新しい場所から同じ実ファイルを指すよう書き換え(別ドライブは絶対パス。`output.file` はそのまま)、処理済み音声 audio.wav/audio.json は引き継ぐ(`POST /api/saveas`、`project.Project.save_as`)。
「書き出し」は設定ダイアログ(範囲 全体/イン〜アウト・H.264/H.265・解像度・fps・GPU/CPU・画質・ビットレート・音声kbps)。設定は `output` に保存: `height` `fps` `codec` `encoder` `quality`(high|standard|small) `bitrate_mbps` `audio_kbps`。
「🎨 画像編集」ボタン: 内蔵の軽量画像エディタ **vimg**(ブイイメージ。別窓 `/image.html?f=<相対パス>`)。新規画像 / テロップ画像(テロップのスタイル入り) / 今の画面の上に作る(画面を書き出さない下敷きに) / 選択中の差し込み画像を編集。保存すると `<名前>.vimg.json`(編集データ) と同名の透過PNG を書き、PNG が差し込みとして画面に自動反映。新規は `<projdir>/img/`。既存PNGを開くと元画像を `<名前>.src.png` に退避し、それを1枚目のレイヤーにした `.vimg.json` を作る。Photoshop 連携(`POST /api/photoshop`)は右クリックメニューに残っている。
メニュー末尾の「画像編集(vimg)を起動」は、プロジェクトに紐づかない単独の vimg を別プロセス・別窓で開く(`POST /api/vimg/launch`)。

## 修正対応(修正点の一覧・修正前との比較)
先方に見せた版の控えと修正点を `<名前>.review.json`(project の隣)に置く。project には入れないので書き出しに影響しない(`review.py`)。
```
vedit review proj.vedit.json start [--video 修正前.mp4] [--force]   # 今の project を修正前として控える(動画の既定は output.file → <名前>.mp4)
vedit review proj.vedit.json add --file 修正点.txt                 # 先方の文をそのまま(「0;07 画像」「042: 景気良い」も読める。時刻の無い行は前の点の続き)
vedit review proj.vedit.json list                                  # 修正点(先方の時刻 t / 今の時刻 now / 元素材 src)と、修正前から変えた所
vedit review proj.vedit.json done 3 / undone 3
vedit review proj.vedit.json export [--out 修正点.txt] [--undone] [--clip]   # vreview の管理ページに貼る文章(標準出力 / ファイル UTF-8 / クリップボード)
vedit review proj.vedit.json next [--video 修正前.mp4]             # 次の回(修正点2, 3…)を始める。前の回は残る
```
- 回: review.json は一番上が今回 `{created, video, base, points, prev:[前の回…(古い順)]}`(回を知らない古い vedit でも今回は読めるように)。prev の無い形は1回だけとして読む。add/list/done/undone/export は `--round N`(1始まり、既定は今回)。API は `?r=`(0始まり、無ければ今回)、`POST` の body に `r`。パネル見出し「修正点」は回が2つ以上あると選択(修正点1(前回) / 修正点2(今回))になる。「控え直す」(start)は全部の回を消して1回目からにする。
- `export`(`review.export_text`、API `GET /api/review/export?undone=0|1` → text/plain、パネルの「📋 vreview 用にコピー」)は読むだけ。1行1点「時刻 内容」、時刻は**今の動画の時刻**(パネル・list の now と同じ)で `分:秒` か `分:秒.百分の1秒`(分は3桁まで)、now の昇順。カットで消えた点はそのカットの継ぎ目の時刻に `[カット済] ` を付ける(vreview で橙)。複数行の文は半角スペースで1行に。見出し・番号は付けない(vreview の parseFixes が時刻の無い行を前の点の続きにするため)。vedit から vreview へは送らない(貼るのは人)。
  ```
  0:42 景気良い
  3:23.44 [カット済] 2回いってるから1回目カット
  4:01.24 画像(画像と言われただけ 謎)
  ```
- 修正点の時刻は**先方が見た動画の時刻**。修正前のカットで元素材の時刻 `src` に直すので、カットを足して後ろがずれても正しい所を指す。
- パネルの時刻は**今の動画の時刻**(カットで縮んだ分を直したもの)を主に出し、違う時は先方の時刻を「(先方 m:ss)」で並べる。カットで消えた所は「カット済」。
- 手順: 先にテロップ修正・画像配置(先方の時刻で src を決める)→ 最後にカット。timebase source なのでカット後も配置はずれない。
- GUI 上部の「📝 修正点」で右に4列目のパネル(`review.js`)。クリックでその所へシーク、チェックで済み、「修正点を足す」に貼り付け。上に修正前の動画(音は既定で消音)が再生ヘッドに合わせて動く(元素材の時刻で対応づけ。修正前に無い所はその旨を出す)。下に「修正前から変えた所」(カット・空白・差し込み/テロップ/トピック/効果音の追加・変更・削除)。保存のたびに読み直す。
- 修正前の動画は軽い版(`/media?...&pv=a` → `proxy.make_video(audio=True)`、720p・キーフレーム0.5秒おき・音入り、`pvvid/`)で出す。元の mp4(1080p)を一緒に流すと本編の再生が引っかかるため。できるまでは元の mp4。再生中はシークせず速さを ±10% 変えて追いつき、1.5秒以上ずれた時だけシークする。
- 「⧉ 別窓」で `review.html` に出す。編集画面とは `BroadcastChannel("vedit-review-"+TOKEN)` で位置(pos)・シーク(seek)・保存(saved)をやり取りする。「⇤ 右に戻す」で戻る。
- API: `GET /api/review`(state)、`POST /api/review {op: start|next|add|update|delete|video, r}`。

## vimg(画像編集アプリの単独起動)
`vimg.cmd`(ダブルクリック / 画像や `.vimg.json` をドラッグ&ドロップ)、または `python vedit_main.py vimg [ファイル(.vimg.json か画像) | フォルダ]`。動画プロジェクトなしで Edge の `--app` 窓に開く。
- 引数なし/フォルダ: はじめの画面(新規作成のサイズ選択・ファイルを開く・フォルダ内の画像一覧)。画面上部に「新規」「開く」「名前を付けて保存」(Ctrl+O / Ctrl+Shift+S)。
- 保存は開いたファイルの場所(`<名前>.vimg.json` + 同名の透過PNG)。新規は一時フォルダで編集し、最初の保存時に保存ダイアログ。取り込んだ画像は保存先の `assets/` へ移す。
- 閲覧・保存できるのは、開いたファイル/選んだフォルダ/保存先の配下と、開いた文書が参照する画像だけ(`server.serve_vimg`、`Handler.standalone`)。単独起動ではパスは絶対パスでやり取りする。
- 窓を閉じて 180 秒(`--idle`)生存確認が途絶えるとサーバーは自動終了。テスト用に `--no-window --port N`。

## 画像編集(vimg)の文書(*.vimg.json)
AIが直接書いてよい。`vedit image doc.vimg.json [-o out.png]` で透過PNGに書き出す(Edge headless で描画するので GUI と同じ見た目。省略時は同名 .png)。
| キー | 内容 |
|---|---|
| `width` `height` | キャンバスの px(既定 1920×1080) |
| `background` | `null`=透明 / `"#RRGGBB"`=単色 |
| `layers` | 下→上の順。各要素は下の共通キー + 種類ごとのキー |
| 共通 | `type`(`text`/`shape`/`image`) `name` `x` `y`(=中心) `w` `h` `rot`(度) `opacity`(0〜1) `visible` `locked` `export`(`false`=書き出さない下敷き) `shadow`(`{"color","opacity","x","y","blur"}` か `null`) |
| `text` | `text`(改行 `\n`) `font`(既定 `HGP創英角ｺﾞｼｯｸUB`) `size` `color` `bold` `line_height`(倍率) `letter_spacing`(px) `scale_x`(横比率%・既定100=Photoshopの水平比率) `max_w`(px。一番長い行がこれを超えたら横だけ縮める。0=なし。新テロ２相当のテロップは 1840) `align`(left/center/right) `valign`(top/middle/bottom) `outlines`(外側→内側 `[{"color","width"}]`、既定 黒6) `gradient`(`{"top","bottom"}` か `null`) `auto`(true=枠を文字に合わせる) |
| `shape` | `shape`(`rect`/`roundrect`/`ellipse`) `fill`(色 か `null`) `stroke`(`{"color","width"}` か `null`) `radius` |
| `image` | `src`(文書からの相対パス。D&D した画像は `assets/` に取り込まれる) `flip` `adjust`(`{"brightness","contrast","saturation"}` 各 -100〜100) |
省略したキーは既定値。`x`/`y`/`w`/`h` を省くと画像は原寸で中央、文字は中央・枠は自動。

