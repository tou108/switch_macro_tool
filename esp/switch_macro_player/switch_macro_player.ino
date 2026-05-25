/**
 * =========================================================================
 * M5Atom Switch Macro Player  –  Bluetooth Classic HID 版
 *
 * 変更点 (対 USB HID 版):
 *  - Switch との接続方式: USB HID → Bluetooth Classic HID
 *    JoyConDroid APK と全く同じ接続方式 (BluetoothHidDevice 相当)
 *  - PC との通信 (BLE UART): NimBLE → Arduino BLE (BlueDroid)
 *  - ターゲットボード: M5AtomS3 → M5Atom (ESP32 無印)
 *    ⚠️ ESP32-S3 は Classic Bluetooth 非対応
 *
 * 必要ライブラリ:
 *   - M5Atom (M5Stack公式)
 *   - ArduinoJson
 *   - ESP32 Arduino 2.x (ESP-IDF 4.4.x 同梱)
 * =========================================================================
 */

#include "SwitchHID.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <M5Atom.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>

// --- BLE UART サービス UUID (元コードと同一, ブラウザ変更不要) ---
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define MACRO_FILE             "/macro.json"

// ============================================================
//  デバイス状態
// ============================================================
enum DeviceStatus {
    STATUS_IDLE,
    STATUS_CONNECTED,
    STATUS_TRANSFERRING,
    STATUS_READY,
    STATUS_PLAYING,
    STATUS_WAITING,
    STATUS_ERROR
};

volatile DeviceStatus currentStatus = STATUS_IDLE;
volatile bool         statusChanged  = true;
String                rxBuffer       = "";
DynamicJsonDocument  *currentMacro   = nullptr;

// ============================================================
//  LED ヘルパー  (M5Atom: drawpix(index, color))
// ============================================================
static inline void setLED(uint32_t color) {
    M5.dis.drawpix(0, color);
    M5.dis.show();
}

// ============================================================
//  MacroPlayer
// ============================================================
class MacroPlayer {
public:
    int           eventIndex    = 0;
    unsigned long startTime     = 0;
    unsigned long waitStartTime = 0;
    int           remainingLoops = 0;
    bool          loopEnabled   = false;
    int           loopCount     = 0;
    int           loopInterval  = 0;

    void loadSettings(DynamicJsonDocument *macro) {
        if (!macro) return;
        JsonObject loop = (*macro)["loop"];
        loopEnabled  = loop["enabled"]  | false;
        loopCount    = loop["count"]    | 0;
        loopInterval = loop["interval"] | 0;
    }

    void start() {
        eventIndex    = 0;
        startTime     = millis();
        remainingLoops = loopEnabled ? (loopCount == 0 ? 0 : loopCount) : 1;
    }

    /**
     * ボタン + スティックを BT HID レポートに変換して Switch へ送信
     * (旧 USB HID: controller.setButtonMask + hid.SendReport)
     * (新 BT HID : controller.setButtonMask + controller.send)
     */
    bool updateHID(uint32_t buttons, int8_t lx, int8_t ly,
                   int8_t rx, int8_t ry) {
        controller.releaseAll();
        controller.setButtonMask(buttons);

        // bit16-19: 十字キー → Hat 変換
        bool u = buttons & (1 << 16), d = buttons & (1 << 17),
             l = buttons & (1 << 18), r = buttons & (1 << 19);
        if      (u && r) controller.setHat(Hat::TOP_RIGHT);
        else if (u && l) controller.setHat(Hat::TOP_LEFT);
        else if (d && r) controller.setHat(Hat::BOTTOM_RIGHT);
        else if (d && l) controller.setHat(Hat::BOTTOM_LEFT);
        else if (u)      controller.setHat(Hat::TOP);
        else if (d)      controller.setHat(Hat::BOTTOM);
        else if (l)      controller.setHat(Hat::LEFT);
        else if (r)      controller.setHat(Hat::RIGHT);
        else             controller.setHat(Hat::CENTER);

        controller.setLeftStickRaw((uint8_t)(lx + 128), (uint8_t)(ly + 128));
        controller.setRightStickRaw((uint8_t)(rx + 128), (uint8_t)(ry + 128));

        return controller.send(); // BT HID 経由で Switch に送信
    }

