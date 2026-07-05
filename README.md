# agri-display-atom

M5Stack **Atom Display**（ESP32-PICO-V3-02 / PSRAM 2MB / Flash 8MB / LT8618SX + Gowin FPGA、720p HDMI 出力）を使った、**agriha MQTT の環境データを HDMI モニタに表示する「表示専用ノード」**。

`agri-*` ファミリーの新メンバー。ただしセンサーは持たず、broker を **subscribe して描画するだけ**の消費側ノード。

---

## 位置づけ / 分業

- データ源は **agriha MQTT（`pi4.local:1883`、retain 付き `{value,unit,ts}`）**。CCM は拾わない（正規化前・ArSprout 専用のため。表示機は MQTT 直結が素直）。
- broker に接続した瞬間、retain で全系列の現在値が届くのでポーリング不要。
- WiFi 機なので `agri-node-poe-core`（W5500/ETH 前提）は載らない → **`agri-amp-wifi` の前例**に倣い、WiFi + PubSubClient + WebServer ベースで自前実装し、見た目・`/api/status` スキーマ・OTA・NVS を family 流儀に合わせる。

## ハードウェア要点

| 項目 | 値 | 実装への影響 |
|---|---|---|
| MCU | ESP32-PICO-V3-02 | WiFi 2.4G 内蔵 |
| PSRAM | 2MB | **1280×720×16bit=1.8MB のフルフレームバッファを PSRAM スプライトに確保 → ダブルバッファでちらつきゼロ** |
| Flash | 8MB | 日本語フォント＋3スキンでも余裕 |
| 出力 | 720p(1280×720) HDMI | 描画は 1280×720 固定 |
| ライブラリ | M5GFX（`M5AtomDisplay.h`） | efont 日本語フォント同梱＝室温/湿度等のラベル可 |
| Grove | ×1 | 将来ローカル入力に流用可（現状未使用） |

## 表示仕様（デザイン確定済み・モック参照）

- **主役ハウス1つを大きく表示**（既定 house2 別棟）。室温を主役（リングゲージ＋60分トレンド）。
- レイアウト: 左=室温リング＋ノード死活 / 中央=室温トレンド＋CO2・飽差＋警告バナー / 右=湿度・気圧・電流・灌水・外気象 / 下=側窓開度＋イベントログ。
- モック: `scratchpad/nerv-dashboard.html`（1280×720 実寸、そのまま描画設計図）。

### スキン（WebUI で選択、NVS 永続）

トークン（配色・書体・パネル形状・演出フラグ）を差し替える方式。レイアウトエンジンは共通。

| スキン | 地 | アクセント | 書体 | 形状/演出 |
|---|---|---|---|---|
| `eva` | 温黒 #0a0806 | NERV橙 #EB6608 | 明朝×等幅 | 六角切欠き・ハザード・スキャンライン |
| `consumer` | 明るい #f4f6f3 | 緑 #2FA36B | ゴシック | 角丸カード・やわらか文言 |
| `corp` | 淡灰青 #eaeef3 | 青 #126FB8 | ゴシック | 角控えめ・業務ダッシュボード調 |

### 状態テーマ（温度閾値で自動切替、画面全体を変色）

| 状態 | 既定閾値(室温) | 演出 |
|---|---|---|
| `normal` | < 26.8℃ | 通常 |
| `caution` | ≥ 26.8℃ | アンバー・注意帯 |
| `alert` | ≥ 27.6℃ | 橙赤・警告帯＋枠 |
| `danger` | ≥ 28.6℃ | 赤ウォッシュ＋枠パルス（EVAは背景「警告」大明朝） |

演出の強さはスキンごとに調整（EVA=劇的 / consumer・corp=上品）。閾値・文言は WebUI で編集可。

## 設定（NVS + WebUI `http://agri-display-01.local/config`）

- **接続**: WiFi（WiFiManager キャプティブポータル、起動時ボタン長押しで再portal）、`mqtt_host`（既定 `pi4.local`）、`house`（主役ハウス）。
- **表示項目**: タイルのリスト（`topic / ラベル / 単位 / 桁 / 閾値色 / 配置スロット`）を JSON で保持。**subscribe 中に見たトピックを自動収集**し、タイルの topic をドロップダウンから選べるようにする（手打ち不要）。
- **スキン**: `eva|consumer|corp`。
- **状態閾値**: 注意/警告/危険の温度、監視対象トピック（既定 `agriha/<house>/sensor/InAirTemp`）。
- **時計**: SNTP（JST）。

## 購読トピック（例）

```
agriha/<house>/sensor/InAirTemp   InAirHumid  InAirCO2  InAirHD  InAirPressure
agriha/<house>/sensor/Flow        Current
agriha/1/sensor/Drain
agriha/farm/weather/WAirTemp      WWindSpeed  WRainfallAmt  WRadiation
agriha/<house>/window/<id>        (pct/target/moving/src)
agriha/<house>/relay/state
agriha/<house>/sys/<node>/online  (死活)
```

## 実装ロードマップ

1. [ ] PlatformIO プロジェクト（board=Atom Display、M5GFX + PubSubClient + WiFiManager）＋ビルド通し
2. [ ] ブリングアップ: HDMI 出力（テストパターン）→ WiFi 接続 → MQTT subscribe → シリアルに受信ダンプ
3. [ ] レンダラ: PSRAM フルスプライト、トークン駆動テーマ（スキン×状態）、リング/トレンド/タイル描画
4. [ ] 状態機械: 室温閾値 → 状態テーマ自動切替
5. [ ] WebUI: `/config`（タイル編集・スキン・閾値）、`/api/status`、自動トピック収集、NVS 永続
6. [ ] mDNS `agri-display-01.local` + HTTP OTA + GitHub Release セルフ更新（family 流儀）

## 罠メモ（family 共通・着手時に確認）

- ビルドはネイティブ PowerShell の `pio.exe`（git-bash 不可）、`$env:PYTHONIOENCODING="utf-8"`。
- `pi4.local` は on-device では lwIP mDNS で解決可（PC からは IPv6 優先で外しやすい＝IPv4 直指定）。broker 実IPは DHCP で漂流するので **mDNS 名で持つ**。
- WebServer 部分POST でチェックボックスが off に落ちる family 既知罠に注意（設定は全フォーム再構成で POST）。
</content>
