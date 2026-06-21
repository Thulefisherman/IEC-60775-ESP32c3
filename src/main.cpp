#include <Arduino.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <NimBLEDevice.h>
#include <Wire.h>

// --- 引脚定义 ---
#define PIN_MOSFET 0 // IO1 控制电磁阀
#define PIN_WS2812 7 // IO10 控制灯带
#define NUM_PIXELS 1 // 假设只有1颗灯珠
#define PIN_SQW 6    // DS3231 的 INT/SQW 连接到 IO6
#define PIN_SDA 9    // DS3231 的 SDA 连接到 IO9
#define PIN_SCL 8    // DS3231 的 SCL 连接到 IO8

// UUID 定义 (建议使用工具生成唯一的 UUID)
#define SERVICE_UUID "0000ABCD-0000-1000-8000-00805F9B34FB"
#define CHAR_CONFIG_UUID "00001234-0000-1000-8000-00805F9B34FB"
#define CHAR_NAME_UUID "00005678-0000-1000-8000-00805F9B34FB"
#define CHAR_NOTIFY_UUID "00009ABC-0000-1000-8000-00805F9B34FB"

// --- 新增中断控制变量 ---
volatile bool rtcTickOccurred = false;
volatile uint32_t rtcTotalSeconds = 0; // 自启动以来的总秒数

// --- 全局增加一个目标时刻变量 ---
uint32_t nextEventSeconds = 0;

// --- 枚举定义 ---
enum SystemStatus
{
    RUNNING = 0,
    PAUSED = 1,
    STANDBY = 2,
    COMPLETED = 3
};

// --- 全局变量结构体 ---
struct DeviceData
{
    String bleName; // 【蓝牙名称】用于 BLE 广播识别，掉电存储可修改设备绰号

    int totalStages;  // 【总阶段数】整个测试/工作流程包含的任务总数
    int currentStage; // 【当前阶段】指示当前正在运行第几个任务阶段（从0或1开始）

    int onTime;  // 【吸合时长】电磁阀导通（IO4输出低电平）的持续时间，单位：毫秒(ms)
    int offTime; // 【断开时长】单次循环中电磁阀断电的持续时间，单位：毫秒(ms)

    int targetCycles;  // 【目标循环数】当前阶段设定需要完成的动作总次数
    int currentCycles; // 【当前循环数】已实际完成的动作次数，通常达到 targetCycles 后进入下一阶段

    int coolDownTime;    // 【冷却总时长】单位：小时 (用于设置)
    int currentCoolDown; // 【剩余冷却秒数】单位：秒 (用于倒计时和掉电恢复)
    SystemStatus status; // 【系统运行状态】枚举值：存储当前设备是处于“运行、暂停、待机还是完成”
};

// 专门用于传输的结构体 (剔除 String，确保长度固定)
struct __attribute__((packed)) DeviceDataTransfer
{
    uint8_t totalStages;      // 1字节 (Offset 0)
    uint8_t currentStage;     // 1字节 (Offset 1)
    uint16_t onTime;          // 2字节 (Offset 2)
    uint16_t offTime;         // 2字节 (Offset 4)
    uint32_t targetCycles;    // 4字节 (Offset 6)
    uint32_t currentCycles;   // 4字节 (Offset 10)
    uint8_t coolDownTime;     // 1字节 (Offset 14) - 范围0-60足够
    uint32_t currentCoolDown; // 4字节 (Offset 15)
    uint8_t status;           // 1字节 (Offset 19)
}; // 总计：1+1+2+2+4+4+1+4+1 = 20 字节 (符合 BLE 20字节通知限制)

NimBLEServer *pServer;
NimBLECharacteristic *pConfigChar;
NimBLECharacteristic *pNameChar;
NimBLECharacteristic *pNotifyChar;

DeviceData myConfig;
Preferences prefs;
Adafruit_NeoPixel pixels(NUM_PIXELS, PIN_WS2812, NEO_GRB + NEO_KHZ800);

// --- 新增：存储控制变量 ---
unsigned long lastSaveMillis = 0;
int lastSaveCycles = 0;
const unsigned long FIFTEEN_MINUTES = 15 * 60 * 1000; // 15分钟毫秒数

// --- 状态控制专用变量 ---
unsigned long previousMillis = 0;
bool isValveOn = false;
bool inCoolDown = false;

