/**
 * SwitchHID.cpp  –  Bluetooth Classic HID 実装
 *
 * JoyConDroid が行っていること (BluetoothHidDevice.registerApp /
 * sendReport) を ESP-IDF の esp_hidd_api で再現します。
 *
 * プロトコル概要:
 *  - SDP 登録: subclass=0x08, name="Wireless Gamepad", provider="Nintendo"
 *    → JoyConDroid の BluetoothHidDeviceAppSdpSettings と同一
 *  - HID 記述子: JoyConDroid の DESCRIPTOR 定数と同一バイト列
 *  - 入力レポート 0x30 (フルモード): ボタン3バイト + 12bit スティック×2
 *  - サブコマンド応答 (0x21): Switch からの初期化コマンドに最低限返答
 */

#include "SwitchHID.h"
#include <cstring>
#include <esp_log.h>

static const char *TAG = "SwitchHID";

// ============================================================
//  HID 記述子  –  JoyConDroid の DESCRIPTOR 定数と完全一致
//  (BluetoothHidDeviceAppSdpSettings に渡される hexStringToByteArray と同値)
// ============================================================
static const uint8_t JCD_HID_DESCRIPTOR[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x06, 0x01, 0xff, 0x85, 0x21, 0x09, 0x21, 0x75, 0x08, 0x95,
    0x30, 0x81, 0x02, 0x85, 0x30, 0x09, 0x30, 0x75, 0x08, 0x95, 0x30, 0x81, 0x02, 0x85, 0x31, 0x09,
    0x31, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02, 0x85, 0x32, 0x09, 0x32, 0x75, 0x08, 0x96, 0x69,
    0x01, 0x81, 0x02, 0x85, 0x33, 0x09, 0x33, 0x75, 0x08, 0x96, 0x69, 0x01, 0x81, 0x02, 0x85, 0x3f,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x05, 0x09,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x16, 0x00, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0x06, 0x01,
    0xff, 0x85, 0x01, 0x09, 0x01, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x10, 0x09, 0x10, 0x75,
    0x08, 0x95, 0x30, 0x91, 0x02, 0x85, 0x11, 0x09, 0x11, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0x85,
    0x12, 0x09, 0x12, 0x75, 0x08, 0x95, 0x30, 0x91, 0x02, 0xc0
};

// ============================================================
//  QoS パラメータ  –  JoyConDroid の BluetoothHidDeviceAppQosSettings と一致
//  (SERVICE_GUARANTEED, tokenRate=21720, bucketSize=362, ...)
// ============================================================
static esp_hidd_qos_param_t s_qos = {
    .service_type      = ESP_HID_TRANS_QOS_GUARANTEED,
    .token_rate        = 21720,
    .token_bucket_size = 362,
    .peak_bandwidth    = 21720,
    .access_latency    = 16667,
    .delay_variation   = 16667,
};

// ============================================================
//  静的メンバー初期化
// ============================================================
bool          SwitchControllerHID::_connected  = false;
esp_bd_addr_t SwitchControllerHID::_remoteAddr = {};

// グローバルインスタンス
SwitchControllerHID controller;

// ============================================================
//  SPI フラッシュ スタブデータ (0x10 サブコマンド応答用)
//  Switch はペアリング時に特定アドレスを読む。ゼロ埋めで問題なし。
// ============================================================
static uint8_t s_spiStub[0x1D] = {};

// ============================================================
//  コンストラクタ
// ============================================================
SwitchControllerHID::SwitchControllerHID()
    : _lx(2048), _ly(2048), _rx(2048), _ry(2048), _timer(0)
{
    memset(_btBtn, 0, sizeof(_btBtn));
}

