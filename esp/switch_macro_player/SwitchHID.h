#pragma once

/**
 * SwitchHID.h  –  Bluetooth Classic HID 版
 *
 * JoyConDroid と同じ接続方式（android.bluetooth.BluetoothHidDevice 相当）を
 * ESP32 (Classic BT 対応版) で再現します。
 *
 * ⚠️ 必要ハードウェア: ESP32 無印 (例: M5Atom) のみ対応。
 *    ESP32-S3 (M5AtomS3) は Classic Bluetooth 非対応のため使用不可。
 *
 * 依存ライブラリ: ESP-IDF (Arduino ESP32 2.x に同梱)
 *   - esp_hidd_api.h   (Classic BT HID Device プロファイル)
 *   - esp_gap_bt_api.h (Classic BT GAP)
 */

#include <Arduino.h>
#include <esp_hidd_api.h>   // Classic BT HID Device profile
#include <esp_gap_bt_api.h> // Classic BT GAP
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_bt.h>

// ============================================================
//  ボタン定義  (JoyConDroid の 0x30 フルレポート形式に準拠)
// ============================================================

/**
 * @brief Switchプロコン ボタン定義
 *
 * setButtonMask() に渡すビットマスクの定義。
 * 内部では JoyConDroid 互換の 0x30 フルレポートへ変換されます。
 */
enum class Button : uint32_t {
    // 右側ボタン (Report Byte1)
    Y       = (1UL << 0),
    X       = (1UL << 1),
    B       = (1UL << 2),
    A       = (1UL << 3),
    R       = (1UL << 6),
    ZR      = (1UL << 7),
    // 共通ボタン (Report Byte2)
    MINUS   = (1UL << 8),
    PLUS    = (1UL << 9),
    RCLICK  = (1UL << 10),
    LCLICK  = (1UL << 11),
    HOME    = (1UL << 12),
    CAPTURE = (1UL << 13),
    // 左側ボタン (Report Byte3)
    L       = (1UL << 22),
    ZL      = (1UL << 23),
    NONE    = 0
};

inline Button operator|(Button a, Button b) {
    return static_cast<Button>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline Button operator&(Button a, Button b) {
    return static_cast<Button>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline Button &operator|=(Button &a, Button b) { a = a | b; return a; }

/**
 * @brief 十字キー (D-Pad) 定義  –  JoyConDroid の Hat 形式と同一
 */
enum class Hat : uint8_t {
    TOP          = 0x00,
    TOP_RIGHT    = 0x01,
    RIGHT        = 0x02,
    BOTTOM_RIGHT = 0x03,
    BOTTOM       = 0x04,
    BOTTOM_LEFT  = 0x05,
    LEFT         = 0x06,
    TOP_LEFT     = 0x07,
    CENTER       = 0x08  // ニュートラル
};

// ============================================================
//  SwitchControllerHID クラス
//  (JoyConDroid の BluetoothHidDevice + SwitchController 相当)
// ============================================================

class SwitchControllerHID {
public:
    SwitchControllerHID();

    /**
     * @brief Classic BT HID プロファイルを初期化してアドバタイズを開始する
     *
     * 事前に initBluetoothDualMode() で Bluetooth を BTDM モードで
     * 起動しておく必要があります。
     */
    void begin();
    void end() {}

    // --- ボタン / スティック 操作 ---
    void press(Button b);
    void release(Button b);

    /**
     * @brief ボタンビットマスクを一括設定する
     *
     * 引数の bit 定義は Button enum と同一です。
     * 内部で 0x30 フルレポートの 3 バイトボタン表現に変換します。
     */
    void setButtonMask(uint32_t mask);

    /** 全入力をニュートラルに戻す */
    void releaseAll();

    /** 十字キーを設定する (Hat enum 使用) */
    void setHat(Hat h);

    /** スティック値設定 (-1.0f 〜 1.0f) */
    void setLeftStick(float x, float y);
    void setRightStick(float x, float y);

    /** スティック値設定 (0 〜 255, 128 = センター) */
    void setLeftStickRaw(uint8_t x, uint8_t y);
    void setRightStickRaw(uint8_t x, uint8_t y);

    /**
     * @brief 現在のレポートを BT HID 経由で Switch に送信する
     * @return true  送信成功
     * @return false 未接続または送信失敗
     */
    bool send();

    /** Switch との BT HID 接続状態を返す */
    bool isConnected() const { return _connected; }

    // ----------------------------------------------------------
    //  内部コールバック  (esp_hidd_api から呼ばれる、直接呼ばない)
    // ----------------------------------------------------------
    static void onHiddEvent(esp_hidd_cb_event_t event,
                            esp_hidd_cb_param_t *param);
    static void onGapEvent(esp_bt_gap_cb_event_t event,
                           esp_bt_gap_cb_param_t *param);

private:
    // --- Button enum ビット空間での現在状態 (press/release 用) ---
    uint32_t _currentButtonMask = 0;

    // --- 0x30 フルレポートの生バイト ---
    // Byte0: タイマー
    // Byte1: 右ボタン  (Y X B A SR SL R ZR)
    // Byte2: 共通ボタン (- + RS LS Home Cap -- --)
    // Byte3: 左ボタン  (Down Up Right Left SR SL L ZL)
    // Byte4-6: 左スティック (12bit×2 packed)
    // Byte7-9: 右スティック (12bit×2 packed)
    // Byte10: バイブレーション
    uint8_t _btBtn[3];    // [0]=右, [1]=共通, [2]=左
    uint16_t _lx, _ly;   // 12-bit stick (0-4095, center=2048)
    uint16_t _rx, _ry;
    uint8_t _timer;

    // --- 接続状態 ---
    static bool _connected;
    static esp_bd_addr_t _remoteAddr;

    // --- サブコマンド処理 (Switch → コントローラー) ---
    static void handleSubcommand(const uint8_t *data, uint16_t len);

    // --- レポート送信ヘルパー ---
    bool sendFullReport();
    static bool sendSubcmdAck(uint8_t subcmd, const uint8_t *reply,
                              uint8_t replyLen);

    // 8bit (0-255) → 12bit (0-4095) 変換
    static constexpr uint16_t to12bit(uint8_t v) {
        return static_cast<uint16_t>(v) << 4;
    }
};

// グローバルインスタンス (switch_macro_player.ino から使用)
extern SwitchControllerHID controller;

// ============================================================
//  Bluetooth 初期化ヘルパー
//  (NimBLE の代わりに BlueDroid BTDM を使うために必要)
// ============================================================

/**
 * @brief BT コントローラーを Classic + BLE デュアルモードで初期化する
 *
 * BLEDevice::init() と Classic BT HID を共存させるために、
 * どちらよりも先に呼び出してください。
 *
 * 内部で以下を実行します:
 *   1. esp_bt_controller_init (BTDM モード)
 *   2. esp_bluedroid_init / enable
 *
 * BLEDevice::init() は後から呼んでも二重初期化を行わないため安全です。
 */
esp_err_t initBluetoothDualMode();
