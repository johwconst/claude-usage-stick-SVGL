#include "api.h"
#include "config.h"
#include "certs.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#define H5U "anthropic-ratelimit-unified-5h-utilization"
#define H5R "anthropic-ratelimit-unified-5h-reset"
#define H5S "anthropic-ratelimit-unified-5h-status"
#define D7U "anthropic-ratelimit-unified-7d-utilization"
#define D7R "anthropic-ratelimit-unified-7d-reset"
#define D7S "anthropic-ratelimit-unified-7d-status"
#define UST "anthropic-ratelimit-unified-status"
#define URS "anthropic-ratelimit-unified-reset"
#define URC "anthropic-ratelimit-unified-representative-claim"
#define UFB "anthropic-ratelimit-unified-fallback-percentage"
#define UOS "anthropic-ratelimit-unified-overage-status"
#define UOR "anthropic-ratelimit-unified-overage-disabled-reason"

static const char* RL_HEADERS[] = {
    H5U, H5R, H5S, D7U, D7R, D7S, UST, URS, URC, UFB, UOS, UOR
};
static const int RL_HEADER_COUNT = 12;


// Cliente TLS compartilhado entre fetchUsage() e probeModel(). O handshake
// completo (verificacao da cadeia RSA) custa ~1-2s no ESP32-S3; com keep-alive
// so a primeira requisicao de cada sessao paga esse preco. Reinstanciar um
// WiFiClientSecure por chamada tambem realocava ~40KB de heap a cada poll.
static WiFiClientSecure& apiClient() {
    static WiFiClientSecure c;
    static bool ready = false;
    if (!ready) {
        c.setCACert(CA_BUNDLE);
        c.setHandshakeTimeout(10);
        ready = true;
    }
    return c;
}
static HTTPClient g_api;   // objeto persistente: e ele que guarda o socket vivo

void apiClose() {
    g_api.end();
    apiClient().stop();
}

static void apiCommonHeaders(const char* token) {
    g_api.setReuse(true);
    g_api.addHeader("Authorization", String("Bearer ") + token);
    g_api.addHeader("anthropic-version", ANTHROPIC_VERSION);
    g_api.addHeader("anthropic-beta", "oauth-2025-04-20");
    g_api.addHeader("content-type", "application/json");
    g_api.addHeader("User-Agent", "claude-code/2.1.5");
    g_api.setTimeout(API_TIMEOUT_MS);
}

// POST com uma retentativa em socket limpo.
//
// http_-1 (HTTPC_ERROR_CONNECTION_REFUSED) e o erro que mais aparece nessa placa e
// quase nunca e a API: e conexao que nao subiu. Tres origens reais —
//  1. sem WiFi no momento do poll (antes, isso gastava 15s de timeout p/ dar -1);
//  2. keep-alive: o servidor fechou o socket ocioso e o HTTPClient so descobre ao
//     escrever nele;
//  3. heap: o handshake TLS precisa de ~45KB e falhava seco quando cada chamada
//     instanciava seu proprio WiFiClientSecure.
// A retentativa cobre (2) derrubando o socket antes de tentar de novo.
static int apiPost(const char* token, const String& body, bool collectRl) {
    if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_CONNECTION_REFUSED;

    int code = HTTPC_ERROR_CONNECTION_REFUSED;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt == 1) {
            apiClient().stop();                 // forca handshake novo
            delay(150);
        }
        if (!g_api.begin(apiClient(), MESSAGES_ENDPOINT)) continue;
        apiCommonHeaders(token);
        if (collectRl) g_api.collectHeaders(RL_HEADERS, RL_HEADER_COUNT);

        code = g_api.POST(body);
        if (code > 0) return code;

        // O maior bloco contiguo no momento da falha e o dado que distingue
        // "rede caiu" de "handshake TLS nao coube na heap" (~45KB).
        Serial.printf("[API] POST falhou (%d), tentativa %d/2, maior bloco=%u rssi=%d\n",
                      code, attempt + 1,
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                      (int)WiFi.RSSI());
        g_api.end();
    }
    return code;
}

bool fetchUsage(const char* token, UsageData& out) {
    String body = "{\"model\":\"" PROBE_MODEL "\","
                  "\"max_tokens\":1,"
                  "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";

    Serial.printf("[API] POST %s\n", MESSAGES_ENDPOINT);
    int code = apiPost(token, body, true);
    Serial.printf("[API] HTTP %d\n", code);

    if (code <= 0) {
        if (WiFi.status() != WL_CONNECTED) strlcpy(out.error, "sem wifi", sizeof(out.error));
        else snprintf(out.error, sizeof(out.error), "http_%d", code);
        out.ok = false;
        g_api.end();
        return false;
    }

    String h5u = g_api.header(H5U);
    String d7u = g_api.header(D7U);

    if (h5u.length() == 0 && d7u.length() == 0) {
        if (code == 401) strlcpy(out.error, "auth_failed", sizeof(out.error));
        else snprintf(out.error, sizeof(out.error), "no_usage_h_%d", code);
        out.ok = false;
        g_api.getString();
        g_api.end();
        return false;
    }

    out.h5 = h5u.toFloat() * 100.0f;
    out.d7 = d7u.toFloat() * 100.0f;
    out.h5ResetEpoch = (uint32_t)g_api.header(H5R).toInt();
    out.d7ResetEpoch = (uint32_t)g_api.header(D7R).toInt();
    out.unifiedResetEpoch = (uint32_t)g_api.header(URS).toInt();
    out.fallbackPct = g_api.header(UFB).toFloat() * 100.0f;

    strlcpy(out.statusOverall, g_api.header(UST).c_str(), sizeof(out.statusOverall));
    strlcpy(out.status5h,      g_api.header(H5S).c_str(), sizeof(out.status5h));
    strlcpy(out.status7d,      g_api.header(D7S).c_str(), sizeof(out.status7d));
    strlcpy(out.repClaim,      g_api.header(URC).c_str(), sizeof(out.repClaim));
    strlcpy(out.overageStatus, g_api.header(UOS).c_str(), sizeof(out.overageStatus));
    strlcpy(out.overageReason, g_api.header(UOR).c_str(), sizeof(out.overageReason));

    // Drenar o corpo antes do end(): sem isso o HTTPClient guarda um socket com
    // bytes pendentes e a proxima resposta vem corrompida. Corpo e minusculo
    // (max_tokens:1).
    g_api.getString();

    Serial.printf("[API] 5h:%.0f%% (%s)  7d:%.0f%% (%s)  claim:%s  overall:%s\n",
                  out.h5, out.status5h, out.d7, out.status7d, out.repClaim, out.statusOverall);

    g_api.end();
    out.ok = true;
    return true;
}

bool probeModel(const char* token, const char* modelId, ProbeResult& out) {
    String body = String("{\"model\":\"") + modelId + "\","
                  "\"max_tokens\":1,"
                  "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";

    uint32_t t0 = millis();
    int code = apiPost(token, body, false);
    uint32_t dt = millis() - t0;
    if (code > 0) g_api.getString();   // drena p/ manter o keep-alive saudavel
    g_api.end();

    out.code = code;
    out.ms = (dt > 65000) ? 65000 : (uint16_t)dt;
    Serial.printf("[PROBE] %s -> HTTP %d (%ums)\n", modelId, code, (unsigned)dt);
    return code == 200;
}