// ============================================================
//  begin()  –  JoyConDroid の BluetoothHidDevice.registerApp() 相当
// ============================================================
void SwitchControllerHID::begin() {
    // --- SDP アプリ設定 (JoyConDroid の BluetoothHidDeviceAppSdpSettings と一致) ---
    esp_hidd_app_param_t appParam;
    appParam.name         = "Wireless Gamepad";   // JoyConDroid: HID_NAME
    appParam.description  = "Gamepad";            // JoyConDroid: HID_DESCRIPTION
    appParam.provider     = "Nintendo";           // JoyConDroid: HID_PROVIDER
    appParam.subclass     = 0x08;                 // JoyConDroid: SUBCLASS (Gamepad)
    appParam.desc_list     = const_cast<uint8_t *>(JCD_HID_DESCRIPTOR);
    appParam.desc_list_len = sizeof(JCD_HID_DESCRIPTOR);

    // HID コールバック登録 (JoyConDroid の BluetoothHidDevice.Callback 相当)
    ESP_ERROR_CHECK(esp_hidd_register_callbacks(SwitchControllerHID::onHiddEvent));
    // HID プロファイル開始
    ESP_ERROR_CHECK(esp_hidd_profile_init());

    // Classic BT GAP 設定
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(SwitchControllerHID::onGapEvent));
    // デバイス名 = Switch が認識する "Pro Controller"
    esp_bt_dev_set_device_name("Pro Controller");
    // ペアリング待機のためスキャン可能にする (JoyConDroid: startHidDeviceDiscovery 相当)
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ESP_LOGI(TAG, "Bluetooth HID initialized. Waiting for Nintendo Switch...");
}

// ============================================================
//  ボタン / スティック 操作
// ============================================================

void SwitchControllerHID::press(Button b) {
    // 現在の内部状態を Button enum ビット空間に逆変換してから OR して再設定
    uint32_t cur = _currentButtonMask;
    cur |= static_cast<uint32_t>(b);
    _currentButtonMask = cur;
    setButtonMask(cur);
}

void SwitchControllerHID::release(Button b) {
    uint32_t cur = _currentButtonMask;
    cur &= ~static_cast<uint32_t>(b);
    _currentButtonMask = cur;
    setButtonMask(cur);
}

/**
 * ボタンビットマスクを 0x30 レポートの 3 バイト形式に変換して保存する。
 *
 * 入力ビット定義 (Button enum):
 *   bit0=Y  bit1=X  bit2=B  bit3=A  bit6=R  bit7=ZR
 *   bit8=-  bit9=+  bit10=RS bit11=LS bit12=Home bit13=Cap
 *   bit22=L  bit23=ZL
 *
 * 0x30 レポート Byte1 (右ボタン):
 *   bit0=Y, bit1=X, bit2=B, bit3=A, bit4=SR, bit5=SL, bit6=R, bit7=ZR
 * 0x30 レポート Byte2 (共通):
 *   bit0=-, bit1=+, bit2=RS, bit3=LS, bit4=Home, bit5=Cap
 * 0x30 レポート Byte3 (左ボタン):
 *   bit0=Down, bit1=Up, bit2=Right, bit3=Left, bit4=SR, bit5=SL, bit6=L, bit7=ZL
 *   (D-Pad bits は setHat() が担当するので、ここでは L/ZL のみ変換)
 */
