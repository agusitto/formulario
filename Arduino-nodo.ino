#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#ifndef NOMBRE_DISPOSITIVO_NODO
#define NOMBRE_DISPOSITIVO_NODO "Nodo 1"
#endif

#ifndef CANAL_WIFI_NODO
#define CANAL_WIFI_NODO 6
#endif

#ifndef PIN_INTERRUPTOR_FORZADO
#define PIN_INTERRUPTOR_FORZADO 15
#endif

const int  PINES_LED_PRINCIPAL[]  = {15};
const bool LED_ENCIENDE_EN_ALTO[] = {false};
const uint8_t CANTIDAD_LEDS_PRINCIPALES = sizeof(PINES_LED_PRINCIPAL) / sizeof(PINES_LED_PRINCIPAL[0]);

const int  PINES_INDICADOR[] = {2, 5, 19, 23};
const uint8_t CANTIDAD_INDICADORES = sizeof(PINES_INDICADOR) / sizeof(PINES_INDICADOR[0]);

const int   MAXIMO_NODOS              = 10;
const int   LONGITUD_MAXIMA_NOMBRE    = 16;
const char  VERSION_MENSAJE           = 4;

const uint32_t TIEMPO_REBOTE_MS       = 1000;
const uint32_t INTERVALO_REFRESCO_MS  = 8000;
const uint32_t INTERVALO_LATIDO_MS    = 5000;
const uint32_t TIEMPO_PRUNEO_MS       = 10000;
const uint32_t INTERVALO_REPORTE_MS   = 7000;

const uint8_t  ROL_ADMINISTRADOR      = 0;
const uint8_t  ROL_NODO_SIMPLE        = 1;
const uint8_t  ROL_DESCONOCIDO        = 0xFF;
const uint8_t  BANDERA_FORZADO_ACTIVO = 0x01;

const uint8_t  MAX_SALTOS_RESUMEN        = 3;
const uint8_t  MAX_REGISTROS_POR_MENSAJE = 8;
const uint32_t DURACION_DESTELLO_RECEPCION_MS   = 250;

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

unsigned long momentoUltimoLatido = 0;
unsigned long momentoUltimaPoda = 0;
unsigned long momentoUltimoReporte = 0;

volatile uint8_t solicitudesRecepcion = 0;
bool forzadoLedActivo = false;
bool parpadeoActivo = false;
unsigned long momentoFinParpadeo = 0;

int ultimoConteoIndicadores = -1;
const uint32_t INTERVALO_LOG_DIFUSION_MS = 1000;
const uint32_t INTERVALO_LOG_TABLA_MS = 1000;
unsigned long ultimoLogDifusion = 0;
unsigned long ultimoLogTabla = 0;

void encenderLedPrincipal();
void apagarLedPrincipal();
void configurarPines();
void autopruebaLed();
void gestionarLedNodo(unsigned long ahora);
void actualizarIndicadores(int cantidad);
void actualizarIndicadoresSegunActivos();
void enviarLatido(unsigned long ahora);
void difundirResumen(unsigned long ahora, bool forzar = false);
void procesarPoda(unsigned long ahora);
void imprimirResumenNodo(unsigned long ahora);
void manejarRecepcion(const esp_now_recv_info* info, const uint8_t* datos, int longitud);
void logEstadoTablaNodos(const char* contexto);

RegistroNodo* obtenerRegistro(const char* nombre, const uint8_t* mac, bool crearSiNoExiste);
bool actualizarConLatido(RegistroNodo& registro, const MensajeLatido& latido, unsigned long ahora);
bool actualizarConResumen(RegistroNodo& registro, const RegistroResumenNodoDifundido& dato, unsigned long ahora);
void prepararRegistroResumen(const RegistroNodo& registro, unsigned long ahora, RegistroResumenNodoDifundido& destino);

void onReceive(const esp_now_recv_info* info, const uint8_t* datos, int longitud){
  manejarRecepcion(info, datos, longitud);
}

// Inicializa radio, pares ESP-NOW y el estado del nodo.
void setup(){
  Serial.begin(115200);
  delay(100);

  configurarPines();
  autopruebaLed();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(CANAL_WIFI_NODO, WIFI_SECOND_CHAN_NONE);
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
  peerDifusion.channel = CANAL_WIFI_NODO;
  peerDifusion.encrypt = false;
  if(esp_now_add_peer(&peerDifusion) != ESP_OK){
    delay(1000);
    ESP.restart();
  }

  hayDifusionPendiente = true;
  momentoUltimoEnvio = millis();
  momentoUltimoLatido = millis();
  momentoUltimaPoda = millis();
  momentoUltimoReporte = millis();
}

// Rutina principal: gestión de latidos, difusiones, podas y LED indicador.
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

  if((ahora - momentoUltimoReporte) >= INTERVALO_REPORTE_MS){
    imprimirResumenNodo(ahora);
    momentoUltimoReporte = ahora;
  }

  gestionarLedNodo(ahora);
}

