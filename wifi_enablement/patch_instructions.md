# WiFi / HTTPS 有効化パッチ適用手順および差分解説書

本ドキュメントは、Raspberry Pi 5 で Wi-Fi および HTTPS 通信を有効化するための 2 つのパッチファイル：
1. `wifi_changes.patch` (Zephyr RTOS 本体用)
2. `hal_infineon.patch` (Infineon HAL モジュール用)

を適用する手順、および各モジュール・ファイルの変更詳細をまとめたものです。

---

## 1. パッチファイルの適用手順 (How to Apply)

### 手順 1: Zephyr 本体へのパッチ適用
```bash
cd /path/to/zephyr-workspace/zephyr
git apply /path/to/raspi5_zephyer/wifi_enablement/wifi_changes.patch
```

### 手順 2: Infineon HAL へのパッチ適用
```bash
cd /path/to/zephyr-workspace/modules/hal/infineon
git apply /path/to/raspi5_zephyer/wifi_enablement/hal_infineon.patch
```

### 手順 3: ファームウェアバイナリのダウンロード
CYW43455 用の Wi-Fi ファームウェアおよび CLM BLOB を取得します：
```bash
cd /path/to/zephyr-workspace/zephyr/samples/hello_world/firmware
./get_firmware.sh
```

### 手順 4: ビルドと実行
```bash
cd /path/to/zephyr-workspace/zephyr
source ~/zephyr-env/bin/activate
west build -p always -b rpi_5/bcm2712 samples/hello_world
```
生成された `build/zephyr/zephyr.bin` を SD カードの直下に `kernel8.img` としてコピーして Raspberry Pi 5 を起動します。

---

## 2. 変更差分の詳細説明 (Detailed Changes)

### 2.1. デバイスツリー (DTS) レイヤー
- **`dts/arm64/broadcom/bcm2712.dtsi`**: BCM2712 の `sdhci1` (物理アドレス `0x1001100000`, GIC SPI 274) および `gio` ノードを定義。
- **`boards/raspberrypi/rpi_5/rpi_5_bcm2712.dts`**: `sdhci1` ノードを有効化し、子ノードとして `infineon,airoc-wifi` を登録。電源制御ピンとして `GPIO 28 (REG_ON)` を指定。

### 2.2. ドライバレイヤー
- **`drivers/sdhc/sdhc_bcm2712.c`**: BCM2712 向け SDHCI ホストコントローラドライバを新規実装（CMD0, CMD5, CMD3, CMD7, CMD52, CMD53 サポート、4-bit / 50MHz High-Speed）。
- **`drivers/wifi/infineon/airoc_whd_hal_sdio.c`**: 
  - CMD53 のブロックアライメント（64バイト境界の切り上げパディング）を実装。
  - バウンスバッファを 4096 バイトに拡張し、AArch64 64バイトキャッシュアライメント（`__attribute__((aligned(64)))`）を付与。
  - SDPCM ハードウェアタグ検証時の不要なログ氾濫を抑制。

### 2.3. サブシステム（SD / Sockets / HTTP）レイヤー
- **`subsys/sd/sd.c` & `sdio.c`**: SDIO カード初期化時に不要な SD メモリ用 CMD8 を回避し、ダイレクトに `sdio_card_init` を先行実行するよう最適化。
- **`subsys/net/lib/sockets/sockets_tls.c`**: TLS ハンドシェイク失敗時に Mbed TLS の詳細エラーコード（`-0x3a00` 等）をコンソールに出力する診断コードを追加。
- **`subsys/net/lib/shell/http_client.c`**: 接続先 IP、ポート、タイムアウト値、および詳細な `errno` を明示出力するように拡張。

### 2.4. Infineon HAL (`hal_infineon.patch`)
- AArch64 でのブレークポイント命令 `brk #0` への修正。
- CLM BLOB 未指定時の安全なダウンロードスキップ。
- 国コード設定失敗時のフォールバック処理。
- WHD スレッドのパケット受信ポーリング対応。