void SwitchControllerHID::setButtonMask(uint32_t mask) {
    // Byte1: 右ボタン
    uint8_t b1 = 0;
    if (mask & static_cast<uint32_t>(Button::Y))       b1 |= 0x01;
    if (mask & static_cast<uint32_t>(Button::X))       b1 |= 0x02;
    if (mask & static_cast<uint32_t>(Button::B))       b1 |= 0x04;
    if (mask & static_cast<uint32_t>(Button::A))       b1 |= 0x08;
    if (mask & static_cast<uint32_t>(Button::R))       b1 |= 0x40;
    if (mask & static_cast<uint32_t>(Button::ZR))      b1 |= 0x80;
    _btBtn[0] = b1;

    // Byte2: 共通ボタン
    uint8_t b2 = 0;
    if (mask & static_cast<uint32_t>(Button::MINUS))   b2 |= 0x01;
    if (mask & static_cast<uint32_t>(Button::PLUS))    b2 |= 0x02;
    if (mask & static_cast<uint32_t>(Button::RCLICK))  b2 |= 0x04;
    if (mask & static_cast<uint32_t>(Button::LCLICK))  b2 |= 0x08;
    if (mask & static_cast<uint32_t>(Button::HOME))    b2 |= 0x10;
    if (mask & static_cast<uint32_t>(Button::CAPTURE)) b2 |= 0x20;
    _btBtn[1] = b2;

    // Byte3: 左ボタン (L/ZL のみ; D-Pad は setHat() が管理)
    uint8_t b3 = _btBtn[2] & 0x0F; // D-Pad ビット (下位4bit) を保持
    if (mask & static_cast<uint32_t>(Button::L))  b3 |= 0x40;
    if (mask & static_cast<uint32_t>(Button::ZL)) b3 |= 0x80;
    _btBtn[2] = b3;
}

void SwitchControllerHID::releaseAll() {
    _currentButtonMask = 0;
    memset(_btBtn, 0, sizeof(_btBtn));
    _lx = _ly = _rx = _ry = 2048; // 12-bit センター値
}

/**
 * 十字キーを設定する。
 * 0x30 レポートの Byte3 下位4ビットに書き込む。
 *   bit0=Down, bit1=Up, bit2=Right, bit3=Left
 */
void SwitchControllerHID::setHat(Hat h) {
    uint8_t dpad = 0;
    switch (h) {
        case Hat::TOP:          dpad = 0x02; break; // Up
        case Hat::BOTTOM:       dpad = 0x01; break; // Down
        case Hat::LEFT:         dpad = 0x08; break; // Left
        case Hat::RIGHT:        dpad = 0x04; break; // Right
        case Hat::TOP_RIGHT:    dpad = 0x06; break; // Up + Right
        case Hat::TOP_LEFT:     dpad = 0x0A; break; // Up + Left
        case Hat::BOTTOM_RIGHT: dpad = 0x05; break; // Down + Right
        case Hat::BOTTOM_LEFT:  dpad = 0x09; break; // Down + Left
        case Hat::CENTER:       dpad = 0x00; break;
        default:                dpad = 0x00; break;
    }
    _btBtn[2] = (_btBtn[2] & 0xF0) | (dpad & 0x0F);
}

void SwitchControllerHID::setLeftStick(float x, float y) {
    x = constrain(x, -1.0f, 1.0f);
    y = constrain(y, -1.0f, 1.0f);
    _lx = static_cast<uint16_t>((x + 1.0f) * 2047.5f);
    _ly = static_cast<uint16_t>((y + 1.0f) * 2047.5f);
}

void SwitchControllerHID::setRightStick(float x, float y) {
    x = constrain(x, -1.0f, 1.0f);
    y = constrain(y, -1.0f, 1.0f);
    _rx = static_cast<uint16_t>((x + 1.0f) * 2047.5f);
    _ry = static_cast<uint16_t>((y + 1.0f) * 2047.5f);
}

void SwitchControllerHID::setLeftStickRaw(uint8_t x, uint8_t y) {
    _lx = to12bit(x);
    _ly = to12bit(y);
}

void SwitchControllerHID::setRightStickRaw(uint8_t x, uint8_t y) {
    _rx = to12bit(x);
    _ry = to12bit(y);
}

// ============================================================
//  send()  –  JoyConDroid の proxy.sendReport() 相当
// ============================================================
bool SwitchControllerHID::send() {
    if (!_connected) return false;
    return sendFullReport();
}

/**
 * 0x30 フルボタンレポートを送信する。
 *
 * JoyConDroid 側では:
 *   proxy.sendReport(remoteDevice, FULL_BUTTON_REPORT, data)
 * となっているものを、ESP-IDF では:
 *   esp_hidd_send_input_report(id, len, data)
 * で置き換えます。
 */