// --- 存储数据 ---
void saveData()
{
    prefs.begin("sys_cfg", false); // 打开命名空间 "sys_cfg"

    prefs.putString("bleName", myConfig.bleName);
    prefs.putInt("totalStages", myConfig.totalStages);
    prefs.putInt("currStage", myConfig.currentStage);
    prefs.putInt("onTime", myConfig.onTime);
    prefs.putInt("offTime", myConfig.offTime);
    prefs.putInt("targetCycles", myConfig.targetCycles);
    prefs.putInt("currCycles", myConfig.currentCycles);
    prefs.putInt("coolDown", myConfig.coolDownTime);
    prefs.putInt("currCoolDown", myConfig.currentCoolDown);
    prefs.putInt("status", (int)myConfig.status); // 存储枚举需强转为int
    prefs.end();
    Serial.println("数据已存入掉电存储");
}

// main_40.cpp

void updateLocalConfig(DeviceDataTransfer *incoming)
{
    // --- 权限 A：能读能写的配置项 (直接覆盖) ---
    myConfig.totalStages = incoming->totalStages;
    // 转换单位（秒->毫秒）
    myConfig.onTime = (incoming->onTime < 100) ? (incoming->onTime * 1000) : incoming->onTime;
    myConfig.offTime = (incoming->offTime < 100) ? (incoming->offTime * 1000) : incoming->offTime;
    myConfig.targetCycles = incoming->targetCycles;
    myConfig.coolDownTime = incoming->coolDownTime;

    // --- 权限 B：状态控制 ---
    if (myConfig.status != RUNNING && (SystemStatus)incoming->status == RUNNING)
    {
        // 刚开始运行，立即执行第一次动作
        nextEventSeconds = rtcTotalSeconds;
    }
    myConfig.status = (SystemStatus)incoming->status;

    // --- 权限 C：进度控制 (只有在特定指令下才清零) ---
    // 逻辑：如果收到 currentStage 为 0 且 status 为 STANDBY (2)，视为“清零”指令
    if (incoming->currentStage == 0 && incoming->status == STANDBY)
    {
        Serial.println(">>> 触发重置指令：进度归零");
        myConfig.currentStage = 0;
        myConfig.currentCycles = 0;
        myConfig.currentCoolDown = 0;
        inCoolDown = false;
        nextEventSeconds = rtcTotalSeconds;
        saveData(); // 立即存档
    }
    else
    {
        // 正常运行或暂停时，硬件忽略手机传来的 currentStage/Cycles，保持自己的进度
        Serial.println(">>> 忽略手机端进度数据，保持硬件本地进度");
    }

    // 非法参数检查
    if (myConfig.totalStages == 0 || myConfig.targetCycles == 0)
    {
        myConfig.status = COMPLETED;
    }
}

// --- 发送通知的函数 ---
void notifyStatusUpdate()
{
    if (pServer->getConnectedCount() > 0)
    {
        DeviceDataTransfer txData;
        txData.totalStages = (uint8_t)myConfig.totalStages;
        txData.currentStage = (uint8_t)myConfig.currentStage;
        txData.onTime = (uint16_t)myConfig.onTime / 1000;   // 转换为秒，符合传输结构定义
        txData.offTime = (uint16_t)myConfig.offTime / 1000; // 转换为秒，符合传输结构定义
        txData.targetCycles = (uint32_t)myConfig.targetCycles;
        txData.currentCycles = (uint32_t)myConfig.currentCycles;
        txData.coolDownTime = (uint8_t)myConfig.coolDownTime; // 修正为 uint8_t
        txData.currentCoolDown = (uint32_t)myConfig.currentCoolDown;
        txData.status = (uint8_t)myConfig.status;

        pNotifyChar->setValue((uint8_t *)&txData, sizeof(txData));
        pNotifyChar->notify();

        // --- 新增：打印通知发送的时刻与核心参数 ---
        Serial.printf("[BLE通知] 已成功发送 -> 毫秒级时间戳: %lu ms | RTC总秒数: %u s | 状态: %d | 循环数: %d/%d | 剩余冷却: %u s\n", 
                      millis(), rtcTotalSeconds, myConfig.status, myConfig.currentCycles, myConfig.targetCycles, myConfig.currentCoolDown);
    }
}
// --- 读取数据 ---
void loadData()
{
    prefs.begin("sys_cfg", true); // 只读模式打开

    // 第二个参数是默认值，防止初次上电读取空数据
    myConfig.bleName = prefs.getString("bleName", "ESP32-Valve");
    myConfig.totalStages = prefs.getInt("totalStages", 1);
    myConfig.currentStage = prefs.getInt("currStage", 0);
    myConfig.onTime = prefs.getInt("onTime", 1000);
    myConfig.offTime = prefs.getInt("offTime", 1000);
    myConfig.targetCycles = prefs.getInt("targetCycles", 10);
    myConfig.currentCycles = prefs.getInt("currCycles", 0);
    myConfig.coolDownTime = prefs.getInt("coolDown", 5000);
    myConfig.status = (SystemStatus)prefs.getInt("status", (int)STANDBY);
    myConfig.currentCoolDown = prefs.getInt("currCoolDown", 0);
    if (myConfig.currentCoolDown > 0 && myConfig.status == RUNNING)
    {
        inCoolDown = true;
    }
    prefs.end();
    Serial.println("从存储加载数据成功");
}

