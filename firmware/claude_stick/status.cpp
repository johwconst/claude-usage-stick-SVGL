#include "status.h"
#include "config.h"
#include "certs.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

bool fetchModelStatus(ModelStatus& out) {
    // Guarda de cadencia AQUI (nao no chamador): status.claude.com e outro host,
    // ou seja, um handshake TLS inteiro (~1-2s) que nao aproveita o keep-alive da
    // api.anthropic.com. Incidente nao muda em 2 min; STATUS_POLL_SEC = 5 min.
    static uint32_t lastOkMs = 0;
    if (lastOkMs != 0 && millis() - lastOkMs < (uint32_t)STATUS_POLL_SEC * 1000)
        return out.ok;                      // mantem o ultimo estado conhecido

    WiFiClientSecure client;
    client.setCACert(CA_BUNDLE);

    HTTPClient https;
    if (!https.begin(client, STATUS_ENDPOINT)) {
        Serial.println("[STATUS] https_init failed");
        return false;
    }

    https.addHeader("User-Agent", "claude-usage-stick/1.0");
    https.setTimeout(API_TIMEOUT_MS);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    Serial.printf("[STATUS] GET %s\n", STATUS_ENDPOINT);
    int code = https.GET();
    Serial.printf("[STATUS] HTTP %d\n", code);

    if (code != 200) {
        https.end();
        return false;   // mantém o último estado conhecido
    }

    int len = https.getSize();
    if (len > 131072) {
        Serial.printf("[STATUS] body too large (%d)\n", len);
        https.end();
        return false;
    }

    String body = https.getString();
    https.end();

    body.toLowerCase();
    out.haikuUp  = body.indexOf("haiku")  < 0;
    out.sonnetUp = body.indexOf("sonnet") < 0;
    out.opusUp   = body.indexOf("opus")   < 0;
    out.fableUp  = body.indexOf("fable")  < 0;
    out.ok = true;
    lastOkMs = millis();

    Serial.printf("[STATUS] haiku:%d sonnet:%d opus:%d fable:%d\n",
                  out.haikuUp, out.sonnetUp, out.opusUp, out.fableUp);
    return true;
}