bool SwitchControllerHID::sendFullReport() {
    // 0x30 レポートは 12 バイト (IMU なし)
    uint8_t report[12] = {};

    report[0] = _timer++;  // タイマー (0-255, 自動インクリメント)

    // Byte1-3: ボタン
    report[1] = _btBtn[0]; // 右ボタン
    report[2] = _btBtn[1]; // 共通ボタン
    report[3] = _btBtn[2]; // 左ボタン + D-Pad

    // Byte4-6: 左スティック (12bit × 2 → 3 バイト packed)
    report[4] = (uint8_t)(_lx & 0xFF);
    report[5] = (uint8_t)((_lx >> 8) | ((_ly & 0x0F) << 4));
    report[6] = (uint8_t)(_ly >> 4);

    // Byte7-9: 右スティック (同上)
    report[7] = (uint8_t)(_rx & 0xFF);
    report[8] = (uint8_t)((_rx >> 8) | ((_ry & 0x0F) << 4));
    report[9] = (uint8_t)(_ry >> 4);

    // Byte10: バイブレーション入力レポート (0x00 固定)
    report[10] = 0x00;
    // Byte11: 予備
    report[11] = 0x00;

    esp_err_t err = esp_hidd_send_input_report(0x30, sizeof(report), report);
    return err == ESP_OK;
}

/**
 * サブコマンド応答 (0x21 レポート) を送信する。
 * JoyConDroid の sendReport(SUBCOMMAND_REPLY_REPORT, data) 相当。
 */
bool SwitchControllerHID::sendSubcmdAck(uint8_t subcmd,
                                         const uint8_t *reply,
                                         uint8_t replyLen) {
    // 0x21 レポート: 12 バイト共通ヘッダ + サブコマンドデータ
    const uint8_t kMaxReply = 30;
    uint8_t report[12 + kMaxReply] = {};
    uint8_t len = 12 + min(replyLen, kMaxReply);

    // Byte0: タイマー (静的にインクリメント)
    static uint8_t t = 0;
    report[0] = t++;

    // Byte1-10: ボタン / スティック ニュートラル
    report[4] = 0x00; // LX low
    report[5] = 0x80; // LX high / LY low
    report[6] = 0x00; // LY high
    report[7] = 0x00; // RX low
    report[8] = 0x80; // RX high / RY low
    report[9] = 0x00; // RY high

    // Byte11: ACK | サブコマンドID  (JoyConDroid の ACK | subcmd 相当)
    report[11] = 0x80 | subcmd;

    // Byte12+: サブコマンド返答データ
    if (reply && replyLen > 0) {
        memcpy(&report[12], reply, min(replyLen, kMaxReply));
    }

    esp_err_t err = esp_hidd_send_input_report(0x21, len, report);
    return err == ESP_OK;
}