    bool updateFromEvent(JsonObject event) {
        uint32_t buttons = 0;
        for (int b : event["b"].as<JsonArray>())
            buttons |= (1 << b);
        JsonArray axes = event["a"];
        return updateHID(buttons,
            (int8_t)(axes[0].as<float>() * 127),
            (int8_t)(axes[1].as<float>() * 127),
            (int8_t)(axes[2].as<float>() * 127),
            (int8_t)(axes[3].as<float>() * 127));
    }

    void stop() { updateHID(0, 0, 0, 0, 0); }
};

MacroPlayer player;

// ============================================================
//  BLE UART コールバック  (NimBLE → Arduino BLE)
// ============================================================
static BLEServer *pBleServer = nullptr;

class BleServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *) override {
        currentStatus = STATUS_CONNECTED;
        statusChanged = true;
    }
    void onDisconnect(BLEServer *server) override {
        if (currentStatus != STATUS_PLAYING  &&
            currentStatus != STATUS_WAITING  &&
            currentStatus != STATUS_READY) {
            currentStatus = STATUS_IDLE;
            statusChanged = true;
        }
        server->startAdvertising(); // 再アドバタイズ
    }
};

class BleRxCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) override {
        std::string value = pChar->getValue();

        // プロトコル A: リアルタイム中継 "L:buttons:lx:ly:rx:ry"
        if (value.length() > 2 && value[0] == 'L' && value[1] == ':') {
            String s = value.c_str();
            int f  = s.indexOf(':', 2),
                p2 = s.indexOf(':', f + 1),
                p3 = s.indexOf(':', p2 + 1),
                p4 = s.indexOf(':', p3 + 1);
            player.updateHID(
                (uint32_t)s.substring(2, f).toInt(),
                (int8_t)s.substring(f + 1, p2).toInt(),
                (int8_t)s.substring(p2 + 1, p3).toInt(),
                (int8_t)s.substring(p3 + 1, p4).toInt(),
                (int8_t)s.substring(p4 + 1).toInt());
            return;
        }

        // プロトコル B: マクロ転送 START: ... END
        if (value.find("START:") == 0) {
            rxBuffer = "";
            currentStatus = STATUS_TRANSFERRING;
            statusChanged = true;
        } else if (value == "END") {
            File file = LittleFS.open(MACRO_FILE, FILE_WRITE);
            if (file) { file.print(rxBuffer); file.close(); }

            if (currentMacro) delete currentMacro;
            currentMacro = new DynamicJsonDocument(32768);
            if (deserializeJson(*currentMacro, rxBuffer)) {
                currentStatus = STATUS_ERROR;
            } else {
                player.loadSettings(currentMacro);
                currentStatus = STATUS_READY;
            }
            statusChanged = true;
        } else if (currentStatus == STATUS_TRANSFERRING) {
            rxBuffer += value.c_str();
        }
    }
};

