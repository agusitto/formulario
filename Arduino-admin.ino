#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

// Firmware del administrador: mantiene vista centralizada de la red ESP-NOW,
// redistribuye información multi-hop y refleja el estado mediante logs y LEDs.

#ifndef NOMBRE_DISPOSITIVO_ADMIN
#define NOMBRE_DISPOSITIVO_ADMIN "Admin"
#endif

#ifndef CANAL_WIFI_ADMIN
#define CANAL_WIFI_ADMIN 6
#endif

#ifndef WIFI_SSID_ADMIN
#define WIFI_SSID_ADMIN "iPhone de mauro"
#endif

#ifndef WIFI_PASSWORD_ADMIN
#define WIFI_PASSWORD_ADMIN "perez123"
#endif

#ifndef URL_API_ADMIN
#define URL_API_ADMIN "https://api.certplag.cl/api/v1/cebox/datos"
#endif

#ifndef OMITIR_VERIFICACION_SSL_ADMIN
#define OMITIR_VERIFICACION_SSL_ADMIN 1
#endif

const uint32_t TIEMPO_ESPERA_CONEXION_WIFI_MS = 10000;
const uint32_t TIEMPO_REINTENTO_WIFI_MS       = 30000;
const uint32_t INTERVALO_ENVIO_API_MS         = 5000;
const uint32_t TIMEOUT_API_SEGUNDOS           = 5;

// ---- Pines de hardware (ajustar según placa) ----
const int  PINES_LED_PRINCIPAL[]  = {2};
const bool LED_ENCIENDE_EN_ALTO[] = {false};
const uint8_t CANTIDAD_LEDS_PRINCIPALES = sizeof(PINES_LED_PRINCIPAL) / sizeof(PINES_LED_PRINCIPAL[0]);

const int  PINES_INDICADOR[] = {2, 5, 19, 23};
const uint8_t CANTIDAD_INDICADORES = sizeof(PINES_INDICADOR) / sizeof(PINES_INDICADOR[0]);

// ---- Parámetros de red y protocolo ----
const int   MAXIMO_NODOS              = 10;
const int   LONGITUD_MAXIMA_NOMBRE    = 16;
const char  VERSION_MENSAJE           = 4;

const uint32_t TIEMPO_REBOTE_MS       = 1000;
const uint32_t INTERVALO_REFRESCO_MS  = 8000;
const uint32_t INTERVALO_LATIDO_MS    = 5000;
const uint32_t TIEMPO_PRUNEO_MS       = 10000;
const uint32_t INTERVALO_IMPRESION_MS = 5000;

const uint8_t  ROL_ADMINISTRADOR      = 0;
const uint8_t  ROL_NODO_SIMPLE        = 1;
const uint8_t  ROL_DESCONOCIDO        = 0xFF;
const uint8_t  BANDERA_FORZADO_ACTIVO = 0x01;

const uint8_t  MAX_SALTOS_RESUMEN        = 3;
const uint8_t  MAX_REGISTROS_POR_MENSAJE = 8;

// Estructuras compactas utilizadas para serializar latidos y resúmenes.
struct CabeceraMensaje {
  char version;
  char tipo;
} __attribute__((packed));

struct MensajeLatido {
  CabeceraMensaje cabecera;
  char     nombre[LONGITUD_MAXIMA_NOMBRE];
  uint32_t milisegundosDesdeInicio;
  uint32_t consecutivoLatido;
  uint8_t  rolEmisor;
  uint8_t  banderas;
  uint8_t  reservado1;
  uint8_t  reservado2;
} __attribute__((packed));

struct RegistroResumenNodoDifundido {
  char     nombre[LONGITUD_MAXIMA_NOMBRE];
  uint16_t segundosEncendido;
  uint16_t edadLatidoSegundos;
  uint16_t edadContactoDirectoSegundos;
  uint16_t ultimaSecuencia;
  uint8_t  rolReportado;
  uint8_t  saltosReportados;
  uint8_t  banderas;
  uint8_t  reservado;
} __attribute__((packed));

struct MensajeResumenRed {
  CabeceraMensaje cabecera;
  char     emisor[LONGITUD_MAXIMA_NOMBRE];
  uint8_t  cantidadRegistros;
  uint8_t  reservado;
  RegistroResumenNodoDifundido registros[MAX_REGISTROS_POR_MENSAJE];
} __attribute__((packed));

