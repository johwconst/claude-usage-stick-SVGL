#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "config.h"

#define MAX_SAVED_NETWORKS 3

class WiFiManager {
public:
    struct NetworkInfo {
        char ssid[33];
        int rssi;
        bool open;
    };

    bool begin() {
        _prefs.begin("wifi", false);
        _loadAll();
        // Scan em TODOS os canais (fast scan para no 1o match e pode pular o AP).
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        _radioOn();
        WiFi.disconnect();
        // Modem sleep default (DTIM) e a causa classica de queda/latencia nesta
        // placa: o radio dorme entre beacons, perde beacon, o AP derruba.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(false);   // nao gravar credencial na NVS do core a cada begin()
        // Motivo real de cada queda: WiFi.status() so diz "desconectado".
        WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
            uint8_t r = info.wifi_sta_disconnected.reason;
            Serial.printf("WiFi: caiu, reason=%u (%s) rssi=%d heap=%u\n", r,
                          WiFi.STA.disconnectReasonName((wifi_err_reason_t)r),
                          (int)info.wifi_sta_disconnected.rssi, (unsigned)ESP.getFreeHeap());
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        return true;
    }

    // Reconexao NAO bloqueante, chamada todo loop(). O autoReconnect do core cobre
    // a queda simples do mesmo AP; isto cobre o AP que sumiu de vez, girando entre
    // as redes salvas. autoConnect() nao serve aqui: bloqueia ate 8s POR rede.
    void maintain() {
        if (WiFi.status() == WL_CONNECTED) { _retryAt = 0; _retryIdx = 0; return; }
        if (_count == 0) return;
        uint32_t now = millis();
        // Primeiro carimbo: janela de graca. O autoReconnect do core ja esta
        // tentando; entrar por cima dele so rende "sta is connecting, cannot set
        // config" e aborta a tentativa que estava em curso.
        if (_retryAt == 0) { _retryAt = now + 30000; return; }
        if ((int32_t)(now - _retryAt) < 0) return;
        Serial.printf("WiFi: reconnect '%s' (status=%d)\n", _nets[_retryIdx].ssid, (int)WiFi.status());
        // Restart completo do STA. disconnect()+begin() nao serve: o disconnect e
        // assincrono, o begin() seguinte leva "sta is connecting, cannot set
        // config" e falha calado, e o evento ASSOC_LEAVE ainda desliga o
        // autoReconnect do core — o radio ficava parado para sempre.
        WiFi.mode(WIFI_OFF);
        _radioOn();                      // setSleep(false) persiste no core
        WiFi.begin(_nets[_retryIdx].ssid, _nets[_retryIdx].pass);
        _retryIdx = (_retryIdx + 1) % _count;
        _retryAt = now + 20000;
    }

