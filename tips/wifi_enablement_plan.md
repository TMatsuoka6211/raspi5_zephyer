# Raspberry Pi 5 Zephyr Wi-Fi (SDIO) 有効化計画・作業実績報告書

本ドキュメントは、Zephyr RTOS においてデフォルトで非対応である Raspberry Pi 5 (BCM2712) の内蔵 Wi-Fi チップ (CYW43455) への SDIO 通信を確立し、Wi-Fi 機能を有効化するための一連の作業実績、直面した課題と解決の軌跡、および今後の作業ロードマップを包括的にまとめたものです。

---

## 1. プロジェクトの背景と目的

### 背景
- Zephyr RTOS の公式ツリーにおける Raspberry Pi 5 (Board Target: `rpi_5/bcm2712`) は、基本的なシリアル出力や基本的な周辺機能のみがサポートされており、SDIO バスおよび Wi-Fi チップ (`CYW43455`) によるワイヤレスネットワーク機能には一切対応していません。
- BCM2712 SoC は、外部 SD カードスロット用の `sdhost` コントローラとは別に、Wi-Fi チップに直結された独立した標準 SDHCI コントローラ (`sdhci1`) を持っていますが、このドライバも定義されていませんでした。

### 目的
- Raspberry Pi 5 本体のオンボード Wi-Fi チップへ SDIO バスを介した通信経路を確立し、最終的に Zephyr の Wi-Fi ドライバおよびネットワークスタックとバインドして、ワイヤレス通信を可能にします。

---

## 2. ハングアップ・ハードウェア構成の推測とリサーチ

Linux カーネルの DTS パッチや Broadcom/Raspberry Pi の公開ドキュメントから、以下の内部ハードウェア構成を推測・特定しました。

1. **SDIO ホストコントローラ (SDHCI1)**:
   - 物理レジスタベースアドレス: `0x1001100000` (CPU物理アドレス空間上、64bit表現)。   - レジスタ規格: 標準 SDHCI (Version 3.00/4.00 に準拠)。
2. **Wi-Fi チップ接続仕様**:
   - オンボードの CYW43455 コンボチップと SDIO 1ビット / 4ビット幅で直結。
3. **ピンアサインと電源制御**:
   - `WL_ON` (Wi-Fi 電源有効化ピン): BCM2712 の GPIO 28
   - `BT_ON` (Bluetooth 電源有効化ピン): BCM2712 の GPIO 29
   - `SDIO ピン` (CMD, CLK, DAT0-3): BCM2712 の GPIO 30〜35 (機能モード: `ALT3`)
   - ピンのプルアップ: 信号安定化および CMOS 入力の HIGH 維持のため、全ピンにプルアップ (`2`) が必須。

---

## 3. これまで実施した作業実績

Wi-Fi 有効化に向けて、デバイスツリー、ドライバ、カーネル設定、テストコードに至る全レイヤーで以下の実装・改修を行いました。

