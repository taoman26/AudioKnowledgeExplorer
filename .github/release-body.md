## AppImage (Linux, x86-64)

`AudioKnowledgeExplorer-<version>-x86_64.AppImage` をダウンロードして、実行権限を付けてダブルクリックするだけで動きます(インストール不要)。

```sh
chmod +x AudioKnowledgeExplorer-*.AppImage && ./AudioKnowledgeExplorer-*.AppImage
```

- 動作条件: x86-64 の Linux、glibc 2.35 以上(Ubuntu 22.04 以降が目安)、**AVX2 / FMA / F16C 対応の CPU(2013 年以降)**。FUSE 2(Ubuntu では `libfuse2`)が必要です。無いときは `--appimage-extract-and-run` を付けて実行してください。
- 表示は X11(xcb)です。Wayland のデスクトップでは XWayland を使います。
- **whisper のモデルは同梱していません。** 設定中のモデル(標準は `ggml-small.bin`)が無ければ、起動時に、アプリが Hugging Face から自動でダウンロードします(約 0.5 GB。進捗は画面上部に出て、中止もできます)。設定で `large-v3-turbo`(約 1.6 GB)も選べます。日本語にはこちらをお勧めします([README](https://github.com/taoman26/AudioKnowledgeExplorer#モデルの用意))。
- 日本語入力(fcitx5)用のプラグインが入っています。

### ライセンスとソース
アプリ本体は MIT です。**この AppImage は、GPL の部品を含みます**(Ubuntu 22.04 の FFmpeg は GPL v2+ のビルド、GStreamer の plugins-ugly など)。同梱しているライブラリと、元の Ubuntu のパッケージの一覧は、AppImage の中の `usr/share/doc/AudioKnowledgeExplorer/BUNDLED-PACKAGES.txt` にあり、ソースは `apt source <パッケージ名>`(Ubuntu 22.04)で入手できます。アプリ本体のソースは、このタグの Source code(zip / tar.gz)です。ビルド手順は `packaging/appimage/` にあります。

### 既知の制限
アルファ版です。文字起こしの精度はモデルに依存します。詳しくは README の「既知の制限」を見てください。

---