// Configura entradas/salidas y deja el LED apagado hasta la primera recepción.
void configurarPines(){
  for(uint8_t i=0;i<CANTIDAD_LEDS_PRINCIPALES;i++){
    pinMode(PINES_LED_PRINCIPAL[i], OUTPUT);
  }
  for(uint8_t i=0;i<CANTIDAD_INDICADORES;i++){
    pinMode(PINES_INDICADOR[i], OUTPUT);
    digitalWrite(PINES_INDICADOR[i], LOW);
  }
  pinMode(PIN_INTERRUPTOR_FORZADO, INPUT_PULLUP);
  apagarLedPrincipal();
}

// Autoprueba rápida del LED principal al arrancar.
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

// Controla el LED según el estado del interruptor y los eventos de recepción.
void gestionarLedNodo(unsigned long ahora){
  // El interruptor se considera activo cuando entrega nivel BAJO para encender fijo.
  bool overrideActivo = digitalRead(PIN_INTERRUPTOR_FORZADO) == LOW;

  if(overrideActivo && !forzadoLedActivo){
    forzadoLedActivo = true;
    parpadeoActivo = false;
    encenderLedPrincipal();
  } else if(!overrideActivo && forzadoLedActivo){
    forzadoLedActivo = false;
    apagarLedPrincipal();
  }

  if(forzadoLedActivo) return;

  if(solicitudesRecepcion){
    noInterrupts();
    solicitudesRecepcion = 0;
    interrupts();
    encenderLedPrincipal();
    parpadeoActivo = true;
    momentoFinParpadeo = ahora + DURACION_DESTELLO_RECEPCION_MS;
  }

  if(parpadeoActivo && (int32_t)(ahora - momentoFinParpadeo) >= 0){
    apagarLedPrincipal();
    parpadeoActivo = false;
  }
}

// Actualiza la barra de indicadores acorde a la cantidad de vecinos activos.
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

