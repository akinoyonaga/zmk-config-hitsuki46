# hitsuki46 ZMK設定

## 概要

hitsuki46は46キーの分割キーボードで、両手にトラックボールを搭載可能な設計です。

## 主な機能

- **46キー分割レイアウト** (左右各23キー)
- **デュアルトラックボール対応**
  - 左手側: 常にスクロール操作
  - 右手側: カーソル操作 + レイヤー5でスクロール操作
- **ニッケル水素電池対応** (カスタム電圧管理)
- **Bluetooth接続性能向上** (+8dBm出力)
- **WS2812 LEDステータス表示**
- **ZMK Studio対応**
- **Zephyr 4.1対応**

## トラックボール設定

### 閾値付き Auto Mouse Layer

右トラックボールの相対移動量が 200 ms 以内に累計 15 を超えた場合だけ、
Auto Mouse Layer（レイヤー 5）を有効にします。微小なセンサー入力ではレイヤーは
切り替わりません。有効化後は最後の移動から 10 秒でレイヤー 5 を解除します。

設定値は `boards/shields/hitsuki46/hitsuki46_R.overlay` の `aml_threshold` と
`&aml_threshold 5 10000` で変更できます。processor の実装はこのリポジトリの
`src/input_processor_temp_layer_threshold.c` に含まれているため、外部モジュールへの
依存はありません。

### センサー仕様
- **センサー**: PixArt PMW3610 (badjeff/zmk-pmw3610-driver使用)
- **CPI**: 800
- **通信**: SPI (2MHz)
- **ピン配置** (両手共通):
  - SPI0: SCK=P0.5, MOSI/MISO=P0.4, CS=P0.9
  - IRQ: P1.1

### モード設定 (コンパイル時切り替え)

左右のトラックボールの動作モードは、`hitsuki46_R.overlay`ファイル先頭の`#define`で設定できます：

```c
//=============================================================================
// Trackball Mode Configuration
// 0 = CURSOR mode (mouse cursor movement)
// 1 = SCROLL mode (scroll operation)
//=============================================================================
#define LEFT_TRACKBALL_MODE  1   // 左トラックボール (0/1)
#define RIGHT_TRACKBALL_MODE 0   // 右トラックボール (0/1)
//=============================================================================
```

- **値を`0`に設定**: カーソルモード（マウスカーソル操作 + Layer 5でスクロール）
- **値を`1`に設定**: スクロールモード（常にスクロール操作）

変更後はファームウェアを再ビルドしてください。

### 左手側 (デフォルト: カーソルモード)
- スクロールモード時: センサーを動かすとスクロール操作
- カーソルモード時: センサーを動かすと自動的にマウスレイヤー(Layer 1)へ移行

### 右手側 (デフォルト: カーソルモード)
- **カーソルモード**: センサーを動かすと自動的にマウスレイヤー(Layer 1)へ移行
  - タイムアウト: 700ms (動きが止まるとデフォルトレイヤーに戻る)
- **スクロールモード**: Layer 5でセンサーを動かすとスクロール操作

### XY Clipper (斜め入力防止)

スクロール操作時に斜め方向の入力を防止するため、xy_clipperを使用しています。
しきい値(threshold=20)以下の弱い軸の入力はカットされ、最も強い軸（上下または左右）のみが検出されます。

## レイヤー構成

### Layer 0 (Default)
QWERTY配列のベースレイヤー

### Layer 1 (MOUSE)
右手トラックボールの自動マウスレイヤー
- マウスボタン (MB1/MB2/MB3)
- Alt+Left/Right (ブラウザ履歴)

### Layer 2 (FUNCTION)
記号キーとファンクションキー
- 特殊記号 (!@#$%^&*())
- F1-F12
- 矢印キー

### Layer 3 (NUM)
テンキーと記号
- 0-9の数字
- 演算子 (+, -, *, /, =)
- 括弧記号

### Layer 4 (ARROW)
矢印キー専用レイヤー

### Layer 5 (SCROLL)
右手トラックボールのスクロールレイヤー
- `-` キーホールド時に有効

### Layer 6 (Bluetooth)
Bluetooth設定レイヤー
- BT_SEL 0-4: プロファイル選択
- BT_CLR: プロファイルクリア
- BT_CLR_ALL: 全プロファイルクリア
- Bootloader起動

## WS2812 LED設定

[zmk-ws2812-driver](https://github.com/gohanda11/zmk-ws2812-driver)による視覚的なステータス表示

### レイヤー表示
レイヤー切り替え時に白色で点滅。アクティブレイヤーを色で表示 (右手側のみ):
- Layer 0 (Default): 消灯
- Layer 1 (MOUSE): 赤
- Layer 2 (FUNCTION): 緑
- Layer 3 (NUM): 黄
- Layer 4 (ARROW): 青
- Layer 5 (SCROLL): マゼンタ
- Layer 6 (Bluetooth): シアン

### バッテリー表示
電源ON時のバッテリーレベル表示:
- 高 (80%以上): 緑
- 中 (20-80%): 黄
- 低 (5-20%): オレンジ
- 危険 (5%未満): 赤

### 接続状態表示
- USB接続: 緑
- BLE接続済み: 青
- アドバタイジング中: シアン
- 切断: 赤

### ハードウェア構成
- LED: WS2812B 1個/側
- 制御ピン: MOSI=P0.19, SCK=P0.20
- 電源制御: P0.15 (P-ch MOSFET)

## バッテリー管理

ニッケル水素電池用のカスタム電圧管理設定:
- 分圧回路: R2=1MΩ, R3=470kΩ (比率 0.32)
- MIN: 1100mV (0%) → ADC: 352mV
- MAX: 1400mV (100%) → ADC: 448mV
- スリープタイムアウト: 15分

## ファームウェアビルド

このリポジトリはGitHub Actionsで自動ビルドされます。
`.github/workflows/build.yml`でビルド設定を確認できます。

## 外部モジュール

- [zmk](https://github.com/zmkfirmware/zmk) - ベースファームウェア
- [zmk-pmw3610-driver](https://github.com/badjeff/zmk-pmw3610-driver) - トラックボールドライバー
- [zmk-ws2812-driver](https://github.com/gohanda11/zmk-ws2812-driver) - LEDドライバー
- [zmk-feature-non-lipo-battery-management](https://github.com/sekigon-gonnoc/zmk-feature-non-lipo-battery-management) - 非LiPoバッテリー管理
- [zmk-feature-xy_clipper](https://github.com/iwk7273/zmk-feature-xy_clipper) - 斜め入力防止フィルター

## キーマップ

![キーマップ](keymap-drawer/hitsuki46.svg)