    // Tenta cada rede salva até uma conectar.
    //
    // `tick` e chamado durante a espera, a cada ~100ms, com a rede da vez. Existe
    // porque esta funcao bloqueia por ate timeout_ms POR REDE — com as 3 salvas do
    // MAX_SAVED_NETWORKS sao 24s — e quem chama no boot precisa de alguem bombeando
    // o LVGL nesse intervalo, senao a tela fica congelada ate o loop() comecar.
    bool autoConnect(int timeout_ms = 10000,
                     void (*tick)(const char *ssid, int idx, int total) = nullptr) {
        for (int i = 0; i < _count; i++) {
            Serial.printf("WiFi: trying '%s' (%d/%d)...\n", _nets[i].ssid, i + 1, _count);
            WiFi.begin(_nets[i].ssid, _nets[i].pass);

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < (unsigned long)timeout_ms) {
                if (tick) tick(_nets[i].ssid, i + 1, _count);
                delay(100);
            }
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("WiFi: connected to '%s'! IP=%s\n",
                    _nets[i].ssid, WiFi.localIP().toString().c_str());
                if (i > 0) _promote(i);
                return true;
            }
            WiFi.disconnect();
        }
        Serial.println("WiFi: no saved network available");
        return false;
    }

    bool connectTo(const char *ssid, const char *pass, int timeout_ms = 15000) {
        Serial.printf("WiFi: connecting to '%s'...\n", ssid);
        WiFi.begin(ssid, pass);

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < (unsigned long)timeout_ms) {
            delay(100);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("WiFi: connected! IP=%s\n", WiFi.localIP().toString().c_str());
            _addNetwork(ssid, pass);
            return true;
        }
        Serial.println("WiFi: connection failed");
        WiFi.disconnect();
        return false;
    }

    int scanNetworks(NetworkInfo *results, int max_results) {
        int n = WiFi.scanNetworks();
        Serial.printf("WiFi: scan -> %d redes\n", n);
        for (int i = 0; i < n; i++)
            Serial.printf("  ch%2d %4d dBm  %s\n", (int)WiFi.channel(i), (int)WiFi.RSSI(i), WiFi.SSID(i).c_str());
        int count = min(n, max_results);
        for (int i = 0; i < count; i++) {
            strncpy(results[i].ssid, WiFi.SSID(i).c_str(), 32);
            results[i].ssid[32] = '\0';
            results[i].rssi = WiFi.RSSI(i);
            results[i].open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        WiFi.scanDelete();
        return count;
    }

    bool isConnected() { return WiFi.status() == WL_CONNECTED; }
    String getIP() { return WiFi.localIP().toString(); }
    String getSSID() { return WiFi.SSID(); }

    String getSavedSSID() {
        if (_count > 0) return String(_nets[0].ssid);
        return "";
    }

    int getSavedCount() { return _count; }
    const char *getSavedSSID(int idx) {
        if (idx >= 0 && idx < _count) return _nets[idx].ssid;
        return "";
    }

    void disconnect() { WiFi.disconnect(); }

    bool connectSaved(int idx, int timeout_ms = 10000) {
        if (idx < 0 || idx >= _count) return false;
        return connectTo(_nets[idx].ssid, _nets[idx].pass, timeout_ms);
    }

    void forgetNetwork(int idx) {
        if (idx < 0 || idx >= _count) return;
        for (int i = idx; i < _count - 1; i++) _nets[i] = _nets[i + 1];
        _count--;
        _saveAll();
        Serial.printf("WiFi: forgot network at index %d, %d remaining\n", idx, _count);
    }

    void forgetAll() {
        _count = 0;
        _prefs.clear();
        Serial.println("WiFi: all networks forgotten");
    }

private:
    Preferences _prefs;

    struct SavedNet {
        char ssid[33];
        char pass[65];
    };
    SavedNet _nets[MAX_SAVED_NETWORKS];
    int _count = 0;
    uint32_t _retryAt = 0;
    int _retryIdx = 0;

    // Liga o STA com pais BR (canais 1-13). O default do IDF e "01" (1-11): AP
    // no canal 12/13 nao aparece no scan -> NO_AP_FOUND (201) em loop.
    // 802.11d DESLIGADO de proposito: ligado, o ESP adota o pais anunciado no
    // beacon dos APs vizinhos (ex.: "US", 1-11) e perde o canal 13 de novo.
    // Reaplicar a cada mode(WIFI_STA): o mode(WIFI_OFF) desfaz.
    static void _radioOn() {
        WiFi.mode(WIFI_STA);
        esp_err_t e = esp_wifi_set_country_code(WIFI_COUNTRY_CODE, false);
        char cc[3] = {0};
        esp_wifi_get_country_code(cc);
        Serial.printf("WiFi: pais=%s (%s)\n", cc, esp_err_to_name(e));
    }

    void _loadAll() {
        _count = _prefs.getInt("count", 0);
        if (_count > MAX_SAVED_NETWORKS) _count = MAX_SAVED_NETWORKS;
        for (int i = 0; i < _count; i++) {
            char ks[8], kp[8];
            snprintf(ks, sizeof(ks), "s%d", i);
            snprintf(kp, sizeof(kp), "p%d", i);
            String s = _prefs.getString(ks, "");
            String p = _prefs.getString(kp, "");
            strncpy(_nets[i].ssid, s.c_str(), 32); _nets[i].ssid[32] = '\0';
            strncpy(_nets[i].pass, p.c_str(), 64); _nets[i].pass[64] = '\0';
        }
    }

    void _saveAll() {
        _prefs.putInt("count", _count);
        for (int i = 0; i < _count; i++) {
            char ks[8], kp[8];
            snprintf(ks, sizeof(ks), "s%d", i);
            snprintf(kp, sizeof(kp), "p%d", i);
            _prefs.putString(ks, _nets[i].ssid);
            _prefs.putString(kp, _nets[i].pass);
        }
    }

    void _addNetwork(const char *ssid, const char *pass) {
        for (int i = 0; i < _count; i++) {
            if (strcmp(_nets[i].ssid, ssid) == 0) {
                strncpy(_nets[i].pass, pass, 64);
                if (i > 0) _promote(i);
                _saveAll();
                return;
            }
        }
        int slots = min(_count + 1, MAX_SAVED_NETWORKS);
        for (int i = slots - 1; i > 0; i--) {
            _nets[i] = _nets[i - 1];
        }
        strncpy(_nets[0].ssid, ssid, 32); _nets[0].ssid[32] = '\0';
        strncpy(_nets[0].pass, pass, 64); _nets[0].pass[64] = '\0';
        _count = slots;
        _saveAll();
    }

    void _promote(int idx) {
        if (idx <= 0 || idx >= _count) return;
        SavedNet tmp = _nets[idx];
        for (int i = idx; i > 0; i--) _nets[i] = _nets[i - 1];
        _nets[0] = tmp;
        _saveAll();
    }
};

extern WiFiManager g_wifi;

#endif // WIFI_MANAGER_H
