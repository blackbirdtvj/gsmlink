// #define TINY_GSM_RX_BUFFER 4096
#include "main.h"
#include "setup.tpp"
#include "gprs.tpp"
#include "network.tpp"
#include "telegram.tpp"
#include "ota.tpp"

#define REMOTE_DBG(x) sendTelegramMessage(String(BIN_NAME) + " " + x, DEBUG_CHAT_ID, DEBUG_BOT_URL)
Ticker ticker;

#ifdef GSM
void onNewSMS(SMS sms)
{
    for (uint8_t i = 0; i < 3; i++)
    {
        if (sendTelegramMessage("FROM: " + sms.sender + "\n" + sms.date + "\n\n" + sms.message))
            break;
        DEBUG_PRINT("send fail");
        delay(2000);
    }
}

volatile bool ringFlag = true;
volatile bool otaFlag = true;
volatile uint32_t clearMillis;
void IRAM_ATTR ringISR()
{
    ringFlag = true;
    clearMillis = millis();
}
#endif

void syncNTPTime(uint32_t timeout = 10000)
{
    if (isNTPSet)
        return;

    DEBUG_PRINTLN("Trying NTP");
    if (WiFi.status() != WL_CONNECTED)
        return;
    if (!Ping.ping(googleDNS, 3))
        return;

    DEBUG_PRINTLN("Getting time from NTP");
    configTime(12600, 0, "pool.ntp.org", "time.windows.com", "ntp1.ripe.net");

    uint32_t startMillis = millis(); // Store the start time
    while (time(nullptr) <= 100000)
    {
        if (millis() - startMillis >= timeout)
        {
            DEBUG_PRINTLN("NTP sync timed out.");
            return; // Timeout reached
        }

        DEBUG_PRINT(".");
        delay(1000); // Wait before retrying
    }

    time_t tnow = time(nullptr);
    DEBUG_PRINTLN(String(ctime(&tnow)));

    isNTPSet = true; // Set the global flag to true
    return;          // NTP sync successful
}

void setup()
{
#ifdef DEBUG
    SerialMon.begin(115200);
    delay(2000);
#endif

    // uint8_t key[32] = {};
    // uint8_t iv[16] = {};
    // preferences.begin("esp", false);
    // preferences.putBytes("aes_key", key, sizeof(key));
    // preferences.putBytes("aes_iv", iv, sizeof(iv));
    // preferences.end();

    setupDevice();
    //     // TODO if no wifi
    initWiFi();
    DEBUG_PRINTLN();
    DEBUG_PRINTLN(BIN_NAME);
    // Check for panics
    esp_reset_reason_t resetReason = esp_reset_reason();
    DEBUG_PRINT("Reset reason: ");
    DEBUG_PRINTLN(resetReason);

    DEBUG_PRINT("Can rollback: ");
    DEBUG_PRINTLN(Update.canRollBack());

    if (Update.canRollBack() && (resetReason == ESP_RST_PANIC))
    {
        preferences.begin("esp", false);
        preferences.putBool("rollback", true);
        preferences.end();
        DEBUG_PRINTLN("Rolling back");
        Update.rollBack();
        ESP.restart();
    }

    syncNTPTime(30000);
    ArduinoOTA
        .onStart([]()
                 { detachInterrupt(MODEM_RING_PIN); })
        .onEnd([]() {})
        .onError([](ota_error_t error)
                 { attachInterrupt(MODEM_RING_PIN, ringISR, FALLING); });
    ArduinoOTA.begin();

    client.setCACertBundle(rootca_crt_bundle_start);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    ssl.setCACertBundle(rootca_crt_bundle_start);

    // check version
    preferences.begin("esp", false);
    version = preferences.getString("ver", "0");
    bool isRollback = preferences.getBool("rollback");
    preferences.end();
    DEBUG_PRINT("Version: ");
    DEBUG_PRINTLN(version);
    // delay(5000);

#ifdef GSM
    DEBUG_PRINTLN(modem.getSignalQuality());
    attachInterrupt(MODEM_RING_PIN, ringISR, FALLING);
    modemSMS.newSMSCallback = onNewSMS;
    modemSMS.begin();
    modemSMS.removeAll();
    // delay(5000);
    modem.sleepEnable(true);
    ticker.attach(60, []()
                  { ringFlag = true; });

    // esp_sleep_enable_ext0_wakeup((gpio_num_t)MODEM_RING_PIN, 0);
    // esp_sleep_enable_timer_wakeup(60 * 1000000L); // 60 seconds
#endif
    if (isRollback)
    {
        REMOTE_DBG("Version " + version + " panic\nRolled back.");
        preferences.begin("esp", false);
        preferences.putBool("rollback", false);
        preferences.end();
    }
    else
    {
        REMOTE_DBG("Version " + version + "\nRESET: " + resetReason);
    }
    clearMillis = millis();
}

volatile bool call = false;

void handleCall()
{
    if (call)
        return;
    detachInterrupt(MODEM_RING_PIN);
    DEBUG_PRINTLN("is call?");
    String data;
    modem.sendAT("+CLCC");
    modem.waitResponse(9000, data);
    uint16_t i = data.indexOf(GF("+CLCC"));
    if (i == -1)
        return;
    i = data.indexOf("\"", i);
    data = data.substring(i + 1, data.indexOf("\"", i + 1));
    DEBUG_PRINT("Call from: ");
    DEBUG_PRINTLN(data);
    call = true;

    for (i = 0; i < 3; i++)
    {
        if (sendTelegramMessage("Call from: " + data))
            break;
    }

    attachInterrupt(MODEM_RING_PIN, ringISR, FALLING);
}

volatile u16_t counter = 0;
volatile uint8_t updateAttempts = 0;
void loop()
{
    ArduinoOTA.handle();
#ifdef GSM
    if (ringFlag)
    {
        if (digitalRead(MODEM_RING_PIN) == LOW)
        {
            delay(150);
            if (digitalRead(MODEM_RING_PIN) == LOW)
            {
                handleCall();
            }
        }
        else
        {
            delay(120);
            if (digitalRead(MODEM_RING_PIN) == HIGH)
                call = false;
        }
        if (call)
            return;
        DEBUG_PRINTLN("ring");
        ringFlag = false;
        if (wakeModem())
            modemSMS.readAll(onNewSMS, true);
        modem.sleepEnable(true);
        syncNTPTime();

        if (counter++ > 120)
        {
            counter = 0;
            otaFlag = true;
        }
    }
    if (otaFlag)
    {
        if (updateAttempts++ > 3)
        {
            otaFlag = false;
        }
        else if (isNTPSet)
            otaFlag = !doOTA();
    }
    if (millis() - clearMillis > 14400000UL)
    {
        modemSMS.clearPartial();
        clearMillis = millis();
        delay(100);
    }
    // esp_light_sleep_start();
    // if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
    // {
    //     DEBUG_PRINTLN("RING Signal");
    //     ringFlag = true;
    //     clearMillis = millis();
    // }
    // else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER)
    // {
    //     DEBUG_PRINTLN("RING Timer");
    //     ringFlag = true;
    //     if (counter++ > 120)
    //     {
    //         counter = 0;
    //         otaFlag = true;
    //     }
    // }

#endif
}