struct RegistroNodo {
  char     nombre[LONGITUD_MAXIMA_NOMBRE];
  uint8_t  mac[6];
  bool     macValida;
  bool     estaActivo;
  uint8_t  rolReportado;
  uint8_t  saltos;
  uint8_t  banderas;
  uint32_t instantePrimeraSeñal;
  uint32_t instanteUltimoLatido;
  uint32_t instanteUltimoContactoDirecto;
  uint32_t instanteUltimaActualizacionResumen;
  uint32_t ultimoConsecutivoLatido;
  uint32_t tiempoEncendidoRemotoMs;
};

RegistroNodo tablaNodos[MAXIMO_NODOS];
int          cantidadNodos = 0;
volatile bool hayDifusionPendiente = true;
unsigned long momentoUltimoEnvio = 0;
uint32_t     contadorLatidosLocales = 0;
uint8_t      macLocal[6] = {0};
uint8_t      indiceResumen = 0;

bool            wifiInicializado = false;
unsigned long   momentoUltimoIntentoWifi = 0;
unsigned long   momentoUltimoEnvioApi = 0;
String          ultimoPayloadSerializado;
WiFiClientSecure clienteHttps;

unsigned long momentoUltimoLatido = 0;
unsigned long momentoUltimaPoda = 0;
unsigned long momentoUltimoResumen = 0;

bool          destelloActivo = false;
bool          destelloLedEncendido = false;
uint8_t       destellosPendientes = 0;
unsigned long momentoCambioDestello = 0;
unsigned long momentoProximaSecuencia = 0;

int ultimoConteoIndicadores = -1;

// ---- Prototipos de funciones internas ----
void encenderLedPrincipal();
void apagarLedPrincipal();
void configurarPines();
void autopruebaLed();
void actualizarIndicadores(int cantidad);
void actualizarIndicadoresSegunActivos();
void enviarLatido(unsigned long ahora);
void difundirResumen(unsigned long ahora, bool forzar = false);
void procesarPoda(unsigned long ahora);
void imprimirResumenAdministrador(unsigned long ahora);
void iniciarSecuenciaDestellos(unsigned long ahora, int vivos);
void gestionarDestellos(unsigned long ahora);
void manejarRecepcion(const esp_now_recv_info* info, const uint8_t* datos, int longitud);
void mantenerConexionWifi(unsigned long ahora);
void enviarResumenApi(unsigned long ahora);
String construirPayloadJson(unsigned long ahora);
String escaparJson(const char* texto);
bool esUrlHttps(const char* url);

RegistroNodo* obtenerRegistro(const char* nombre, const uint8_t* mac, bool crearSiNoExiste);
bool actualizarConLatido(RegistroNodo& registro, const MensajeLatido& latido, unsigned long ahora);
bool actualizarConResumen(RegistroNodo& registro, const RegistroResumenNodoDifundido& dato, unsigned long ahora);
void prepararRegistroResumen(const RegistroNodo& registro, unsigned long ahora, RegistroResumenNodoDifundido& destino);

void onReceive(const esp_now_recv_info* info, const uint8_t* datos, int longitud){
  manejarRecepcion(info, datos, longitud);
}

