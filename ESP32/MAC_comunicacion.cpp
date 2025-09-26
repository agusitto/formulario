#include <esp_now.h>
#include <WiFi.h>

// Cambia la MAC según la placa
// Define aquí la MAC del peer al que quieres enviar mensajes
uint8_t peerAddress[6];

// Estructura de mensaje
struct struct_message {
    char msg[50];
};

struct_message myData;

// Función para leer la MAC por Serial
bool readPeerMAC(uint8_t *mac) {
    Serial.println("Introduce la MAC del peer (formato: XX:XX:XX:XX:XX:XX):");
    while (!Serial.available()) delay(10);
    String macStr = Serial.readStringUntil('\n');
    macStr.trim();
    int values[6];
    if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x", 
        &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)values[i];
        return true;
    }
    Serial.println("❌ Formato MAC inválido.");
    return false;
}

// Callback cuando se recibe un mensaje
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
    struct_message receivedData;
    memcpy(&receivedData, incomingData, sizeof(receivedData));
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("📩 Recibido de %s: %s\n", macStr, receivedData.msg);
}

// Callback cuando se envía un mensaje
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.printf("📤 Estado de envío a %s: %s\n", macStr, status == ESP_NOW_SEND_SUCCESS ? "Éxito" : "Fallo");
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);

    if (!readPeerMAC(peerAddress)) {
        Serial.println("Reinicia y vuelve a intentarlo.");
        while (true) delay(1000);
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ Error inicializando ESP-NOW");
        return;
    }

    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("❌ Error añadiendo peer");
        return;
    }

    Serial.println("✅ ESP-NOW listo y bidireccional");
    Serial.println("Escribe un mensaje y presiona Enter para enviarlo:");
}

void loop() {
    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        input.trim();
        if (input.length() > 0 && input.length() < sizeof(myData.msg)) {
            input.toCharArray(myData.msg, sizeof(myData.msg));
            esp_err_t result = esp_now_send(peerAddress, (uint8_t *)&myData, sizeof(myData));
            if (result != ESP_OK) {
                Serial.println("❌ Error enviando mensaje");
            }
        } else {
            Serial.println("❌ Mensaje vacío o demasiado largo.");
        }
    }
}