// ============================================================
//  サブコマンドハンドラ
//  Switch が接続時に送ってくるコマンドへの最低限の応答
//  JoyConDroid の handleSubcommand() / 各 Handler クラス相当
// ============================================================
void SwitchControllerHID::handleSubcommand(const uint8_t *data, uint16_t len) {
    if (len < 11) return;

    // Interrupt Data のフォーマット:
    //   Byte0: タイマー
    //   Byte1-10: ランブル (無視)
    //   Byte10: サブコマンド ID
    //   Byte11+: サブコマンドパラメータ
    uint8_t subcmd = data[10];
    const uint8_t *param = (len > 11) ? &data[11] : nullptr;

    ESP_LOGD(TAG, "Subcommand: 0x%02X", subcmd);

    switch (subcmd) {
        case 0x00: // Controller State (JoyConDroid: CONTROLLER_STATE)
            sendSubcmdAck(0x00, nullptr, 0);
            break;

        case 0x01: // Bluetooth Manual Pairing (JoyConDroid: BLUETOOTH_MANUAL_PAIRING)
            sendSubcmdAck(0x01, nullptr, 0);
            break;

        case 0x02: { // Request Device Info (JoyConDroid: REQUEST_DEVICE_INFO)
            // Pro Controller デバイス情報 (JoyConDroid の DeviceInfoHandler 相当)
            uint8_t info[12] = {};
            info[0] = 0x03; // Firmware version major
            info[1] = 0x48; // Firmware version minor
            info[2] = 0x03; // Controller type: Pro Controller
            info[3] = 0x02; // Unknown
            // MAC アドレス (6 バイト) は esp_bt_dev_get_address() から取得可能だが省略
            info[10] = 0x01; // Unknown
            info[11] = 0x01; // Unknown
            sendSubcmdAck(0x02, info, sizeof(info));
            break;
        }

        case 0x03: // Set Input Report Mode (JoyConDroid: REQUEST_INPUT_REPORT_MODE)
            // どのモードを要求されても 0x30 フルモードで動作する
            sendSubcmdAck(0x03, nullptr, 0);
            ESP_LOGI(TAG, "Input mode set to 0x%02X", param ? param[0] : 0xFF);
            break;

        case 0x04: // Trigger Buttons Elapsed Time (JoyConDroid: REQUEST_TRIGGER_BUTTONS)
            sendSubcmdAck(0x04, nullptr, 0);
            break;

        case 0x08: // Set Shipment State (JoyConDroid: REQUEST_SET_SHIPMENT)
            sendSubcmdAck(0x08, nullptr, 0);
            break;

        case 0x10: { // SPI Flash Read (JoyConDroid: REQUEST_SPI_FLASH_READ)
            // Switch はスティックキャリブレーション等を SPI から読む
            // ゼロ埋めで返すと Switch はデフォルト値を使用する
            if (!param || len < 16) {
                sendSubcmdAck(0x10, nullptr, 0);
                break;
            }
            uint32_t addr = (uint32_t)param[0] | ((uint32_t)param[1] << 8)
                          | ((uint32_t)param[2] << 16) | ((uint32_t)param[3] << 24);
            uint8_t  readLen = param[4];
            readLen = min(readLen, (uint8_t)0x1D);

            uint8_t reply[5 + 0x1D] = {};
            // アドレスとサイズをエコーバック
            reply[0] = param[0]; reply[1] = param[1];
            reply[2] = param[2]; reply[3] = param[3];
            reply[4] = readLen;
            // データはゼロ埋め (既に memset 済み)

            sendSubcmdAck(0x10, reply, 5 + readLen);
            (void)addr;
            break;
        }

        case 0x30: // Set Player Lights (JoyConDroid: REQUEST_SET_PLAYER_LIGHTS)
            sendSubcmdAck(0x30, nullptr, 0);
            break;

        case 0x40: // Enable IMU (JoyConDroid: REQUEST_AXIS_SENSOR)
            sendSubcmdAck(0x40, nullptr, 0);
            break;

        case 0x41: // IMU Sensitivity (JoyConDroid: SET_IMU_SENSITIVITY)
            sendSubcmdAck(0x41, nullptr, 0);
            break;

        case 0x48: // Enable Vibration (JoyConDroid: REQUEST_VIBRATION)
            sendSubcmdAck(0x48, nullptr, 0);
            break;

        default:
            // 未対応コマンドは ACK だけ返す
            sendSubcmdAck(subcmd, nullptr, 0);
            ESP_LOGW(TAG, "Unknown subcommand 0x%02X – ACK only", subcmd);
            break;
    }
}

