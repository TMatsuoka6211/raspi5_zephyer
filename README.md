# Raspberry Pi 5 × Zephyr RTOS Wi-Fi & HTTPS Enablement

本リポジトリは、**Raspberry Pi 5 (BCM2712 / Cortex-A76)** 上で **Zephyr RTOS** をベアメタルネイティブに動作させ、オンボードの無線通信チップ **Infineon CYW43455** を経由した **Wi-Fi 接続・DHCP・DNS・TCP・HTTP/HTTPS (TLS 1.2 / TLS 1.3) 通信** を実現するためのドライバ実装、設定、およびパッチ集です。

---

## 🌟 主な実現機能・特徴

1. **SDIO ホストコントローラドライバ (`drivers/sdhc/sdhc_bcm2712.c`)**
   - BCM2712 の SDHCI コントローラ制御（4-bit バス幅 / 50MHz High-Speed モード / CMD53 バルク転送）
   - AArch64 64バイトキャッシュラインアライメント済みの 4096B バウンスバッファ実装
2. **オンボード Wi-Fi スタック (`Infineon AIROC / WHD`)**
   - CYW43455 との SDIO 接続、ファームウェア（`43455-fmac.bin`）および CLM BLOB のロード
   - 5GHz帯（802.11ac / 702 Mbps リンクレート）での高速接続
3. **ネイティブ ネットワークプロトコルスタック**
   - DHCPv4 による自動 IP / ゲートウェイ / DNS 取得
   - ICMP Ping 導通
   - 非同期 DNS リゾルバによるホスト名解決
4. **セキュリティ・暗号化レイヤ (`Mbed TLS` + `PSA Crypto`)**
   - TLS 1.2 & TLS 1.3 完全対応
   - RSA & ECDSA 暗号スイート（AES-128/256-GCM, SHA-256/384）
   - 主要な楕円曲線（`secp256r1`, `secp384r1`, `secp521r1`, `X25519`）のサポート
   - 64-bit ARM に最適化したスタックサイズ設計（Instruction Abort 防止）
5. **HTTP / HTTPS クライアント**
   - Zephyr Shell から `net http get -k https://google.com/` によるセキュアな Web ページ取得

---

## 📂 リポジトリ構成

```text
raspi5_zephyer/
├── doc/
│   └── rpi5-zephyr-wifi-https.md          # 技術書展21 頒布予定の技術解説原稿（詳細ドキュメント）
├── wifi_enablement/
│   ├── wifi_changes.patch                 # Zephyr リポジトリ本体向けパッチ
│   ├── hal_infineon.patch                 # modules/hal/infineon 向けパッチ
│   ├── patch_instructions.md              # パッチ適用とビルドの詳細手順書
│   ├── boards/raspberrypi/rpi_5/          # デバイスツリー (.dts) & defconfig
│   ├── dts/                               # BCM2712 DTS 定義 & バインディング YAML
│   ├── drivers/sdhc/                      # BCM2712 SDHCI ドライバ実装
│   ├── drivers/wifi/infineon/             # AIROC Wi-Fi HAL & SDIO Glue
│   ├── subsys/                            # SDIO / TLS / HTTP Client 修正コード
│   └── samples/hello_world/               # 動作検証用サンプルプロジェクト
│       ├── prj.conf                       # 最適化済み Kconfig 設定
│       ├── src/main.c                     # 診断ログ出力付きエントリーポイント
│       └── firmware/
│           ├── get_firmware.sh            # CYW43455 ファームウェア取得スクリプト
│           └── brcmfmac43455-sdio.txt     # NVRAM 設定ファイル
└── README.md
```

---

## 🚀 クイックスタートガイド

### 1. ワークスペースの準備
Zephyr SDK 1.0.0 (または 0.16.8+) および Python 仮想環境がセットアップされた Zephyr ワークスペースを用意します。

```bash
cd /path/to/zephyr-workspace/zephyr
```

### 2. パッチの適用
本リポジトリに収録されているパッチを適用します：

```bash
# 1. Zephyr 本体へのパッチ適用
git apply /path/to/raspi5_zephyer/wifi_enablement/wifi_changes.patch

# 2. hal_infineon へのパッチ適用
cd /path/to/zephyr-workspace/modules/hal/infineon
git apply /path/to/raspi5_zephyer/wifi_enablement/hal_infineon.patch
```

### 3. Wi-Fi ファームウェアの取得
ライセンスおよび再配布制約に配慮し、ファームウェアバイナリはリポジトリに含めていません。以下のスクリプトを実行して、Raspberry Pi 公式リポジトリからダウンロードしてください：

```bash
cd /path/to/zephyr-workspace/zephyr/samples/hello_world/firmware
./get_firmware.sh
```

### 4. ビルドの実行
```bash
cd /path/to/zephyr-workspace/zephyr
source /path/to/zephyr-env/bin/activate
west build -d build -b rpi_5/bcm2712 samples/hello_world
```

### 5. 実機での起動とテスト
生成された `build/zephyr/zephyr.bin` を FAT32 フォーマットの SD カードに `kernel8.img` として配置し、Raspberry Pi 5 を起動します。

シリアルコンソール（UART 115200 bps）から以下のコマンドを実行します：

```bash
# 1. Wi-Fi 接続 (SSID / パスワードを指定)
uart:~$ net wifi connect -s "Your_SSID" -p "Your_Password" -k 1 -b 2

# 2. DHCP での IP 取得確認
uart:~$ net ipv4

# 3. 外界との Ping 疎通確認
uart:~$ net ping 8.8.8.8

# 4. HTTPS 経由での Web ページ取得
uart:~$ net http get -k https://google.com/
```

実行結果例：
```html
TLS verification is disabled
Connecting to 142.251.118.113:443 (timeout: 15000 ms)...

<HTML><HEAD><meta http-equiv="content-type" content="text/html;charset=utf-8">
<TITLE>301 Moved</TITLE></HEAD><BODY>
<H1>301 Moved</H1>
The document has moved
<A HREF="https://www.google.com/">here</A>.
</BODY></HTML>
```

---

## 📖 技術解説ドキュメント（同人誌 原稿）

開発の全軌跡、SDIO プロトコルシーケンス、Instruction Abort 例外の解析、Mbed TLS エラー `-0x3a00` の突破法などを網羅した詳細な技術ドキュメントを [`doc/rpi5-zephyr-wifi-https.md`](doc/rpi5-zephyr-wifi-https.md) に収録しています。技術書展21にて頒布予定です。

---

## 📜 ライセンス

- ソースコードおよび設定ファイル: **Apache License 2.0**
- Wi-Fi ファームウェア: Broadcom / Cypress / Infineon のライセンス条項に準拠します（`get_firmware.sh` を通じて公式リポジトリより取得されます）。