// ============================================================
//  setup()
// ============================================================
void setup() {
    // M5Atom 初期化 (M5AtomS3 から変更)
    M5.begin(true, false, true); // Serial, I2C, LED
    M5.dis.setBrightness(20);
    setLED(0x00FFFF); // 水色: 起動中
    M5.update();

    // ファイルシステム
    if (!LittleFS.begin(true)) {
        setLED(0xFF0000);
        delay(1000);
    }

    // ----------------------------------------------------------
    // ① BTDM (Classic BT + BLE) デュアルモード初期化
    //
    //    JoyConDroid は Android の BluetoothAdapter を BTDM モードで
    //    使用している。ESP32 でも同様に BTDM を先に有効化することで
    //    Classic BT HID (Switch 向け) と BLE UART (PC 向け) を共存させる。
    //    BLEDevice::init() はコントローラーが既に有効なら二重初期化しない。
    // ----------------------------------------------------------
    ESP_ERROR_CHECK(initBluetoothDualMode());

    // ----------------------------------------------------------
    // ② Classic BT HID プロファイル開始
    //    JoyConDroid の BluetoothHidDevice.registerApp() 相当
    //    Switch の「コントローラーとの通信」画面でペアリングできる
    // ----------------------------------------------------------
    controller.begin();

    // ----------------------------------------------------------
    // ③ BLE UART サーバー (ブラウザ ↔ M5Atom, UUID は元コードと同一)
    //    NimBLE から Arduino BLE (BlueDroid) へ変更
    // ----------------------------------------------------------
    BLEDevice::init("M5Atom-Macro");
    pBleServer = BLEDevice::createServer();
    pBleServer->setCallbacks(new BleServerCB());

    BLEService *pSvc = pBleServer->createService(SERVICE_UUID);
    BLECharacteristic *pRx = pSvc->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE   |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_READ);
    pRx->setCallbacks(new BleRxCB());
    pSvc->start();
    BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
    BLEDevice::startAdvertising();

    setLED(0xFF00FF); // 紫: 起動完了
    M5.update();
    delay(500);

    // 保存済みマクロを自動ロード
    if (LittleFS.exists(MACRO_FILE)) {
        File file = LittleFS.open(MACRO_FILE, FILE_READ);
        if (file) {
            if (currentMacro) delete currentMacro;
            currentMacro = new DynamicJsonDocument(32768);
            if (!deserializeJson(*currentMacro, file)) {
                player.loadSettings(currentMacro);
                currentStatus = STATUS_READY;
            }
            file.close();
        }
    }
    statusChanged = true;
}

// ============================================================
//  loop()  –  マクロ再生ロジックは元コードから変更なし
// ============================================================
void loop() {
    M5.update();

    // LED 状態更新
    if (statusChanged) {
        statusChanged = false;
        switch (currentStatus) {
            case STATUS_IDLE:         setLED(0xFFFF00); break; // 黄
            case STATUS_CONNECTED:    setLED(0x0000FF); break; // 青
            case STATUS_TRANSFERRING: setLED(0x00FFFF); break; // 水色
            case STATUS_READY:        setLED(0x00FF00); break; // 緑
            case STATUS_PLAYING:      setLED(0xFF00FF); break; // 紫
            case STATUS_WAITING:      setLED(0x00FFFF); break; // 水色
            case STATUS_ERROR:        setLED(0xFF0000); break; // 赤
        }
    }

    // ボタン A: 再生 / 停止  (M5Atom: M5.Btn, M5AtomS3: AtomS3.BtnA)
    if (M5.Btn.wasPressed()) {
        if ((currentStatus == STATUS_READY   ||
             currentStatus == STATUS_IDLE    ||
             currentStatus == STATUS_CONNECTED) && currentMacro) {
            currentStatus = STATUS_PLAYING;
            statusChanged = true;
            player.start();
        } else if (currentStatus == STATUS_PLAYING) {
            currentStatus = (pBleServer->getConnectedCount() > 0)
                            ? STATUS_CONNECTED : STATUS_READY;
            statusChanged = true;
            player.stop();
        }
    }

    // マクロ再生 (タイミング制御は元コードと同一)
    if (currentStatus == STATUS_PLAYING && currentMacro) {
        JsonArray events = (*currentMacro)["events"];
        if (player.eventIndex >= (int)events.size()) {
            player.stop();
            if (player.loopEnabled &&
                (player.loopCount == 0 || player.remainingLoops > 1)) {
                if (player.loopCount > 0) player.remainingLoops--;
                if (player.loopInterval > 0) {
                    currentStatus = STATUS_WAITING;
                    statusChanged = true;
                    player.waitStartTime = millis();
                } else {
                    player.eventIndex = 0;
                    player.startTime  = millis();
                }
            } else {
                currentStatus = STATUS_READY;
                statusChanged = true;
            }
        } else {
            JsonObject event = events[player.eventIndex];
            if (millis() - player.startTime >= (unsigned long)(int)event["t"]) {
                if (player.updateFromEvent(event)) {
                    player.eventIndex++;
                }
            }
        }
    } else if (currentStatus == STATUS_WAITING) {
        if (millis() - player.waitStartTime >=
            (unsigned long)(player.loopInterval * 1000)) {
            currentStatus  = STATUS_PLAYING;
            statusChanged  = true;
            player.eventIndex = 0;
            player.startTime  = millis();
        }
    }
}