// ============================================================
//  HID Device コールバック
//  JoyConDroid の BluetoothHidDevice.Callback 内メソッドに対応
// ============================================================
void SwitchControllerHID::onHiddEvent(esp_hidd_cb_event_t event,
                                       esp_hidd_cb_param_t *param) {
    switch (event) {
        case ESP_HIDD_API_INIT_EVT:
            // HID プロファイル初期化完了
            // JoyConDroid の onAppStatusChanged(registered=true) 相当
            ESP_LOGI(TAG, "HID profile initialized");
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            break;

        case ESP_HIDD_OPEN_EVT:
            // Switch と接続完了
            // JoyConDroid の onConnectionStateChanged(STATE_CONNECTED) 相当
            _connected = true;
            if (param) {
                memcpy(_remoteAddr, param->open.bd_addr, sizeof(esp_bd_addr_t));
                ESP_LOGI(TAG, "Switch connected: %02X:%02X:%02X:%02X:%02X:%02X",
                    _remoteAddr[0], _remoteAddr[1], _remoteAddr[2],
                    _remoteAddr[3], _remoteAddr[4], _remoteAddr[5]);
            }
            // 接続後すぐに初期レポートを送ってハンドシェイクを促す
            controller.sendFullReport();
            break;

        case ESP_HIDD_CLOSE_EVT:
            // 切断  –  JoyConDroid の onConnectionStateChanged(STATE_DISCONNECTED) 相当
            _connected = false;
            ESP_LOGI(TAG, "Switch disconnected. Re-advertising...");
            // 再接続のためスキャン可能に戻す
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            break;

        case ESP_HIDD_INTR_DATA_EVT:
            // Switch からのデータ (サブコマンド等)
            // JoyConDroid の onInterruptData() 相当
            if (param) {
                handleSubcommand(param->intr_data.data, param->intr_data.len);
            }
            break;

        case ESP_HIDD_SET_REPORT_EVT:
            // JoyConDroid の onSetReport() 相当
            break;

        case ESP_HIDD_GET_REPORT_EVT:
            // JoyConDroid の onGetReport() 相当
            controller.sendFullReport();
            break;

        case ESP_HIDD_SET_PROTOCOL_EVT:
            // JoyConDroid の onSetProtocol() 相当 (無視で OK)
            break;

        case ESP_HIDD_VC_UNPLUG_EVT:
            // JoyConDroid の onVirtualCableUnplug() 相当
            _connected = false;
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            break;

        default:
            break;
    }
}

// ============================================================
//  GAP (General Access Profile) コールバック
// ============================================================
void SwitchControllerHID::onGapEvent(esp_bt_gap_cb_event_t event,
                                      esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Pairing success with: %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGW(TAG, "Pairing failed, status=%d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_PIN_REQ_EVT:
            // Switch は SSP を使うが念のため PIN 対応
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4,
                                 (uint8_t *)"0000");
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            // Just Works (数字確認なし)
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_KEY_NOTIF_EVT:
            ESP_LOGI(TAG, "SSP Passkey: %06lu", param->key_notif.passkey);
            break;

        default:
            break;
    }
}

// ============================================================
//  Bluetooth デュアルモード初期化ヘルパー
//  NimBLE の代わりに BlueDroid (BTDM) を使う際の初期化
// ============================================================
esp_err_t initBluetoothDualMode() {
    esp_err_t ret;

    // BT コントローラーが未初期化の場合のみ初期化
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ret = esp_bt_controller_init(&btCfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
            return ret;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        // BTDM = Classic BT + BLE 両方を有効化
        ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_enable(BTDM) failed: %d", ret);
            return ret;
        }
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        ret = esp_bluedroid_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bluedroid_init failed: %d", ret);
            return ret;
        }
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_bluedroid_enable failed: %d", ret);
            return ret;
        }
    }

    // SSP (Secure Simple Pairing) を有効化
    esp_bt_sp_param_t param = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t ioCapMode = ESP_BT_IO_CAP_NONE; // Just Works
    esp_bt_gap_set_security_param(param, &ioCapMode, sizeof(esp_bt_io_cap_t));
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, (uint8_t *)"0000");

    ESP_LOGI(TAG, "Bluetooth BTDM (Classic+BLE) initialized");
    return ESP_OK;
}
