# WiFi 有効化パッチ適用手順および差分解説書

本ドキュメントは、`/home/matsuoka/workspace/raspi5_zephyer/wifi_enablement/wifi_changes.patch` を Zephyr ワークスペースに適用する手順、および本パッチによって変更される各モジュール・ファイルの変更詳細をまとめたものです。

---

## 1. パッチファイルの適用手順 (How to Apply)

パッチファイル `wifi_changes.patch` には、新規作成したドライバファイルやバインディング定義も含めて、すべての変更が一括して収録されています。

以下の手順で Zephyr ワークスペースに適用してください。

### 手順 1: 適用前の状態確認 (ドライバ競合のクリーンアップ)
もしワークスペース上にすでに修正中・テスト中のファイルが存在する場合は、差分が衝突（Conflict）するのを避けるため、事前に現在の状態を退避またはリセットしてください。
*(※ 既存の作業ファイルを完全に消去して適用し直す場合は、以下のコマンドを実行します)*
```bash
cd /home/matsuoka/zephyr-workspace/zephyr
git reset --hard HEAD
git clean -fd
```

### 手順 2: パッチのテスト適用 (ドライラン)
実際にファイルを書き換える前に、パッチがエラーなく適用できるか確認します。
```bash
git apply --check /home/matsuoka/workspace/raspi5_zephyer/wifi_enablement/wifi_changes.patch
```
*(※ 何も出力されなければ正常に適用可能です)*

### 手順 3: パッチの適用
実際に変更を適用します。
```bash
git apply /home/matsuoka/workspace/raspi5_zephyer/wifi_enablement/wifi_changes.patch
```

### 手順 4: ビルドと書き込み
適用後、通常通り west ビルドを実行し、バイナリを SD カードに格納して実行してください。
```bash
source ~/zephyr-env/bin/activate
west build -p always -b rpi_5 samples/hello_world
```

---

## 2. 変更差分の詳細説明 (Detailed Changes)

本パッチで変更・追加されるファイルと、それぞれの役割および変更理由は以下の通りです。

### 2.1. デバイスツリー (DTS) レイヤー
#### 📄 `dts/arm64/broadcom/bcm2712.dtsi` (変更)
- **変更内容**: `sdhci1` (物理ベース `0x1001100000`) および `gio` (GPIO1コントローラ) ノードを新規登録しました。
- **役割**: BCM2712 SoC 内に存在する Wi-Fi 直結用の標準 SDHCI コントローラの物理アドレス、割り込み番号 (`GIC_SPI 274`)、およびクロックを Zephyr カーネルに認識させます。

#### 📄 `boards/raspberrypi/rpi_5/rpi_5_bcm2712.dts` (変更)
- **変更内容**: 新設した `sdhci1` および `gio` を `status = "okay";` に設定して有効化し、`sdhci1` の下に cyw43455 デバイスを登録しました。

#### 📄 `dts/bindings/sdhc/brcm,bcm2712-sdhci.yaml` (新規追加)
- **役割**: 新規ドライバ用のデバイスツリーバインディング（定義スキーマ）です。DTS上の `compatible = "brcm,bcm2712-sdhci";` ノードが、後述の C ドライバコードとプロパティベースでバインドできるようにするための設定メタデータです。

---

### 2.2. ドライバレイヤー (SDHCIコントローラ)
#### 📄 `drivers/sdhc/sdhc_bcm2712.c` (新規追加)
- **役割**: BCM2712 独自の SDHCI コントローラ制御ドライバです。
- **実装された主な制御ロジック**:
  - **レジスタマッピング**: `DEVICE_MMIO` マクロによる物理レジスタ空間の仮想マッピングと読み書き。
  - **分周クロック設定**: ベースクロック 200MHz からターゲット周波数 (400kHz 等) への分周比計算と設定。
  - **タイムアウト検出最適化**: コマンドの応答タイムアウト時にスタックへ **`-ETIMEDOUT`** を返却するように制御。
  - **エラー復旧の安定化**: タイムアウト検知時にホストコントローラをソフトリセットする `sdhc_bcm2712_recover_error` を実装。コマンドラインがロックするのを防ぐため、リカバリ処理内の不要な `set_clk` の一時停止・再開処理を削除しました。
  - **ノンリムーバブルSDIO対応**: 物理スロットを持たない直結SDIO WiFiチップ向けに、着脱式スロットで用いられる `SDIO_CFG_SD_PIN_SEL_SD` ビットの操作を無効化（常時接続状態の維持）。
  - **電源制御デバッグログ**: 電源書き込み後の安定化を検証するため、レジスタ書き込み後の値を読み戻すログ出力を追加。

