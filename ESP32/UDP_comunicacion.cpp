#include <WiFi.h>
#include <WiFiUdp.h>

const char* ssid = "SSID";
const char* password = "contraseña";

WiFiUDP udp;
const unsigned int udpPort = 4210;

IPAddress getBroadcastAddress() {
    IPAddress ip = WiFi.localIP();
    IPAddress subnet = WiFi.subnetMask();
    IPAddress broadcast;
    for (int i = 0; i < 4; i++) {
        broadcast[i] = ip[i] | (~subnet[i]);
    }
    return broadcast;
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    Serial.print("Conectando WiFi");
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
        if (millis() - start > 15000) {
            Serial.println("\nError: No se pudo conectar a WiFi.");
            return;
        }
    }
    Serial.println();
    Serial.print("IP: "); Serial.println(WiFi.localIP());

    if (!udp.begin(udpPort)) {
        Serial.println("Error: No se pudo iniciar UDP.");
        return;
    }
    Serial.print("UDP listo en puerto "); Serial.println(udpPort);
    Serial.println("Escribe en el Serial y presiona Enter para enviar broadcast.");
}

void loop() {
    // recibir
    int packetSize = udp.parsePacket();
    if (packetSize) {
        char incoming[255];
        int len = udp.read(incoming, sizeof(incoming) - 1);
        if (len > 0) incoming[len] = 0;
        Serial.print("📩 Recibido: ");
        Serial.println(incoming);
    }

    // enviar desde Serial
    if (Serial.available()) {
        String s = Serial.readStringUntil('\n');
        s.trim();
        if (s.length()) {
            IPAddress broadcast = getBroadcastAddress();
            udp.beginPacket(broadcast, udpPort);
            udp.printf("%s: %s", WiFi.macAddress().c_str(), s.c_str());
            udp.endPacket();
            Serial.println("📤 Enviado (broadcast).");
        }
    }
    delay(10);
}