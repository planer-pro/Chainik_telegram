#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>

#define BOT_POLLING_INTERVAL_MS 1000
#define OTA_HOSTNAME "Teapot"

#define AVEARGE_COUNTS 10
#define HYSTERESIS 10

#define LED_PIN LED_BUILTIN // LED pin
#define HEATER_PIN D6
#define BUTT_PIN D2
#define NTC_PIN A0

enum
{
    OFF,
    HOT,
    TERMO
};

String botToken;
String chatId;
String welcome;
uint32_t ledTm = 0;
volatile uint32_t btnPressedTime = 0; // for calculate hold btn time
volatile bool btnFlag = false;        // if btn activated
volatile uint8_t currentMode = OFF;
int16_t an = 0;
uint8_t hotVal = 100;
uint8_t termoVal = 80;

WiFiClientSecure secured_client;
UniversalTelegramBot *bot;

void IRAM_ATTR btnIrq();
void loadConfig();
void saveConfig(const char *token, const char *chat);
void setupWiFiManager();
void setupOTA();
void handleNewMessages(int numNewMessages);
void parseCommand(String command);
void saveLastMessageId(long id); // <<< ИЗМЕНЕНИЕ: Добавлен прототип функции сохранения ID
long loadLastMessageId();        // <<< ИЗМЕНЕНИЕ: Добавлен прототип функции загрузки ID
void getTempData();
void buttonHandler();
void telegramHandler();
void heaterHandler();
// void ledHandler();
void setHeaterHot();
void setHeaterTermo();
void setHeaterOff();