#### 📄 `drivers/sdhc/CMakeLists.txt` / `Kconfig` / `Kconfig.bcm2712` (変更および新規追加)
- **役割**: 上記の `sdhc_bcm2712.c` を Zephyr のビルドシステムおよび Kconfig メニューシステムに登録し、`CONFIG_SDHC_BCM2712=y` が有効な際にコンパイル対象となるように統合しました。

---

### 2.3. SDスタックレイヤー
#### 📄 `subsys/sd/sd.c` (変更)
- **役割**: Zephyr 標準の SD スタック初期化フロー制御。
- **変更内容**: 
  - **SDIOファースト初期化順序の導入**: CYW43455 のようなSDIO専用カードは、SDメモリーカード用コマンドである `CMD8` (電圧確認) を受け取ると、内部状態遷移がクラッシュし、以降 `CMD0` などのあらゆるコマンドに応答しなくなります。これを防ぐため、初期化シーケンスの先頭で **`CMD0` 後に直接 `CMD5` (`sdio_card_init`) を実行する**ように順序を再構成しました。SDIOとしての初期化が失敗した場合のみ、安全に従来の `CMD8` から始まるSDMMCシーケンスにフォールバックします。
  - **CMD8 タイムアウト時の即時リターン**: `CMD8` タイムアウト時に無駄なリトライループを回さず、即座に `-ETIMEDOUT` を返してコントローラをリセットし、レガシーカード検出へ遷移させます。

---

### 2.4. システム設定・アプリケーションレイヤー
#### 📄 `boards/raspberrypi/rpi_5/rpi_5_bcm2712_defconfig` (変更)
- **変更内容**: `CONFIG_MAX_XLAT_TABLES=24` を追加。
- **理由**: ドライバが物理 MMIO 領域を安全にページテーブルにマップするために必要な、仮想変換テーブル領域（バッファテーブル数）を拡張し、枯渇ハングを防止します。

#### 📄 `samples/hello_world/prj.conf` (変更)
- **変更内容**: SDHC ホストインターフェース (`CONFIG_SDHC=y`) および SD/SDIO カードスタック (`CONFIG_SDIO_STACK=y`) の有効化に加え、**Zephyr Shell サブシステム**（`CONFIG_SHELL=y` ほか）を有効化しました。
- **ポーリング受信モードの適用**: RPi5 の UART 割り込み問題に対応するため、シリアル受信を割り込み駆動ではなく一定周期でレジスタを直接監視するポーリング方式（`CONFIG_SHELL_BACKEND_SERIAL_API_POLLING=y`）で動作させ、シェル入力が常に受け付けられるように調整しました。

#### 📄 `samples/hello_world/src/main.c` (変更)
- **変更内容**: 
  - **BCM2712 D0 ピン多重化（Pin Muxing）の修正**: SDIOピン（GPIO 30-35）のMux値を D0 ステッピング用の **`1` (sd2)** に設定し、適切なプルアップ設定を適用しました。
  - **WiFiチップ起動制御と自動リセット**: コントローラの初期化（`sd_init`）時に発生するホスト側電源リセットと同期してWiFiチップも綺麗に再起動するよう、`WL_ON`（GPIO 28）を一度LOWに落としてからHIGHに戻す自動パワーサイクルロジックを統合しました。
  - **対話型 `sdio` シェルコマンドの登録**: 以下のサブコマンドを登録し、シリアルコンソールから動的に呼び出せるようにしました。
    - `sdio pinctrl` (ピン多重化の設定)
    - `sdio power <on/off>` (WL_ON GPIOの手動制御)
    - `sdio init` (WiFi電源制御＆D0ピン設定を実行後の、SDIOスタック初期化)
    - `sdio read <func> <addr>` (CMD52による1バイトレジスタ読み込み)
    - `sdio write <func> <addr> <val>` (CMD52による1バイトレジスタ書き込み)