// --- 写入回调：当手机修改数据时触发 ---
// 在 NimBLE 回调类中添加反馈逻辑
class MyCallbacks : public NimBLECharacteristicCallbacks
{
    // 在 MyCallbacks 类中修改
    // main_40.cpp -> MyCallbacks 类

    void onRead(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
    {
        if (pCharacteristic->getUUID().equals(NimBLEUUID(CHAR_CONFIG_UUID)))
        {
            DeviceDataTransfer txData;

            // 填充硬件当前的真实值
            txData.totalStages = (uint8_t)myConfig.totalStages;
            txData.currentStage = (uint8_t)myConfig.currentStage;
            txData.onTime = (uint16_t)(myConfig.onTime / 1000);
            txData.offTime = (uint16_t)(myConfig.offTime / 1000);
            txData.targetCycles = (uint32_t)myConfig.targetCycles;
            txData.currentCycles = (uint32_t)myConfig.currentCycles; // 关键：实时循环数
            txData.coolDownTime = (uint8_t)myConfig.coolDownTime;
            txData.currentCoolDown = (uint32_t)myConfig.currentCoolDown;
            txData.status = (uint8_t)myConfig.status;

            pCharacteristic->setValue((uint8_t *)&txData, sizeof(txData));
        }
    }
    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
    {
        processWrite(pCharacteristic);
    }
    void processWrite(NimBLECharacteristic *pCharacteristic)
    {
        Serial.print("收到写入请求，UUID: ");
        Serial.println(pCharacteristic->getUUID().toString().c_str());

        // 使用 equals 直接比较对象，而不是字符串
        if (pCharacteristic->getUUID().equals(NimBLEUUID(CHAR_CONFIG_UUID)))
        {
            Serial.println(">>> 匹配成功：配置特征写入");
            std::string value = pCharacteristic->getValue();

            if (value.length() == sizeof(DeviceDataTransfer))
            {
                DeviceDataTransfer *incoming = (DeviceDataTransfer *)value.data();
                updateLocalConfig(incoming);
                Serial.println(">>> 配置已更新并存储");
                saveData();
                notifyStatusUpdate();
            }
            else
            {
                Serial.printf(">>> 数据长度不匹配: 预期 %d, 实际 %d\n",
                              sizeof(DeviceDataTransfer), value.length());
            }
        }
        else if (pCharacteristic->getUUID().equals(NimBLEUUID(CHAR_NAME_UUID)))
        {
            Serial.println(">>> 匹配成功：蓝牙名称写入");
            myConfig.bleName = String(pCharacteristic->getValue().c_str());
            saveData();
            Serial.println(">>> 名称已保存，系统即将在 500ms 后重启以生效...");

            // 2. 延迟一小会儿，确保数据处理完成并让手机端收到可能的确认
            delay(500);

            // 3. 执行重启
            ESP.restart();
        }
    }
};

class MyServerCallbacks : public NimBLEServerCallbacks
{
    // 加上 NimBLEConnInfo& 参数，并建议加上 override 强制校验
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
    {
        Serial.println(">>> 手机已连接");
        notifyStatusUpdate();
    };

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
    {
        Serial.printf(">>> 手机已断开，原因代码: %d\n", reason);

        // 重新启动广播，这样手机才能再次搜到它
        NimBLEDevice::startAdvertising();
        Serial.println(">>> 已重新启动广播...");
    }
};

void setupBLE()
{
    NimBLEDevice::init(myConfig.bleName.c_str());

    // ----------------------------
    pServer = NimBLEDevice::createServer();
    NimBLEService *pService = pServer->createService(SERVICE_UUID);
    pServer->setCallbacks(new MyServerCallbacks());

    // 1. 配置信息特征 (读+写)
    pConfigChar = pService->createCharacteristic(
        CHAR_CONFIG_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    pConfigChar->setCallbacks(new MyCallbacks());
    // --- 新增：初始化时就给特征值赋值 ---
    DeviceDataTransfer initData;
    initData.totalStages = (uint8_t)myConfig.totalStages;
    initData.currentStage = (uint8_t)myConfig.currentStage;
    initData.onTime = (uint16_t)(myConfig.onTime / 1000);   // 这里要除以 1000
    initData.offTime = (uint16_t)(myConfig.offTime / 1000); // 这里要除以 1000[cite: 6]
    initData.targetCycles = (uint32_t)myConfig.targetCycles;
    initData.currentCycles = (uint32_t)myConfig.currentCycles;
    initData.coolDownTime = (uint8_t)myConfig.coolDownTime;
    initData.currentCoolDown = (uint32_t)myConfig.currentCoolDown;
    initData.status = (uint8_t)myConfig.status;

    pConfigChar->setValue((uint8_t *)&initData, sizeof(initData));

    // 2. 蓝牙名特征 (读+写)
    pNameChar = pService->createCharacteristic(
        CHAR_NAME_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    pNameChar->setValue(myConfig.bleName.c_str());
    pNameChar->setCallbacks(new MyCallbacks());

    // 3. 通知特征 (只读+通知)
    pNotifyChar = pService->createCharacteristic(
        CHAR_NOTIFY_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();

    // --- 1. 配置主广播数据 (Advertisement Data) ---
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP); // 设置广播标志
    advData.setName(myConfig.bleName.c_str());                          // 设置名称
    advData.setCompleteServices(NimBLEUUID(SERVICE_UUID));              // 设置服务 UUID
    pAdvertising->setAdvertisementData(advData);

    // --- 2. 配置扫描响应数据 (Scan Response Data) ---
    // 在 2.x 版本中，通过设置这个对象即等同于开启了 Scan Response
    NimBLEAdvertisementData scanResponseData;
    scanResponseData.setCompleteServices(NimBLEUUID(SERVICE_UUID)); // 也可以在这里放 UUID
    pAdvertising->setScanResponseData(scanResponseData);

    // --- 3. 启动广播 ---
    pAdvertising->start();
    Serial.println(">>> 蓝牙广播已启动 (基于 NimBLE 2.x 协议)");
}

// --- 修改后的智能存储判断函数 ---
void smartSave(bool force = false)
{
    unsigned long currentMillis = millis();
    Serial.printf("智能存储检查: 时间间隔=%lu ms, 循环间隔=%d, 强制存储=%s\n",
                  currentMillis - lastSaveMillis, abs(myConfig.currentCycles - lastSaveCycles),
                  force ? "是" : "否");
    Serial.println("ontime: " + String(myConfig.onTime) + " | offtime: " + String(myConfig.offTime));

    // 15分钟阈值判断[cite: 13]
    bool timeCondition = (currentMillis - lastSaveMillis >= FIFTEEN_MINUTES);

    // 次数阈值判断：此处改为你要求的 100 次 (或根据需要设为更高)[cite: 13]
    bool cycleCondition = (abs(myConfig.currentCycles - lastSaveCycles) >= 100);

    if ((timeCondition && cycleCondition) || force)
    {
        saveData();
        lastSaveMillis = currentMillis;
        lastSaveCycles = myConfig.currentCycles;
    }
}

// --- 新增：蓝紫渐变函数 ---
void updateRunningLights()
{
    static float breathPos = 0;
    breathPos += 0.001; // 控制渐变速度
    if (breathPos > 2 * PI)
        breathPos = 0;

    // 计算渐变比率 (0.0 ~ 1.0)
    float ratio = (sin(breathPos) + 1.0) / 2.0;

    // 在 蓝色(0,0,255) 和 紫色(128,0,128) 之间插值
    int r = (int)(128 * ratio);
    int g = 0;
    int b = 255 - (int)(127 * ratio);

    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}
// --- 中断服务函数 (ISR) ---
// 放在 IRAM 中以保证运行速度
void IRAM_ATTR onRtcInterrupt()
{
    rtcTickOccurred = true;
    rtcTotalSeconds++;
}

void setup()
{
    Serial.begin(115200);

    pinMode(PIN_MOSFET, OUTPUT);
    digitalWrite(PIN_MOSFET, HIGH); // 默认高电平=关闭

    pixels.begin();
    loadData(); // 加载存储
    setupBLE(); // 开启蓝牙服务
    Wire.begin(PIN_SDA, PIN_SCL);

    Wire.beginTransmission(0x68); // DS3231 地址
    Wire.write(0x0E);             // 控制寄存器
    // BBSQW=1, RS2=0, RS1=0, INTCN=0 (使能1Hz方波输出)
    Wire.write(0b01000000);
    Wire.endTransmission();

    pinMode(PIN_SQW, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_SQW), onRtcInterrupt, FALLING);
}

unsigned long lastBlePrintMillis = 0;
const unsigned long BLE_PRINT_INTERVAL = 5000; // 10秒
void loop()
{
    // 依然保留给灯效，因为灯效需要毫秒级的平滑度
    if (myConfig.status == RUNNING)
    {
        updateRunningLights();
    }
    // --- 核心：RTC 驱动的逻辑 ---
    if (rtcTickOccurred)
    {
        rtcTickOccurred = false;

        // 获取当前绝对秒数（增加临界区保护，防止读取时被中断修改）
        noInterrupts();
        uint32_t now = rtcTotalSeconds;
        interrupts();

        if (myConfig.status == RUNNING)
        {
            // 阶段检查
            if (myConfig.currentStage >= myConfig.totalStages && myConfig.currentStage != 255)
            {
                myConfig.status = COMPLETED;
                saveData();
                notifyStatusUpdate();
                return;
            }

            // --- 状态机处理 ---
            if (inCoolDown)
            {
                // 冷却逻辑
                if (now >= nextEventSeconds)
                {
                    inCoolDown = false;
                    myConfig.currentStage++;
                    myConfig.currentCycles = 0;
                    myConfig.currentCoolDown = 0;

                    // 冷却结束，计算下一次吸合的结束时刻
                    nextEventSeconds = now + (myConfig.onTime / 1000);
                    isValveOn = true;
                    digitalWrite(PIN_MOSFET, LOW); // 开启

                    smartSave(true);
                    notifyStatusUpdate();
                    Serial.println(">>> 冷却结束，进入下一阶段吸合");
                }
                else
                {
                    // 更新剩余冷却秒数用于显示
                    myConfig.currentCoolDown = nextEventSeconds - now;
                    //if (myConfig.currentCoolDown % 5 == 0)
                        notifyStatusUpdate();
                }
            }
            else
            {
                // 正常循环逻辑（吸合/断开）
                if (now >= nextEventSeconds)
                { 
                    if (isValveOn)
                    {
                        // 准备转为断开
                        isValveOn = false;
                        digitalWrite(PIN_MOSFET, HIGH);
                        myConfig.currentCycles++;

                        // 计算下一次断开结束的时刻
                        nextEventSeconds = now + (myConfig.offTime / 1000);

                        // 检查是否该进入冷却
                        if (myConfig.currentCycles >= myConfig.targetCycles)
                        {
                            inCoolDown = true;
                            nextEventSeconds = now + ((uint32_t)myConfig.coolDownTime * 3600);
                            smartSave(true);
                        }
                        else
                        {
                            smartSave(false);
                        }
                    }
                    else
                    {
                        // 准备转为吸合
                        isValveOn = true;
                        digitalWrite(PIN_MOSFET, LOW);
                        Serial.printf("xxxxxx");
                        // 计算下一次吸合结束的时刻
                        nextEventSeconds = now + (myConfig.onTime / 1000);
                    }
                    notifyStatusUpdate();
                }
            }
        }

        // 每10秒打印一次状态
        if (now % 10 == 0)
        {
            Serial.printf("[RTC] 阶段:%d | 循环:%d/%d | 目标时刻:%u | 当前:%u\n",
                          myConfig.currentStage, myConfig.currentCycles, myConfig.targetCycles, nextEventSeconds, now);
        }
    }
    else if (myConfig.status == PAUSED)
    {
        digitalWrite(PIN_MOSFET, HIGH);                   // 暂停时关闭电磁阀
        pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // 红色代表暂停
        pixels.show();
    }
    else if (myConfig.status == STANDBY)
    {
        digitalWrite(PIN_MOSFET, HIGH);
        pixels.setPixelColor(0, pixels.Color(255, 165, 0)); // 橙色代表待机
        pixels.show();
    }
    else if (myConfig.status == COMPLETED)
    {
        // --- 逻辑 A 的显示部分移到这里 ---
        digitalWrite(PIN_MOSFET, HIGH);                       // 确保阀门关闭
        pixels.setPixelColor(0, pixels.Color(255, 255, 255)); // 持续显示白灯
        pixels.show();
    }
}
