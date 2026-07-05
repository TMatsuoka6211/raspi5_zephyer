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
- **変更内容**: `sdhci1` (物理ベース `0x1001100000`) ノードを新規登録しました。
- **役割**: BCM2712 SoC 内に存在する Wi-Fi 直結用の標準 SDHCI コントローラの物理アドレス、割り込み番号 (`GIC_SPI 274`)、およびクロックを Zephyr カーネルに認識させます。

#### 📄 `boards/raspberrypi/rpi_5/rpi_5_bcm2712.dts` (変更)
- **変更内容**: 新設した `sdhci1` を `status = "okay";` に設定して有効化しました。

#### 📄 `dts/bindings/sdhc/brcm,bcm2712-sdhci.yaml` (新規追加)
- **役割**: 新規ドライバ用のデバイスツリーバインディング（定義スキーマ）です。DTS上の `compatible = "brcm,bcm2712-sdhci";` ノードが、後述の C ドライバコードとプロパティベースでバインドできるようにするための設定メタデータです。

---

### 2.2. ドライバレイヤー (SDHCIコントローラ)
#### 📄 `drivers/sdhc/sdhc_bcm2712.c` (新規追加)
- **役割**: BCM2712 独自の SDHCI コントローラ制御ドライバです。
- **実装された主な制御ロジック**:
  - **レジスタマッピング**: `DEVICE_MMIO` マクロによる物理レジスタ空間の仮想マッピングと読み書き。
  - **分周クロック設定**: ベースクロック 200MHz からターゲット周波数 (400kHz 等) への分周比計算と設定。
  - **タイムアウト検出最適化**: コマンドの応答タイムアウト時にスタックへ **`-ETIMEDOUT`** を返却するように制御（SDIO 専用カードの検出をスタック側で迂回させるために必須の変更）。
  - **エラー復旧**: タイムアウト検知時にホストコントローラをソフトリセットしてバスフリーズを防ぐ `sdhc_bcm2712_recover_error` の実装。

#### 📄 `drivers/sdhc/CMakeLists.txt` / `Kconfig` / `Kconfig.bcm2712` (変更および新規追加)
- **役割**: 上記の `sdhc_bcm2712.c` を Zephyr のビルドシステムおよび Kconfig メニューシステムに登録し、`CONFIG_SDHC_BCM2712=y` が有効な際にコンパイル対象となるように統合しました。

---

### 2.3. システム設定・アプリケーションレイヤー
#### 📄 `boards/raspberrypi/rpi_5/rpi_5_bcm2712_defconfig` (変更)
- **変更内容**: `CONFIG_MAX_XLAT_TABLES=24` を追加。
- **理由**: ドライバが物理 MMIO 領域を安全にページテーブルにマップするために必要な、仮想変換テーブル領域（バッファテーブル数）を拡張し、枯渇ハングを防止します。

#### 📄 `samples/hello_world/prj.conf` (変更)
- **変更内容**: SDHC ホストインターフェース (`CONFIG_SDHC=y`) および SD バス・カードスタック (`CONFIG_SD_STACK=y`) を有効化しました。また、ログレベルを `DBG` に引き上げました。

#### 📄 `samples/hello_world/src/main.c` (変更)
- **変更内容**: `sdhci1` デバイスがレディ状態になったのち、Zephyr 標準の SD カードスタック初期化 API `sd_init()` を呼び出して、通信相手（CYW43455）の検出を開始する検証ロジックを追加しました。