// Recalcula vecinos activos excluyéndose y actualiza indicadores.
void actualizarIndicadoresSegunActivos(){
  int vivos = 0;
  for(int i=0;i<cantidadNodos;i++){
    if(!tablaNodos[i].estaActivo) continue;
    if(strcmp(tablaNodos[i].nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
    vivos++;
  }
  actualizarIndicadores(vivos);
}

// Busca o crea un registro asociado al nombre/MAC recibido.
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

// Integra la información de un latido directo y señala si hubo cambios relevantes.
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

// Ajusta el registro con datos obtenidos por intermediarios (multi-hop).
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

// Serializa información local para compartirla con vecinos.
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

// Publica el latido del nodo, incluyendo el estado del interruptor forzado.
void enviarLatido(unsigned long ahora){
  MensajeLatido latido;
  memset(&latido, 0, sizeof(latido));
  latido.cabecera.version = VERSION_MENSAJE;
  latido.cabecera.tipo = 'H';
  strncpy(latido.nombre, NOMBRE_DISPOSITIVO_NODO, LONGITUD_MAXIMA_NOMBRE-1);
  latido.nombre[LONGITUD_MAXIMA_NOMBRE-1] = '\0';
  latido.milisegundosDesdeInicio = ahora;
  latido.consecutivoLatido = ++contadorLatidosLocales;
  latido.rolEmisor = ROL_NODO_SIMPLE;
  latido.banderas = forzadoLedActivo ? BANDERA_FORZADO_ACTIVO : 0;
  Serial.print(F("[Nodo] Enviando latido #"));
  Serial.print(latido.consecutivoLatido);
  Serial.print(F(" (forzado="));
  Serial.print(forzadoLedActivo ? F("SI") : F("NO"));
  Serial.println(F(")"));
  const uint8_t direccionDifusion[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_send(direccionDifusion, reinterpret_cast<const uint8_t*>(&latido), sizeof(latido));
}

// Envía la tabla local por turnos para que el admin conozca nodos fuera de alcance.
void difundirResumen(unsigned long ahora, bool forzar){
  MensajeResumenRed mensaje;
  memset(&mensaje, 0, sizeof(mensaje));
  mensaje.cabecera.version = VERSION_MENSAJE;
  mensaje.cabecera.tipo = 'L';
  strncpy(mensaje.emisor, NOMBRE_DISPOSITIVO_NODO, LONGITUD_MAXIMA_NOMBRE-1);
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
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
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
  bool loggearDifusion = (int32_t)(ahora - ultimoLogDifusion) >= (int32_t)INTERVALO_LOG_DIFUSION_MS;
  if(loggearDifusion){
    Serial.print(F("[Nodo] Difundiendo resumen con "));
    Serial.print(incluidos);
    Serial.print(F(" registros (forzar="));
    Serial.print(forzar ? F("SI") : F("NO"));
    Serial.println(F(")"));
  }
  if(esp_now_send(direccionDifusion, reinterpret_cast<const uint8_t*>(&mensaje), tamano) == ESP_OK){
    momentoUltimoEnvio = ahora;
    hayDifusionPendiente = false;
    if(loggearDifusion){
      ultimoLogDifusion = ahora;
    }
  }
}

// Depura nodos sin latidos recientes y provoca nueva difusión.
void procesarPoda(unsigned long ahora){
  bool huboCambio = false;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
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

// Muestra por Serial el conteo de vecinos y la antigüedad de los latidos.
void imprimirResumenNodo(unsigned long ahora){
  Serial.print(F("[Nodo] vecinos="));
  int vivos = 0;
  bool esPrimero = true;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(!registro.estaActivo) continue;
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
    vivos++;
    if(!esPrimero) Serial.print(F(", "));
    Serial.print(registro.nombre);
    Serial.print(F(" (saltos="));
    if(registro.saltos == 0xFF) Serial.print('?'); else Serial.print(registro.saltos);
    Serial.print(F("; hace="));
    Serial.print(registro.instanteUltimoLatido ? ((ahora - registro.instanteUltimoLatido)/1000.0f) : -1.0f, 1);
    Serial.print(F("s)"));
    esPrimero = false;
  }
  Serial.println();
}

// Gestiona cada paquete entrante y dispara eventos para el LED y la difusión.
void manejarRecepcion(const esp_now_recv_info* info, const uint8_t* datos, int longitud){
  if(!info || longitud < (int)sizeof(CabeceraMensaje)) return;
  CabeceraMensaje cabecera;
  memcpy(&cabecera, datos, sizeof(CabeceraMensaje));
  if(cabecera.version != VERSION_MENSAJE) return;

  bool esPropio = memcmp(info->src_addr, macLocal, 6) == 0;
  unsigned long ahora = millis();

  if(!esPropio && solicitudesRecepcion < 250){
    solicitudesRecepcion++;
  }

  if(cabecera.tipo == 'H'){
    if(longitud < (int)sizeof(MensajeLatido)) return;
    MensajeLatido latido;
    memcpy(&latido, datos, sizeof(MensajeLatido));
    RegistroNodo* registro = obtenerRegistro(latido.nombre, info->src_addr, true);
    if(!registro) return;
    bool cambio = actualizarConLatido(*registro, latido, ahora);
    Serial.print(F("[Nodo] Latido recibido de "));
    Serial.print(latido.nombre);
    Serial.print(F(" (forzado="));
    Serial.print((latido.banderas & BANDERA_FORZADO_ACTIVO) ? F("SI") : F("NO"));
    Serial.println(F(")"));
    if(!esPropio){
      // Cualquier latido ajeno provoca una difusión rápida para reforzar los saltos.
      hayDifusionPendiente = true;
      if((ahora - momentoUltimoEnvio) < TIEMPO_REBOTE_MS){
        momentoUltimoEnvio = (ahora >= TIEMPO_REBOTE_MS) ? (ahora - TIEMPO_REBOTE_MS) : 0;
      }
      if(cambio){
        logEstadoTablaNodos("latido");
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
      if(strcmp(dato.nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
      RegistroNodo* registro = obtenerRegistro(dato.nombre, nullptr, true);
      if(!registro) continue;
      if(actualizarConResumen(*registro, dato, ahora)){
        huboCambio = true;
      }
    }
    if(huboCambio || seRecibioResumenAjeno){
      // Al oír resúmenes de terceros volvemos a emitir para mantener vivo el multi-hop.
      hayDifusionPendiente = true;
      if((ahora - momentoUltimoEnvio) < TIEMPO_REBOTE_MS){
        momentoUltimoEnvio = (ahora >= TIEMPO_REBOTE_MS) ? (ahora - TIEMPO_REBOTE_MS) : 0;
      }
      if(huboCambio){
        actualizarIndicadoresSegunActivos();
        logEstadoTablaNodos("resumen");
      }
    }
  }
}

void logEstadoTablaNodos(const char* contexto){
  unsigned long ahora = millis();
  if((int32_t)(ahora - ultimoLogTabla) < (int32_t)INTERVALO_LOG_TABLA_MS){
    return;
  }
  ultimoLogTabla = ahora;
  Serial.print(F("[Nodo] Tabla tras "));
  Serial.print(contexto ? contexto : "evento");
  Serial.print(F(": "));
  if(cantidadNodos == 0){
    Serial.println(F("(vacía)"));
    return;
  }
  bool emitio = false;
  for(int i=0;i<cantidadNodos;i++){
    RegistroNodo& registro = tablaNodos[i];
    if(!registro.estaActivo) continue;
    if(strcmp(registro.nombre, NOMBRE_DISPOSITIVO_NODO) == 0) continue;
    if(emitio) Serial.print(F(", "));
    Serial.print(registro.nombre);
    Serial.print(F(" seq="));
    Serial.print(registro.ultimoConsecutivoLatido);
    Serial.print(F(" saltos="));
    if(registro.saltos == 0xFF) Serial.print('?'); else Serial.print(registro.saltos);
    Serial.print(F(" forzado="));
    Serial.print((registro.banderas & BANDERA_FORZADO_ACTIVO) ? F("SI") : F("NO"));
    emitio = true;
  }
  if(!emitio) Serial.print(F("sin vecinos activos"));
  Serial.println();
}