void setup()
{
    delay(1000); // delay to be ready
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nStarting...");

    pinMode(BUTT_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(NTC_PIN, INPUT);
    pinMode(HEATER_PIN, OUTPUT);

    attachInterrupt(digitalPinToInterrupt(BUTT_PIN), btnIrq, CHANGE); // attach button interrupt

    digitalWrite(HEATER_PIN, LOW); // heater off by default
    digitalWrite(LED_PIN, LOW);    // Led on, start configuring mode

    if (!LittleFS.begin())
    {
        Serial.println("Failed to mount file system");
        return;
    }
    Serial.println("File system mounted.");

    loadConfig();
    setupWiFiManager();

    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    secured_client.setInsecure();

    if (botToken.length() > 0)
    {
        bot = new UniversalTelegramBot(botToken, secured_client);

        // <<< ИЗМЕНЕНИЕ: Загружаем ID последнего обработанного сообщения
        long last_id = loadLastMessageId();
        if (last_id > 0)
        {
            bot->last_message_received = last_id;
            Serial.printf("Last message ID loaded: %ld\n", last_id);
        }
    }
    else
        Serial.println("Bot Token is not set. Telegram bot disabled.");

    setupOTA();

    Serial.println("OTA Ready");

    if (bot)
    {
        welcome = "Welcome to Teapot system!\n\n";
        welcome += "/help - This message\n";
        welcome += "/status - Show device status\n";
        welcome += "/restart - Restart the device\n";
        welcome += "/hot - Hot mode (boil to 100 degree)\n";
        welcome += "/termo - Termo mode (hold 80 degree\n";
        welcome += "/off - All modes OFF\n";
        welcome += "/h50 - Example: h - Hot mode, 50 - heating value\n";
        welcome += "/t50 - Example: t - Termo mode, 50 - heating value\n";

        bot->sendMessage(chatId, welcome, "");
        bot->sendMessage(chatId, String(OTA_HOSTNAME) + " Ready", "");
    }

    digitalWrite(LED_PIN, HIGH); // Led off, ready for work
}

void loop()
{
    ArduinoOTA.handle();

    getTempData();     // get current temp data
    buttonHandler();   // button control handler
    telegramHandler(); // telegram messages handler
    heaterHandler();   // heater process handler
}

void IRAM_ATTR btnIrq() // button interrupt
{
    if (!digitalRead(BUTT_PIN))
    {
        btnPressedTime = millis();
    }
    else
    {
        detachInterrupt(digitalPinToInterrupt(BUTT_PIN)); // for debounce

        if (millis() - btnPressedTime < 500)
        {
            if (currentMode == HOT || currentMode == TERMO)
                currentMode = OFF;
            else if (currentMode == OFF && an < hotVal)
                currentMode = HOT;
        }
        else
        {
            if (currentMode == OFF)
                currentMode = TERMO;
            else
                currentMode = OFF;
        }

        btnFlag = true;
    }
}

void getTempData()
{
    static uint32_t _tmTmp = 0;
    static uint16_t inpVal = 0;

    if (millis() - _tmTmp > 250) // get temp every 0.25 sec
    {
        _tmTmp = millis();

        for (size_t i = 0; i < AVEARGE_COUNTS; i++)
            inpVal += analogRead(NTC_PIN);

        inpVal = (inpVal / AVEARGE_COUNTS / 8.0) /*-3.0*/; // average degrees value and convert to temperature

        an = inpVal;
        inpVal = 0;
    }
}

void buttonHandler()
{
    if (btnFlag)
    {
        btnFlag = false;

        switch (currentMode)
        {
        case HOT:
            hotVal = 100;

            setHeaterHot();

#if DEBUG_TELEGRAM
            Serial.println("Btn HOT mode");
#endif

            break;
        case TERMO:
            termoVal = 80;

            setHeaterTermo();

#if DEBUG_TELEGRAM
            Serial.println("Btn TERMO mode");
#endif

            break;
        case OFF: // if (currentMode == OFF)
            setHeaterOff();

            // #if DEBUG_TELEGRAM
            Serial.println("Btn OFF mode");
            // #endif

            break;
        }

        attachInterrupt(digitalPinToInterrupt(BUTT_PIN), btnIrq, CHANGE);
    }
}

void telegramHandler()
{
    static uint32_t bot_last_time = 0;

    if (bot && millis() > bot_last_time + BOT_POLLING_INTERVAL_MS)
    {
        int numNewMessages = bot->getUpdates(bot->last_message_received + 1);

        while (numNewMessages)
        {
            handleNewMessages(numNewMessages);
            numNewMessages = bot->getUpdates(bot->last_message_received + 1);
        }
        bot_last_time = millis();
    }
}

void handleNewMessages(int numNewMessages)
{
#if DEBUG_TELEGRAM
    Serial.print("Got ");
    Serial.print(numNewMessages);
    Serial.println(" new messages");
#endif

    for (int i = 0; i < numNewMessages; i++)
    {
        if (bot->messages[i].chat_id == chatId)
        {
            parseCommand(bot->messages[i].text);
        }
        else
        {
            bot->sendMessage(bot->messages[i].chat_id, "Sorry, you are not authorized to use this bot.", "");
        }
        bot->last_message_received = bot->messages[i].update_id;
    }
}

void parseCommand(String command)
{
    if (command.equalsIgnoreCase("/help"))
        bot->sendMessage(chatId, welcome, "");
    else if (command.equalsIgnoreCase("/status"))
    {
        String status = "Device Status:\n";
        status += "IP: " + WiFi.localIP().toString() + "\n";
        status += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
        status += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes\n";
        status += "Mode: ";

        switch (currentMode)
        {
        case HOT:
            status += "HOT\n";

            break;
        case TERMO:
            status += "TERMO\n";

            break;
        default:
            status += "OFF\n";

            break;
        }

        status += "Requested temp: ";

        switch (currentMode)
        {
        case HOT:
            status += String(hotVal) + " C\n";
            break;
        case TERMO:
            status += String(termoVal) + " C\n";

            break;
        default:
            status += "-- C\n";

            break;
        }

        status += "Current temp: " + String(an) + " C\n";

        bot->sendMessage(chatId, status, "");
    }
    else if (command.equalsIgnoreCase("/restart"))
    {
        bot->sendMessage(chatId, "Restarting...", "");
        // <<< ИЗМЕНЕНИЕ: Сохраняем ID перед перезагрузкой
        saveLastMessageId(bot->last_message_received);

        delay(1000); // Небольшая задержка, чтобы сообщение успело отправиться

        ESP.restart();
    }
    else if (command.equalsIgnoreCase("/hot"))
    {
        hotVal = 100;

        setHeaterHot();
    }
    else if (command.equalsIgnoreCase("/termo"))
    {
        termoVal = 80;

        setHeaterTermo();
    }
    else if (command.equalsIgnoreCase("/off"))
        setHeaterOff();
    else if (command.startsWith("/t"))
    {
        termoVal = command.substring(2).toInt();

        setHeaterTermo();
    }
    else if (command.startsWith("/h"))
    {
        hotVal = command.substring(2).toInt();

        setHeaterHot();
    }
    else
        bot->sendMessage(chatId, "Invalid command: " + command, "");
}

void heaterHandler()
{
    switch (currentMode)
    {
    case HOT:
        if (an >= hotVal)
        {
            bot->sendMessage(chatId, "Hot complete", "");

            setHeaterOff();
        }

        break;
    case TERMO:
        if (an >= termoVal)
            digitalWrite(HEATER_PIN, LOW);
        else if (an < termoVal - HYSTERESIS)
            digitalWrite(HEATER_PIN, HIGH);

        break;
    }
}

// void ledHandler()
// {
//     static uint8_t ledCnt = 0;

//     switch (currentMode)
//     {
//     case HOT:
//         digitalWrite(LED_PIN, LOW); // enable led
//         break;
//     case TERMO:
//         if (ledCnt == 5)
//         {
//             digitalWrite(LED_PIN, !digitalRead(LED_BUILTIN));
//             ledCnt = 0;
//         }
//         else
//             ledCnt++;
//         break;
//     default:                         // OFF
//         digitalWrite(LED_PIN, HIGH); // LED off
//         break;
//     }

//     timer1_write(12500); // Перезагрузка таймера для 200 мс
// }

void setHeaterHot()
{
    if (termoVal > 0 && termoVal <= 100)
    {
        if (an > hotVal)
            bot->sendMessage(chatId, "Impossible to set HOT mode, value above current temp", "");
        else
        {
            currentMode = HOT;

            digitalWrite(HEATER_PIN, HIGH);

            bot->sendMessage(chatId, "Set HOT mode to " + String(hotVal) + " C", "");
        }
    }
    else
        bot->sendMessage(chatId, "Invalid HOT value", "");
}

void setHeaterTermo()
{
    if (termoVal > 0 && termoVal <= 100)
    {
        currentMode = TERMO;

        if (an > termoVal)
            bot->sendMessage(chatId, "Curent temp above requested", "");
        else
            digitalWrite(HEATER_PIN, HIGH);

        bot->sendMessage(chatId, "Set TERMO mode to " + String(termoVal) + " C", "");
    }
    else
        bot->sendMessage(chatId, "Invalid TERMO value", "");
}

void setHeaterOff()
{
    currentMode = OFF;

    digitalWrite(HEATER_PIN, LOW);

    bot->sendMessage(chatId, "All mods are OFF", "");
}

void setupWiFiManager()
{
    WiFiManager wm;

    char token_buf[64];
    char chat_buf[32];

    botToken.toCharArray(token_buf, sizeof(token_buf));
    chatId.toCharArray(chat_buf, sizeof(chat_buf));

    WiFiManagerParameter custom_bot_token("bot_token", "Telegram Bot Token", token_buf, sizeof(token_buf));
    WiFiManagerParameter custom_chat_id("chat_id", "Telegram Chat ID", chat_buf, sizeof(chat_buf));

    wm.addParameter(&custom_bot_token);
    wm.addParameter(&custom_chat_id);

    wm.setSaveParamsCallback([&]()
                             {
    saveConfig(custom_bot_token.getValue(), custom_chat_id.getValue());
    loadConfig(); });

    wm.setConfigPortalTimeout(180);

    if (!wm.autoConnect("Teapot-Config"))
    {
        Serial.println("Failed to connect and hit timeout");

        delay(3000);

        ESP.restart();

        delay(5000);
    }
}

void loadConfig()
{
    if (LittleFS.exists("/config.json"))
    {
        File configFile = LittleFS.open("/config.json", "r");

        if (configFile)
        {
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, configFile);
            if (!error)
            {
                botToken = doc["bot_token"].as<String>();
                chatId = doc["chat_id"].as<String>();
#if DEBUG_TELEGRAM
                Serial.println("Config loaded:");
                Serial.println("  Token: " + botToken);
                Serial.println("  ChatID: " + chatId);
#endif
            }
            else
            {
#if DEBUG_TELEGRAM
                Serial.println("Failed to load json config");
#endif
            }
            configFile.close();
        }
    }
    else
    {
#if DEBUG_TELEGRAM
        Serial.println("Config file not found.");
#endif
    }
}

