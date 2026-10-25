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

## Windows (x64, ポータブル zip)

`AudioKnowledgeExplorer-<version>-windows-x64.zip` を展開して、`AudioKnowledgeExplorer.exe` を起動します(インストール不要)。

- 動作条件: Windows 10 / 11(x64)、**AVX2 / FMA / F16C 対応の CPU(2013 年以降)**。
- ライブラリとモデルは `%APPDATA%\AudioKnowledgeExplorer` に保存されます。whisper のモデルは同梱せず、無ければ初回起動時にダウンロードします。
- 日本語の形態素解析(MeCab と JUMAN 辞書)を同梱しています。日本語や空白を含むフォルダに展開しても動きます(その場合、辞書を `%ProgramData%\AudioKnowledgeExplorer\mecab` にコピーして使うことがあります)。
- 署名をしていないため、初回起動時に SmartScreen の警告が出ることがあります(「詳細情報」→「実行」)。
- 自動テストと、展開した zip の起動確認(GitHub Actions)は通っています。**音の再生、フォルダの監視、日本語入力などの手動の操作は、実機ではまだ確認していません。**

### 既知の制限
文字起こしの精度はモデルに依存します。詳しくは README の「既知の制限」を見てください。

---