### 3.1. デバイスツリー (DTS) & バインディングの構築
- **DTS定義の追加**: [bcm2712.dtsi](file:///home/matsuoka/zephyr-workspace/zephyr/dts/arm64/broadcom/bcm2712.dtsi) に `sdhci1: mmc@1001100000` ノードを新設。割り込み (`GIC_SPI 274`) およびクロックを定義。
- **バインディングファイルの新規作成**: `dts/bindings/sdhc/brcm,bcm2712-sdhci.yaml` を作成し、新規ドライバが DTS から正しくノード定義とパラメータを取得できるように統合。
- **ボード固有の有効化**: `rpi_5_bcm2712.dts` で `sdhci1` ノードを `status = "okay";` に設定。

### 3.2. BCM2712 専用 SDHC ドライバの実装
- **新規ドライバの構築**: [sdhc_bcm2712.c](file:///home/matsuoka/zephyr-workspace/zephyr/drivers/sdhc/sdhc_bcm2712.c) を新規追加。
  - MMIO 領域のマッピングおよびレジスタ読み書き API の定義。
  - 基本コマンドの送受信 (`sdhc_bcm2712_request`)。
  - ベースクロック (200MHz) から分周による転送クロック設定 (`sdhc_bcm2712_set_clk`)。
  - バス幅の動的変更 (`sdhc_bcm2712_set_bus_width`)。
  - コマンドエラー・タイムアウト発生時のソフトリセットリカバリー処理 (`sdhc_bcm2712_recover_error`) の実装。

### 3.3. 動作確認のためのテスト構成の構築
- **テスト用サンプルの改修**: [main.c](file:///home/matsuoka/zephyr-workspace/zephyr/samples/hello_world/src/main.c) に、デバイス検出チェック (`device_is_ready`) および Zephyr 標準 SD スタックの初期化関数 (`sd_init`) の呼び出しを追加。
- **ビルド・デプロイ環境の構築**: WSL (Windows Subsystem for Linux) 上での west ビルド環境と、実機への SD カードマウント・デプロイ手順を確立。

---

## 4. 直面した課題と解決の軌跡

### 課題 1: MMU 仮想メモリ変換テーブルの枯渇 (`xlat tables low`)
- **事象**: ドライバが物理レジスタをマッピングしようとした際、変換テーブル領域が不足し、起動時に `xlat tables low: 10 of 12 in use` の警告やマッピングエラーが発生。
- **解決策**: [rpi_5_bcm2712_defconfig](file:///home/matsuoka/zephyr-workspace/zephyr/boards/raspberrypi/rpi_5/rpi_5_bcm2712_defconfig) に `CONFIG_MAX_XLAT_TABLES=24` を追加し、カーネルのページテーブルバッファを十分に拡張。

### 課題 2: EL3 (Secure Mode) のピンアクセス制御による SError クラッシュ
- **事象**: `main.c` やドライバから、Wi-Fi 給電（WL_ON/BT_ON = GPIO 28, 29）や SDIO ピンの ALT3 設定のために GPIO コントローラ (`0x107d508500`) や pinctrl コントローラ (`0x107d504100`) へ直接物理アクセスした瞬間、CPU が `SError` (セキュアバス例外) を検知してシステムがハングアップ。
- **解決策**: 
  - Raspberry Pi 5 の本体ブートローダー (ファームウェア) が、起動時にすでに `WL_ON = HIGH` および SDIO ピンを `ALT3` + プルアップに設定完了している状態であることを検証・発見。
  - OS 側から危険な pinctrl / GPIO 直接レジスタ書き込み処理を完全に除外・クリーンアップすることで、SError を 100% 回避。

### 課題 3: CMD8 のタイムアウト判定ミスマッチによるスタックのアボート
- **事象**: メモリカード用のコマンド `CMD8` を送った際、相手は SDIO 専用カード（Wi-Fi）であるため応答がなくタイムアウト (`CMD_TIMEOUT_ERR`) が発生。しかし、ドライバが一律で入出力エラー `-EIO` を返していたため、スタックが「致命的なハードエラー」とみなして即座に初期化を中断。
- **解決策**: タイムアウト検知時に `-ETIMEDOUT` を返すようドライバを修正。これによりスタック側が「CMD8 非対応の SDIO/レガシーカード」と正しく判定し、次の初期化フェーズへ流すことに成功。

---

## 5. 現在の状況と直近の課題分析

### 現在のステータス (直近のログ)
```text
[00:00:01.050,000] Sending CMD8 -> 応答タイムアウト (Command error: 0xc)
[00:00:01.051,000] SDHCI error recovery reset (ソフトリセット実行)
[00:00:01.054,000] SD CMD8 failed with error -116 (-ETIMEDOUT) -> 正常に次のフェーズへ
[00:00:01.054,000] Sending CMD0 (arg: 0x0) -> タイムアウト (INT_STATUS: 0x100, PRESENT_STATE: 0xf0001)
[00:00:01.254,000] Card error on CMD0 (CMD0タイムアウト)
[00:00:01.254,000] Card does not support CMD8, assuming legacy card
[00:00:01.254,000] Sending CMD0 (arg: 0x0) -> Command inhibit timeout (バスロック)
```

### 【現在の課題分析】ソフトリセット後のホストコントローラのフリーズ
1. **リセット完了確認の不足**:
   - `CMD8` のタイムアウト後、ドライバは `sdhc_bcm2712_recover_error` を呼び出して `SDHCI_SOFTWARE_RESET` レジスタの `CMD` および `DATA` リセットビットをセットしています。
   - しかし、ハードウェア側でリセットが内部完了してビットが自動クリア（`0` に戻る）されるのを待つポーリング処理（Busy wait）が不足しているため、リセット処理の実行途中で直ちに次の `CMD0` を送信してしまい、ホストコントローラがフリーズ状態（Command Inhibit ビットが戻らない）に陥っています。

---

## 6. 今後の作業計画 (Action Plan)

### フェーズ 1: リセット・リカバリー処理の堅牢化 (短期目標)
- **タスク**: `sdhc_bcm2712_recover_error` 関数内に、リセットビットの自動クリアを安全にポーリング監視する処理を追加し、タイムアウト直後のリセットフリーズを解消します。
- **検証内容**: `CMD8` のタイムアウト発生後、後続の `CMD0`（あるいは `CMD5`）がタイムアウトすることなく送信され、バスがロックしなくなることを検証します。

### フェーズ 2: SDIO 専用初期化フローの優先設定 (中期目標)
- **タスク**: `prj.conf` にて `CONFIG_SDIO_STACK=y` を明示的に設定し、SDMMC (メモリ) スタックの初期化フローをバイパスして、SDIO 専用の `CMD5` (IO_SEND_OP_COND) の送受信からフローを開始できるように最適化します。
- **検証内容**: コントローラから CYW43455 に対して `CMD5` が送信され、Wi-Fi チップの動作電圧および能力情報 (OCR) を取得できることを確認します。

### フェーズ 3: CYW43455 接続の確立と通信テスト (最終目標)
- **タスク**: OCR レスポンスの解析、RCA (相対カードアドレス) の取得、バス幅の 4ビットへの切り替え、および Zephyr の CYW43455 Wi-Fi ドライバとのバインドを実施。
- **検証内容**: L2 ネットワークインターフェースを有効化し、アクセスポイントのスキャンテストを実行します。
