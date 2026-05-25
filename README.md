# Switch Macro Tool

PCブラウザでコントローラー操作を記録・編集し、**Bluetooth (BLE) でワイヤレス転送**。  
**M5Stack ATOMS3** が Nintendo Switch のコントローラーとして振る舞い、マクロを正確に自動再生します。

---

## ✨ 特徴

- **完全ワイヤレス転送**: マクロデータの更新に USB ケーブルの繋ぎ直しは不要
- **スタンドアロン再生**: ATOMS3 内部の LittleFS に保存。PC 不要で本体ボタンだけで再生
- **同時押し完全対応**: A+B, L+R 等も正確に再現
- **アナログスティック対応**
- **exeアプリ対応**: インストーラーまたはポータブル版 exe で単体動作（Electron ベース）

---

## 🚀 使い方

### A) リリース済み exe を使う（推奨）

[Releases](../../releases) から最新の  
`Switch-Macro-Tool-Setup-x.x.x.exe`（インストーラー）または  
`Switch-Macro-Tool-x.x.x-portable.exe`（インストール不要）をダウンロードして実行。

### B) Web ブラウザで使う（Chrome 推奨）

```bash
cd editor
npm install
npm run dev
```

### C) exe をローカルでビルドする

```bash
cd editor
npm install
npm run electron:build:win   # Windows .exe
npm run electron:build:mac   # macOS .dmg
npm run electron:build:linux # Linux .AppImage
# → editor/dist-electron/ に生成
```

---

## 🛠️ プロジェクト構成

| ディレクトリ | 説明 |
|---|---|
| `editor/` | React + Vite + Electron アプリ本体 |
| `editor/electron/` | Electron メインプロセス |
| `editor/src/` | React ソースコード |
| `esp/` | ATOMS3 用 Arduino ファームウェア |
| `schema/` | マクロ JSON スキーマ定義 |
| `.github/workflows/` | GitHub Actions 自動ビルド |

---

## 🔧 ファームウェアのセットアップ

1. Arduino IDE でボードマネージャから `M5Stack` を追加し `M5Stack ATOMS3` を選択
2. ライブラリマネージャから以下をインストール: `M5AtomS3` / `NimBLE-Arduino` / `ArduinoJson`
3. `esp/switch_macro_player/switch_macro_player.ino` を書き込み
4. ツール設定: USB CDC On Boot → `Disabled` / USB Mode → `USB-OTG (TinyUSB)`

---

## 🎮 使い方ガイド

1. ATOMS3 を Switch ドックの USB ポートに接続（「Pro Controller」として認識）
2. アプリを起動 → 「ATOMS3に接続」でBLEペアリング
3. コントローラー接続 → 「記録開始」
4. 操作後 → 「記録停止」→「ATOMS3に転送」
5. ATOMS3 の Button A でマクロ再生