void saveConfig(const char *token, const char *chat)
{
    JsonDocument doc;
    doc["bot_token"] = token;
    doc["chat_id"] = chat;

    File configFile = LittleFS.open("/config.json", "w");
    if (!configFile)
    {
#if DEBUG_TELEGRAM
        Serial.println("Failed to open config file for writing");
#endif
        return;
    }

    serializeJson(doc, configFile);
    configFile.close();

#if DEBUG_TELEGRAM
    Serial.println("Config saved.");
#endif
}

void saveLastMessageId(long id)
{
    File file = LittleFS.open("/last_msg_id.txt", "w");

    if (file)
    {
        file.print(id);
        file.close();
#if DEBUG_TELEGRAM
        Serial.println("Saved last message ID: " + String(id));
#endif
    }
    else
    {
#if DEBUG_TELEGRAM
        Serial.println("Failed to open last_msg_id.txt for writing");
#endif
    }
}

long loadLastMessageId()
{
    if (LittleFS.exists("/last_msg_id.txt"))
    {
        File file = LittleFS.open("/last_msg_id.txt", "r");
        if (file)
        {
            String id_str = file.readString();
            file.close();
            if (id_str.length() > 0)
            {
                return atol(id_str.c_str());
            }
        }
    }
    return 0; // Возвращаем 0, если файл не найден или пуст
}

void setupOTA()
{
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    // ArduinoOTA.setPassword("admin");

    ArduinoOTA.onStart([]()
                       {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
      LittleFS.end();
    }
    Serial.println("Start updating " + type); });
    ArduinoOTA.onEnd([]()
                     { Serial.println("\nEnd"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
    ArduinoOTA.onError([](ota_error_t error)
                       {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed"); });

    ArduinoOTA.begin();
}