// Configura radio, ESP-NOW y el estado inicial de indicadores/tabla.
void setup(){
  Serial.begin(115200);
  delay(100);

  configurarPines();
  autopruebaLed();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  momentoUltimoIntentoWifi = millis() - TIEMPO_REINTENTO_WIFI_MS;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CANAL_WIFI_ADMIN, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  esp_wifi_get_mac(WIFI_IF_STA, macLocal);

  if(esp_now_init() != ESP_OK){
    delay(1000);
    ESP.restart();
  }

  esp_now_register_send_cb(nullptr);
  esp_now_register_recv_cb(onReceive);

  esp_now_peer_info_t peerDifusion;
  memset(&peerDifusion, 0, sizeof(peerDifusion));
  const uint8_t direccionDifusion[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  memcpy(peerDifusion.peer_addr, direccionDifusion, 6);
  peerDifusion.ifidx = WIFI_IF_STA;
  peerDifusion.channel = CANAL_WIFI_ADMIN;
  peerDifusion.encrypt = false;
  if(esp_now_add_peer(&peerDifusion) != ESP_OK){
    delay(1000);
    ESP.restart();
  }

  hayDifusionPendiente = true;
  momentoUltimoEnvio = millis();
  momentoUltimoLatido = millis();
  momentoUltimaPoda = millis();
  momentoUltimoResumen = millis();
}

// Orquesta el intercambio periódico de mensajes y la señalización visual.
void loop(){
  unsigned long ahora = millis();

  if((ahora - momentoUltimoLatido) >= INTERVALO_LATIDO_MS){
    enviarLatido(ahora);
    momentoUltimoLatido = ahora;
  }

  if((ahora - momentoUltimaPoda) >= 1000UL){
    procesarPoda(ahora);
    momentoUltimaPoda = ahora;
  }

  if(hayDifusionPendiente && (ahora - momentoUltimoEnvio) >= TIEMPO_REBOTE_MS){
    difundirResumen(ahora);
  } else if((ahora - momentoUltimoEnvio) >= INTERVALO_REFRESCO_MS){
    difundirResumen(ahora, true);
  }

  if((ahora - momentoUltimoResumen) >= INTERVALO_IMPRESION_MS){
    imprimirResumenAdministrador(ahora);
    momentoUltimoResumen = ahora;
  }

  mantenerConexionWifi(ahora);
  if((ahora - momentoUltimoEnvioApi) >= INTERVALO_ENVIO_API_MS){
    enviarResumenApi(ahora);
    momentoUltimoEnvioApi = ahora;
  }

  gestionarDestellos(ahora);
}

// Define pines como salidas y deja LEDs en estado conocido.
void configurarPines(){
  for(uint8_t i=0;i<CANTIDAD_LEDS_PRINCIPALES;i++){
    pinMode(PINES_LED_PRINCIPAL[i], OUTPUT);
  }
  for(uint8_t i=0;i<CANTIDAD_INDICADORES;i++){
    pinMode(PINES_INDICADOR[i], OUTPUT);
    digitalWrite(PINES_INDICADOR[i], LOW);
  }
  apagarLedPrincipal();
}

// Breve rutina de encendido/apagado para confirmar funcionamiento del LED.
void autopruebaLed(){
  for(int k=0;k<3;k++){
    encenderLedPrincipal();
    delay(80);
    apagarLedPrincipal();
    delay(80);
  }
}

void encenderLedPrincipal(){
  for(uint8_t i=0;i<CANTIDAD_LEDS_PRINCIPALES;i++){
    digitalWrite(PINES_LED_PRINCIPAL[i], LED_ENCIENDE_EN_ALTO[i] ? HIGH : LOW);
  }
}

void apagarLedPrincipal(){
  for(uint8_t i=0;i<CANTIDAD_LEDS_PRINCIPALES;i++){
    digitalWrite(PINES_LED_PRINCIPAL[i], LED_ENCIENDE_EN_ALTO[i] ? LOW : HIGH);
  }
}

// Ajusta la barra de indicadores según la cantidad de vecinos activos.
void actualizarIndicadores(int cantidad){
  int normalizado = cantidad;
  if(normalizado < 0) normalizado = 0;
  if(normalizado > CANTIDAD_INDICADORES) normalizado = CANTIDAD_INDICADORES;
  if(normalizado == ultimoConteoIndicadores) return;
  ultimoConteoIndicadores = normalizado;
  for(uint8_t i=0;i<CANTIDAD_INDICADORES;i++){
    digitalWrite(PINES_INDICADOR[i], (i < normalizado) ? HIGH : LOW);
  }
}

// Recalcula cuántos vecinos activos quedan y refleja el resultado en LEDs.
void actualizarIndicadoresSegunActivos(){
  int vivos = 0;
  for(int i=0;i<cantidadNodos;i++){
    if(!tablaNodos[i].estaActivo) continue;
    if(strcmp(tablaNodos[i].nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    vivos++;
  }
  actualizarIndicadores(vivos);
}

// Devuelve (o crea) la entrada asociada a un nombre/MAC para mantener la tabla.
RegistroNodo* obtenerRegistro(const char* nombre, const uint8_t* mac, bool crearSiNoExiste){
  int indice = -1;
  if(mac){
    for(int i=0;i<cantidadNodos;i++){
      if(tablaNodos[i].macValida && memcmp(tablaNodos[i].mac, mac, 6) == 0){
        indice = i;
        break;
      }
    }
  }
  if(indice < 0){
    for(int i=0;i<cantidadNodos;i++){
      if(strcmp(tablaNodos[i].nombre, nombre) == 0){
        indice = i;
        break;
      }
    }
  }
  if(indice < 0 && crearSiNoExiste){
    if(cantidadNodos >= MAXIMO_NODOS) return nullptr;
    indice = cantidadNodos++;
    RegistroNodo& nuevo = tablaNodos[indice];
    memset(&nuevo, 0, sizeof(RegistroNodo));
    strncpy(nuevo.nombre, nombre, LONGITUD_MAXIMA_NOMBRE-1);
    nuevo.nombre[LONGITUD_MAXIMA_NOMBRE-1] = '\0';
    nuevo.saltos = 0xFF;
    nuevo.rolReportado = ROL_DESCONOCIDO;
  }
  if(indice < 0) return nullptr;
  if(mac){
    memcpy(tablaNodos[indice].mac, mac, 6);
    tablaNodos[indice].macValida = true;
  }
  return &tablaNodos[indice];
}

// Integra un latido recibido y devuelve si el estado cambió lo suficiente.
bool actualizarConLatido(RegistroNodo& registro, const MensajeLatido& latido, unsigned long ahora){
  bool cambio = false;
  if(!registro.estaActivo){
    registro.estaActivo = true;
    cambio = true;
  }
  if(registro.saltos != 0){
    registro.saltos = 0;
    cambio = true;
  }
  if(registro.rolReportado != latido.rolEmisor){
    registro.rolReportado = latido.rolEmisor;
    cambio = true;
  }
  if(registro.banderas != latido.banderas){
    registro.banderas = latido.banderas;
    cambio = true;
  }
  if(registro.ultimoConsecutivoLatido != latido.consecutivoLatido){
    registro.ultimoConsecutivoLatido = latido.consecutivoLatido;
    cambio = true;
  }
  if(registro.tiempoEncendidoRemotoMs != latido.milisegundosDesdeInicio){
    registro.tiempoEncendidoRemotoMs = latido.milisegundosDesdeInicio;
    cambio = true;
  }
  registro.instanteUltimoLatido = ahora;
  registro.instanteUltimaActualizacionResumen = ahora;
  registro.instanteUltimoContactoDirecto = ahora;
  if(registro.instantePrimeraSeñal == 0){
    registro.instantePrimeraSeñal = ahora;
  }
  return cambio;
}

// Integra un resumen multi-hop teniendo en cuenta saltos y vigencia.
bool actualizarConResumen(RegistroNodo& registro, const RegistroResumenNodoDifundido& dato, unsigned long ahora){
  if(dato.saltosReportados >= MAX_SALTOS_RESUMEN) return false;
  uint8_t nuevosSaltos = dato.saltosReportados + 1;
  bool huboCambio = false;

  if(registro.saltos == 0xFF || registro.saltos > nuevosSaltos){
    registro.saltos = nuevosSaltos;
    huboCambio = true;
  }
  if(!registro.estaActivo){
    registro.estaActivo = true;
    huboCambio = true;
  }

  uint32_t edadLatidoMs = (dato.edadLatidoSegundos == 0xFFFF) ? 0xFFFFFFFFu : (uint32_t)dato.edadLatidoSegundos * 1000UL;
  unsigned long instanteEstimado = (edadLatidoMs > ahora) ? 0 : (ahora - edadLatidoMs);
  if(instanteEstimado > registro.instanteUltimoLatido){
    registro.instanteUltimoLatido = instanteEstimado;
    huboCambio = true;
  }
  registro.instanteUltimaActualizacionResumen = ahora;

  if(dato.edadContactoDirectoSegundos != 0xFFFF){
    uint32_t edadDirectoMs = (uint32_t)dato.edadContactoDirectoSegundos * 1000UL;
    unsigned long instanteDirecto = (edadDirectoMs > ahora) ? 0 : (ahora - edadDirectoMs);
    if(instanteDirecto > registro.instanteUltimoContactoDirecto){
      registro.instanteUltimoContactoDirecto = instanteDirecto;
      huboCambio = true;
    }
  }

  uint32_t uptimeMs = (uint32_t)dato.segundosEncendido * 1000UL;
  if(uptimeMs != registro.tiempoEncendidoRemotoMs){
    registro.tiempoEncendidoRemotoMs = uptimeMs;
    huboCambio = true;
  }

  uint32_t nuevaSecuencia = dato.ultimaSecuencia;
  if(nuevaSecuencia != registro.ultimoConsecutivoLatido){
    registro.ultimoConsecutivoLatido = nuevaSecuencia;
    huboCambio = true;
  }

  if(registro.rolReportado != dato.rolReportado){
    registro.rolReportado = dato.rolReportado;
    huboCambio = true;
  }
  if(registro.banderas != dato.banderas){
    registro.banderas = dato.banderas;
    huboCambio = true;
  }

  if(registro.instantePrimeraSeñal == 0 || instanteEstimado < registro.instantePrimeraSeñal){
    registro.instantePrimeraSeñal = instanteEstimado;
    huboCambio = true;
  }

  if(huboCambio && nuevosSaltos < MAX_SALTOS_RESUMEN){
    hayDifusionPendiente = true;
  }
  return huboCambio;
}

// Crea la versión compacta de un registro local para difundirla al resto.
void prepararRegistroResumen(const RegistroNodo& registro, unsigned long ahora, RegistroResumenNodoDifundido& destino){
  strncpy(destino.nombre, registro.nombre, LONGITUD_MAXIMA_NOMBRE-1);
  destino.nombre[LONGITUD_MAXIMA_NOMBRE-1] = '\0';

  uint32_t uptimeSeg = registro.tiempoEncendidoRemotoMs / 1000UL;
  if(uptimeSeg > 0xFFFFu) uptimeSeg = 0xFFFFu;
  destino.segundosEncendido = (uint16_t)uptimeSeg;

  uint32_t edadLatidoSeg = registro.instanteUltimoLatido ? ((ahora - registro.instanteUltimoLatido) / 1000UL) : 0xFFFFu;
  if(edadLatidoSeg > 0xFFFFu) edadLatidoSeg = 0xFFFFu;
  destino.edadLatidoSegundos = (uint16_t)edadLatidoSeg;

  uint32_t edadDirectoSeg = registro.instanteUltimoContactoDirecto ? ((ahora - registro.instanteUltimoContactoDirecto) / 1000UL) : 0xFFFFu;
  if(edadDirectoSeg > 0xFFFFu) edadDirectoSeg = 0xFFFFu;
  destino.edadContactoDirectoSegundos = (uint16_t)edadDirectoSeg;

  destino.ultimaSecuencia = (uint16_t)(registro.ultimoConsecutivoLatido & 0xFFFFu);
  destino.rolReportado = registro.rolReportado;
  destino.saltosReportados = registro.saltos;
  destino.banderas = registro.banderas;
  destino.reservado = 0;
}

// Publica la presencia del administrador para que los nodos calibren alcance.
void enviarLatido(unsigned long ahora){
  MensajeLatido latido;
  memset(&latido, 0, sizeof(latido));
  latido.cabecera.version = VERSION_MENSAJE;
  latido.cabecera.tipo = 'H';
  strncpy(latido.nombre, NOMBRE_DISPOSITIVO_ADMIN, LONGITUD_MAXIMA_NOMBRE-1);
  latido.nombre[LONGITUD_MAXIMA_NOMBRE-1] = '\0';
  latido.milisegundosDesdeInicio = ahora;
  latido.consecutivoLatido = ++contadorLatidosLocales;
  latido.rolEmisor = ROL_ADMINISTRADOR;
  latido.banderas = 0;
  const uint8_t direccionDifusion[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_send(direccionDifusion, reinterpret_cast<const uint8_t*>(&latido), sizeof(latido));
}

// Envía por turnos la tabla de nodos para habilitar el encadenamiento de saltos.
void difundirResumen(unsigned long ahora, bool forzar){
  MensajeResumenRed mensaje;
  memset(&mensaje, 0, sizeof(mensaje));
  mensaje.cabecera.version = VERSION_MENSAJE;
  mensaje.cabecera.tipo = 'L';
  strncpy(mensaje.emisor, NOMBRE_DISPOSITIVO_ADMIN, LONGITUD_MAXIMA_NOMBRE-1);
  mensaje.emisor[LONGITUD_MAXIMA_NOMBRE-1] = '\0';

  if(cantidadNodos == 0){
    hayDifusionPendiente = false;
    momentoUltimoEnvio = ahora;
    return;
  }

  uint8_t incluidos = 0;
  uint8_t procesados = 0;
  if(indiceResumen >= cantidadNodos) indiceResumen = 0;
  uint8_t indice = indiceResumen;

  while(procesados < cantidadNodos && incluidos < MAX_REGISTROS_POR_MENSAJE){
    RegistroNodo& registro = tablaNodos[indice];
    procesados++;
    indice++;
    if(indice >= cantidadNodos) indice = 0;

    if(!registro.estaActivo) continue;
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    if(registro.saltos >= MAX_SALTOS_RESUMEN) continue;

    prepararRegistroResumen(registro, ahora, mensaje.registros[incluidos]);
    incluidos++;
  }

  indiceResumen = indice;

  if(incluidos == 0){
    if(forzar){
      momentoUltimoEnvio = ahora;
    }
    hayDifusionPendiente = false;
    return;
  }

  mensaje.cantidadRegistros = incluidos;
  const uint8_t direccionDifusion[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  size_t tamano = sizeof(CabeceraMensaje) + LONGITUD_MAXIMA_NOMBRE + sizeof(uint8_t)*2 + incluidos * sizeof(RegistroResumenNodoDifundido);
  if(esp_now_send(direccionDifusion, reinterpret_cast<const uint8_t*>(&mensaje), tamano) == ESP_OK){
    momentoUltimoEnvio = ahora;
    hayDifusionPendiente = false;
  }
}

// Desactiva nodos cuyo último latido venció para mantener la tabla sana.
void procesarPoda(unsigned long ahora){
  bool huboCambio = false;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    if(!registro.estaActivo) continue;
    if((ahora - registro.instanteUltimoLatido) >= TIEMPO_PRUNEO_MS){
      registro.estaActivo = false;
      registro.saltos = 0xFF;
      hayDifusionPendiente = true;
      huboCambio = true;
    }
  }
  if(huboCambio){
    actualizarIndicadoresSegunActivos();
  }
}

// Presenta por Serial el panorama actual de la red y rearmar la secuencia LED.
void imprimirResumenAdministrador(unsigned long ahora){
  int vivos = 0;
  for(int i=0;i<cantidadNodos;i++){
    if(!tablaNodos[i].estaActivo) continue;
    if(strcmp(tablaNodos[i].nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    vivos++;
  }
  Serial.print(F("vivos="));
  Serial.print(vivos);
  Serial.print(F(" | "));
  bool esPrimero = true;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(!registro.estaActivo) continue;
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    if(!esPrimero) Serial.print(F(", "));
    Serial.print(registro.nombre);
    Serial.print(F(" (consec="));
    Serial.print(registro.ultimoConsecutivoLatido);
    Serial.print(F("; enc="));
    Serial.print(registro.tiempoEncendidoRemotoMs / 1000.0f, 1);
    Serial.print(F("s; saltos="));
    if(registro.saltos == 0xFF) Serial.print('?'); else Serial.print(registro.saltos);
    Serial.print(F("; interruptor="));
    Serial.print((registro.banderas & BANDERA_FORZADO_ACTIVO) ? F("ACTIVO") : F("INACTIVO"));
    Serial.print(F("; directo="));
    if(registro.instanteUltimoContactoDirecto){
      Serial.print((ahora - registro.instanteUltimoContactoDirecto) / 1000.0f, 1);
      Serial.print(F("s"));
    } else {
      Serial.print(F("N/D"));
    }
    Serial.print(F(")"));
    esPrimero = false;
  }
  Serial.println();
  actualizarIndicadoresSegunActivos();
  if(!destelloActivo){
    iniciarSecuenciaDestellos(ahora, vivos);
  }
}

// Define cuántos destellos mostrará el LED principal según nodos vivos.
void iniciarSecuenciaDestellos(unsigned long ahora, int vivos){
  if(vivos <= 0){
    destelloActivo = false;
    destelloLedEncendido = false;
    destellosPendientes = 0;
    apagarLedPrincipal();
    momentoProximaSecuencia = ahora + 3000;
    return;
  }
  destelloActivo = true;
  destelloLedEncendido = true;
  destellosPendientes = (vivos > 0) ? (uint8_t)(vivos - 1) : 0;
  encenderLedPrincipal();
  momentoCambioDestello = ahora + 500;
}

// Alterna encendido/apagado respetando tiempos de la secuencia vigente.
void gestionarDestellos(unsigned long ahora){
  if(!destelloActivo){
    if(ahora >= momentoProximaSecuencia){
      int vivos = 0;
      for(int i=0;i<cantidadNodos;i++){
        if(!tablaNodos[i].estaActivo) continue;
        if(strcmp(tablaNodos[i].nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
        vivos++;
      }
      iniciarSecuenciaDestellos(ahora, vivos);
    }
    return;
  }

  if((int32_t)(ahora - momentoCambioDestello) < 0) return;

  if(destelloLedEncendido){
    apagarLedPrincipal();
    destelloLedEncendido = false;
    momentoCambioDestello = ahora + 150;
  } else {
    if(destellosPendientes > 0){
      encenderLedPrincipal();
      destelloLedEncendido = true;
      destellosPendientes--;
      momentoCambioDestello = ahora + 500;
    } else {
      destelloActivo = false;
      apagarLedPrincipal();
      momentoProximaSecuencia = ahora + 3000;
    }
  }
}

// Normaliza latidos y resúmenes recibidos, disparando difusiones ante actividad ajena.
void manejarRecepcion(const esp_now_recv_info* info, const uint8_t* datos, int longitud){
  if(!info || longitud < (int)sizeof(CabeceraMensaje)) return;
  CabeceraMensaje cabecera;
  memcpy(&cabecera, datos, sizeof(CabeceraMensaje));
  if(cabecera.version != VERSION_MENSAJE) return;

  bool esPropio = memcmp(info->src_addr, macLocal, 6) == 0;
  unsigned long ahora = millis();

  if(cabecera.tipo == 'H'){
    if(longitud < (int)sizeof(MensajeLatido)) return;
    MensajeLatido latido;
    memcpy(&latido, datos, sizeof(MensajeLatido));
    RegistroNodo* registro = obtenerRegistro(latido.nombre, info->src_addr, true);
    if(!registro) return;
    actualizarConLatido(*registro, latido, ahora);
    if(!esPropio){
      // Reforzamos la difusión inmediata al detectar latidos de terceros.
      hayDifusionPendiente = true;
      if((ahora - momentoUltimoEnvio) < TIEMPO_REBOTE_MS){
        momentoUltimoEnvio = (ahora >= TIEMPO_REBOTE_MS) ? (ahora - TIEMPO_REBOTE_MS) : 0;
      }
    }
    actualizarIndicadoresSegunActivos();
  } else if(cabecera.tipo == 'L'){
    if(longitud < (int)(sizeof(CabeceraMensaje) + LONGITUD_MAXIMA_NOMBRE + 2)) return;
    MensajeResumenRed resumen;
    memset(&resumen, 0, sizeof(resumen));
    size_t bytesACopiar = longitud;
    if(bytesACopiar > sizeof(resumen)) bytesACopiar = sizeof(resumen);
    memcpy(&resumen, datos, bytesACopiar);
    uint8_t cantidad = resumen.cantidadRegistros;
    if(cantidad > MAX_REGISTROS_POR_MENSAJE) cantidad = MAX_REGISTROS_POR_MENSAJE;
    bool huboCambio = false;
    bool seRecibioResumenAjeno = !esPropio && cantidad > 0;
    for(uint8_t i=0;i<cantidad;i++){
      RegistroResumenNodoDifundido& dato = resumen.registros[i];
      if(!dato.nombre[0]) continue;
      if(strcmp(dato.nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
      RegistroNodo* registro = obtenerRegistro(dato.nombre, nullptr, true);
      if(!registro) continue;
      if(actualizarConResumen(*registro, dato, ahora)){
        huboCambio = true;
      }
    }
    if(huboCambio || seRecibioResumenAjeno){
      // Rebroadcast rápido ante actividad externa para mantener la malla.
      hayDifusionPendiente = true;
      if((ahora - momentoUltimoEnvio) < TIEMPO_REBOTE_MS){
        momentoUltimoEnvio = (ahora >= TIEMPO_REBOTE_MS) ? (ahora - TIEMPO_REBOTE_MS) : 0;
      }
      if(huboCambio){
        actualizarIndicadoresSegunActivos();
      }
    }
  }
}

void mantenerConexionWifi(unsigned long ahora){
  if(WIFI_SSID_ADMIN[0] == '\0'){
    return;
  }

  if(WiFi.status() == WL_CONNECTED){
    wifiInicializado = true;
    return;
  }

  wifiInicializado = false;
  if((ahora - momentoUltimoIntentoWifi) < TIEMPO_REINTENTO_WIFI_MS){
    return;
  }
  momentoUltimoIntentoWifi = ahora;

  Serial.print(F("Conectando a WiFi: "));
  Serial.println(WIFI_SSID_ADMIN);
  WiFi.begin(WIFI_SSID_ADMIN, WIFI_PASSWORD_ADMIN);

  unsigned long inicio = millis();
  while(WiFi.status() != WL_CONNECTED && (millis() - inicio) < TIEMPO_ESPERA_CONEXION_WIFI_MS){
    delay(100);
  }

  if(WiFi.status() == WL_CONNECTED){
    wifiInicializado = true;
    if(OMITIR_VERIFICACION_SSL_ADMIN && esUrlHttps(URL_API_ADMIN)){
      clienteHttps.setInsecure();
    }
  } else {
    Serial.println(F("No se logró conectar a WiFi para API"));
    WiFi.disconnect(true, true);
  }
}

void enviarResumenApi(unsigned long ahora){
  if(WIFI_SSID_ADMIN[0] == '\0'){
    return;
  }
  if(WiFi.status() != WL_CONNECTED){
    return;
  }

  String payload = construirPayloadJson(ahora);
  if(payload == ultimoPayloadSerializado){
    return;
  }

  HTTPClient http;
  http.setTimeout(TIMEOUT_API_SEGUNDOS * 1000);

  bool usaHttps = esUrlHttps(URL_API_ADMIN);
  bool inicioOk;
  if(usaHttps){
    inicioOk = http.begin(clienteHttps, URL_API_ADMIN);
  } else {
    inicioOk = http.begin(URL_API_ADMIN);
  }
  if(!inicioOk){
    Serial.println(F("Error iniciando conexión HTTP/HTTPS"));
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int codigo = http.POST(payload);
  if(codigo > 0){
    Serial.print(F("API respuesta: "));
    Serial.println(codigo);
    if(codigo >= 200 && codigo < 300){
      ultimoPayloadSerializado = payload;
    }
  } else {
    Serial.print(F("Error POST API: "));
    Serial.println(http.errorToString(codigo));
  }
  http.end();
}

String construirPayloadJson(unsigned long ahora){
  String json = F("{");
  json += F("\"nombre\":\"");
  json += escaparJson(NOMBRE_DISPOSITIVO_ADMIN);
  json += F("\",\"ts\":");
  json += String(ahora);
  json += F(",\"vivos\":");

  int vivos = 0;
  for(int i=0;i<cantidadNodos;i++){
    if(!tablaNodos[i].estaActivo) continue;
    if(strcmp(tablaNodos[i].nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    vivos++;
  }
  json += vivos;

  json += F(",\"nodos\":[");
  bool primero = true;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(!registro.estaActivo) continue;
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_ADMIN) == 0) continue;
    if(!primero) json += ',';
    primero = false;
    json += F("{");
    json += F("\"nombre\":\"");
    json += escaparJson(registro.nombre);
    json += F("\",\"consecutivo\":");
    json += registro.ultimoConsecutivoLatido;
    json += F(",\"encendido_seg\":");
    json += String(registro.tiempoEncendidoRemotoMs / 1000.0f, 1);
    json += F(",\"saltos\":");
    if(registro.saltos == 0xFF){
      json += F("null");
    } else {
      json += registro.saltos;
    }
    json += F(",\"interruptor\":\"");
    json += (registro.banderas & BANDERA_FORZADO_ACTIVO) ? F("ACTIVO") : F("INACTIVO");
    json += F("\",\"directo_seg\":");
    if(registro.instanteUltimoContactoDirecto){
      float segundos = (ahora - registro.instanteUltimoContactoDirecto) / 1000.0f;
      json += String(segundos, 1);
    } else {
      json += F("null");
    }
    json += F("}");
  }
  json += F("]}");
  return json;
}

String escaparJson(const char* texto){
  String resultado;
  while(*texto){
    char c = *texto++;
    switch(c){
      case '"':
        resultado += F("\\\"");
        break;
      case '\\':
        resultado += F("\\\\");
        break;
      case '\b':
        resultado += F("\\b");
        break;
      case '\f':
        resultado += F("\\f");
        break;
      case '\n':
        resultado += F("\\n");
        break;
      case '\r':
        resultado += F("\\r");
        break;
      case '\t':
        resultado += F("\\t");
        break;
      default:
        if(static_cast<uint8_t>(c) < 0x20){
          char buffer[7];
          snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<uint8_t>(c));
          resultado += buffer;
        } else {
          resultado += c;
        }
        break;
    }
  }
  return resultado;
}

bool esUrlHttps(const char* url){
  if(!url) return false;
  if(strncmp(url, "https://", 8) == 0) return true;
  if(strncmp(url, "HTTPS://", 8) == 0) return true;
  return false;
}
