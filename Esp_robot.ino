/**
 * @file RobotESP32_Pro_Optimized.ino
 * @brief Control de robot de 2 ruedas (L298N) vía WiFi (AP) con ESP32.
 *
 * @details
 * - Crea un Access Point WiFi.
 * - Sirve una interfaz web (HTML/CSS/JS) almacenada en PROGMEM.
 * - Comunicación principal vía WebSockets con JSON (protocolo optimizado).
 * - Utiliza ArduinoJson para parseo y serialización robusta.
 * - Rutas HTTP de 'fallback' (/cmd, /speed) por si el WS falla.
 * - Control de motores PWM de 10 bits (0-1023) usando ledc.
 * - Manejo de estado interno optimizado (sin 'String') para evitar fragmentación de memoria.
 */

// --- 1. Librerías ---
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// --- 2. Configuración de Red (Access Point) ---
const char* AP_SSID = "RobotESP32_Pro";
const char* AP_PASSWORD = "12345678"; // Mínimo 8 caracteres

// --- 3. Pines de Motores (L298N) ---
// Motor A (Derecha)
const int ENA_PIN = 23;
const int IN1_PIN = 22;
const int IN2_PIN = 21;
// Motor B (Izquierda)
const int ENB_PIN = 5;
const int IN3_PIN = 12;
const int IN4_PIN = 13;

// --- 4. Configuración PWM (ledc) ---
const int LEDC_CHANNEL_A = 0; // Canal ledc para Motor A
const int LEDC_CHANNEL_B = 1; // Canal ledc para Motor B
const int LEDC_FREQ = 1000;   // Frecuencia PWM (1kHz)
const int LEDC_RESOLUTION = 10; // Resolución de 10 bits
const int PWM_MAX = 1023;     // Valor máx. (2^10 - 1)

// --- 5. Estado Global del Robot (Optimizado) ---

/**
 * @brief Enum para el estado de movimiento.
 * Almacenar el estado como un 'enum' (un simple int) es mucho más
 * eficiente y evita la fragmentación de memoria que causa 'String'.
 */
enum MoveDirection {
  DIR_STOP,
  DIR_FORWARD,
  DIR_BACKWARD,
  DIR_LEFT,
  DIR_RIGHT
};

MoveDirection currentDirection = DIR_STOP; // Estado de dirección actual
int currentSpeedPercent = 60;     // Velocidad (0-100 %)
int currentDuty = (currentSpeedPercent * PWM_MAX) / 100; // Duty cycle (0-1023)
bool runningState = false;      // true si NO está en DIR_STOP

// --- 6. Objetos Globales ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Documentos JSON (tamaños ajustados para ahorrar RAM)
// 96 bytes es suficiente para: {"type":"cmd","cmd":"backward"}
StaticJsonDocument<96> jsonDocIn;
// 128 bytes es suficiente para: {"type":"status","running":true,"speed":100,"dir":"Izquierda"}
StaticJsonDocument<128> jsonDocOut;


// --- 7. Funciones Helper y de Estado ---

/**
 * @brief Convierte un porcentaje (0-100) al valor 'duty cycle' (0-1023).
 * @param percent El porcentaje de 0 a 100.
 * @return El valor de 'duty cycle' correspondiente.
 */
int percentToDuty(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return (percent * PWM_MAX) / 100;
}

/**
 * @brief Aplica el valor 'currentDuty' actual a ambos motores.
 */
void applyDuty() {
  ledcWrite(LEDC_CHANNEL_A, currentDuty);
  ledcWrite(LEDC_CHANNEL_B, currentDuty);
}

/**
 * @brief Convierte el enum de estado a una cadena de texto para el JSON.
 * @param dir El estado de movimiento (enum MoveDirection).
 * @return Un puntero a la cadena de texto constante (const char*).
 */
const char* stateToString(MoveDirection dir) {
  switch (dir) {
    case DIR_FORWARD:  return "Adelante";
    case DIR_BACKWARD: return "Atras";
    case DIR_LEFT:     return "Izquierda";
    case DIR_RIGHT:    return "Derecha";
    case DIR_STOP:
    default:           return "Parado";
  }
}

/**
 * @brief Envía el estado actual (JSON) a TODOS los clientes WS.
 * @note ¡Optimizado! Serializa a un buffer local (stack) en lugar
 * de crear un objeto 'String' (heap), evitando fragmentación.
 */
void notifyClientsStatus() {
  jsonDocOut.clear();
  jsonDocOut["type"] = "status";
  jsonDocOut["running"] = runningState;
  jsonDocOut["speed"] = currentSpeedPercent;
  jsonDocOut["dir"] = stateToString(currentDirection);

  // Serializar a un buffer de stack, no a un String
  char buffer[128];
  size_t len = serializeJson(jsonDocOut, buffer);
  
  // Enviar el buffer con su longitud
  ws.textAll(buffer, len);
}

/**
 * @brief Envía el estado actual (JSON) a UN cliente WS específico.
 * @note También optimizado para usar un buffer de stack.
 * @param client El cliente al que se le enviará el estado.
 */
void sendStatusToClient(AsyncWebSocketClient * client) {
  jsonDocOut.clear();
  jsonDocOut["type"] = "status";
  jsonDocOut["running"] = runningState;
  jsonDocOut["speed"] = currentSpeedPercent;
  jsonDocOut["dir"] = stateToString(currentDirection);
  
  char buffer[128];
  size_t len = serializeJson(jsonDocOut, buffer);
  
  client->text(buffer, len);
}


// --- 8. Control de Motores ---

/**
 * @brief Detiene ambos motores (frenado) y actualiza el estado.
 */
void stopMotors() {
  // Poner ambos pines de un motor en LOW provoca frenado (brake)
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(IN3_PIN, LOW);
  digitalWrite(IN4_PIN, LOW);
  
  // Apagar PWM
  ledcWrite(LEDC_CHANNEL_A, 0);
  ledcWrite(LEDC_CHANNEL_B, 0);
  
  // Actualizar estado global
  runningState = false;
  currentDirection = DIR_STOP;
}

/**
 * @brief Mueve el robot hacia adelante y actualiza el estado.
 */
void moveForward() {
  digitalWrite(IN1_PIN, HIGH);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(IN3_PIN, HIGH);
  digitalWrite(IN4_PIN, LOW);
  
  runningState = true;
  currentDirection = DIR_FORWARD;
  applyDuty(); // Aplicar velocidad
}

/**
 * @brief Mueve el robot hacia atrás y actualiza el estado.
 */
void moveBackward() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, HIGH);
  digitalWrite(IN3_PIN, LOW);
  digitalWrite(IN4_PIN, HIGH);
  
  runningState = true;
  currentDirection = DIR_BACKWARD;
  applyDuty();
}

/**
 * @brief Gira el robot a la izquierda (en el sitio) y actualiza el estado.
 */
void turnLeft() {
  // Motor A (Derecha) hacia atrás, Motor B (Izq) hacia adelante
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, HIGH);
  digitalWrite(IN3_PIN, HIGH);
  digitalWrite(IN4_PIN, LOW);
  
  runningState = true;
  currentDirection = DIR_LEFT;
  applyDuty();
}

/**
 * @brief Gira el robot a la derecha (en el sitio) y actualiza el estado.
 */
void turnRight() {
  // Motor A (Derecha) hacia adelante, Motor B (Izq) hacia atrás
  digitalWrite(IN1_PIN, HIGH);
  digitalWrite(IN2_PIN, LOW);
  digitalWrite(IN3_PIN, LOW);
  digitalWrite(IN4_PIN, HIGH);
  
  runningState = true;
  currentDirection = DIR_RIGHT;
  applyDuty();
}

// --- 9. Interfaz Web (HTML/CSS/JS) ---
// Almacenada en PROGMEM (Flash) para no consumir RAM.
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1, user-scalable=no"/>
  <title>RobotESP32 - PRO Control</title>
  <style>
    :root{
      --bg:#0f1113; --card:#121416; --accent:#7c4dff; --accent-2:#03dac6; --danger:#cf6679; --muted:#bdbdbd;
    }
    body{ margin:0; font-family:Inter, Roboto, Arial, sans-serif; background:linear-gradient(180deg,var(--bg),#070708); color:#e8eef2; display:flex; align-items:center; justify-content:center; min-height:100vh; }
    .panel{ width:420px; max-width:96%; background:var(--card); border-radius:14px; padding:18px; box-shadow:0 8px 30px rgba(0,0,0,0.6); }
    h1{ margin:0 0 8px 0; font-size:20px; color:var(--accent); text-align:center; }
    .grid{ display:grid; grid-template-columns:1fr 1fr 1fr; gap:10px; margin:18px 0; }
    .btn{ background:#1f1f1f; border:0; padding:18px; font-size:20px; border-radius:10px; color:#e9f6f2; cursor:pointer; user-select:none; -webkit-tap-highlight-color:transparent; transition: transform 0.1s; }
    .btn:active{ transform:scale(0.96); box-shadow:inset 0 0 18px rgba(255,255,255,0.02); }
    .btn-stop{ background:linear-gradient(90deg,#b71c1c,#e53935); color:white; font-weight:700; }
    .center{ grid-column:2 / 3; }
    .sliderWrap{ text-align:center; margin:8px 0 6px; }
    .slider{ width:90%; margin:0 auto; display:block; height:14px; background:#0f0f0f; border-radius:10px; -webkit-appearance:none; }
    input[type=range]::-webkit-slider-thumb{ -webkit-appearance:none; width:26px; height:26px; background:var(--accent); border-radius:50%; box-shadow:0 2px 8px rgba(0,0,0,0.6); cursor:pointer; }
    .status{ display:flex; justify-content:space-between; align-items:center; margin-top:12px; font-size:14px; color:var(--muted); }
    .pill{ padding:6px 10px; border-radius:10px; font-weight:600; }
    .detenido{ background:rgba(207,102,121,0.12); color:var(--danger); }
    .en-marcha{ background:rgba(3,218,198,0.08); color:var(--accent-2); }
    .row{ display:flex; gap:8px; justify-content:center; margin-top:8px; }
    .small{ font-size:13px; color:var(--muted); }
  </style>
</head>
<body>
  <div class="panel">
    <h1>RobotESP32 — PRO Control</h1>
    <div style="text-align:center;" class="small">Abrí <strong>http://192.168.4.1</strong></div>
    <div class="grid" style="margin-top:16px;">
      <div></div>
      <button class="btn center" id="btn-forward">▲</button>
      <div></div>
      <button class="btn" id="btn-left">◀</button>
      <button class="btn btn-stop" id="btn-stop">■</button>
      <button class="btn" id="btn-right">▶</button>
      <div></div>
      <button class="btn center" id="btn-backward">▼</button>
      <div></div>
    </div>
    <div class="sliderWrap">
      <label for="speed">Velocidad: <strong id="speedLabel">60%</strong></label>
      <input id="speed" class="slider" type="range" min="0" max="100" value="60">
    </div>
    <div class="status">
      <div id="statePill" class="pill detenido">Desconectado</div>
      <div class="small" id="connInfo">WS: —</div>
    </div>
    <div class="row small" style="margin-top:10px;">
      <div>Pro Controls — WebSocket JSON</div>
    </div>
  </div>
<script>
  const WS_PATH = `ws://${location.hostname}/ws`;
  let socket = null;
  const speedEl = document.getElementById('speed');
  const speedLabel = document.getElementById('speedLabel');
  const statePill = document.getElementById('statePill');
  const connInfo = document.getElementById('connInfo');

  function setStateRunning(running, dir, speed){
    if(running){
      statePill.textContent = `${dir} • ${speed}%`;
      statePill.className = 'pill en-marcha';
    } else {
      statePill.textContent = 'Detenido';
      statePill.className = 'pill detenido';
    }
  }

  function initWS(){
    connInfo.textContent = 'WS: conectando...';
    socket = new WebSocket(WS_PATH);
    socket.onopen = function(){ connInfo.textContent = 'WS: conectado'; socket.send(JSON.stringify({type:'get_status'})); };
    socket.onclose = function(){ connInfo.textContent = 'WS: desconectado'; statePill.textContent = 'Desconectado'; setStateRunning(false); setTimeout(initWS, 1500); };
    socket.onerror = function(e){ console.log('WS err', e); socket.close(); };
    socket.onmessage = function(ev){
      try {
        const msg = JSON.parse(ev.data);
        if(msg.type === 'status'){
          setStateRunning(msg.running, msg.dir, msg.speed);
          if (document.activeElement !== speedEl) {
             speedEl.value = msg.speed;
          }
          speedLabel.textContent = msg.speed + '%';
        } else if(msg.type === 'info'){
          connInfo.textContent = 'WS: ' + msg.text;
        }
      } catch(e){
        console.log('WS raw:', ev.data);
      }
    };
  }

  function sendJSON(obj){
    if(socket && socket.readyState === WebSocket.OPEN){
      socket.send(JSON.stringify(obj));
    } else {
      console.warn("WS no conectado, usando fallback HTTP");
      if(obj.type === 'cmd' && obj.cmd) fetch(`/cmd?c=${encodeURIComponent(obj.cmd)}`);
      if(obj.type === 'speed' && typeof obj.value !== 'undefined') fetch(`/speed?v=${encodeURIComponent(obj.value)}`);
    }
  }

  function addPressRelease(id, cmd){
    const el = document.getElementById(id);
    if (!el) return;
    const sendCmd = (e) => { e.preventDefault(); sendJSON({type:'cmd', cmd:cmd}); };
    const sendStop = () => sendJSON({type:'cmd', cmd:'stop'});
    
    el.addEventListener('mousedown', sendCmd);
    el.addEventListener('mouseup', sendStop);
    el.addEventListener('mouseleave', sendStop);
    el.addEventListener('touchstart', sendCmd, {passive:false});
    el.addEventListener('touchend', sendStop);
  }

  addPressRelease('btn-forward','forward');
  addPressRelease('btn-backward','backward');
  addPressRelease('btn-left','left');
  addPressRelease('btn-right','right');
  
  document.getElementById('btn-stop').addEventListener('click', ()=> sendJSON({type:'cmd', cmd:'stop'}) );

  let sendTimer = null;
  speedEl.addEventListener('input', ()=> {
    const v = parseInt(speedEl.value);
    speedLabel.textContent = v + '%';
    if(sendTimer) clearTimeout(sendTimer);
    sendTimer = setTimeout(()=> {
      sendJSON({type:'speed', value: v});
      sendTimer = null;
    }, 60);
  });

  window.addEventListener('load', initWS);
</script>
</body>
</html>
)rawliteral";


// --- 10. Rutas HTTP (Servir UI y Fallbacks) ---

/**
 * @brief Maneja la solicitud a la raíz ("/").
 * Sirve la página web principal desde PROGMEM.
 */
void onIndexRequest(AsyncWebServerRequest *request) {
  AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_html);
  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  request->send(response);
}

/**
 * @brief Maneja la ruta de 'fallback' para comandos ("/cmd").
 * Usado por el JS si la conexión WebSocket falla.
 */
void onCmdRequest(AsyncWebServerRequest *request) {
  if (!request->hasParam("c")) {
    request->send(400, "text/plain", "Missing command");
    return;
  }
  // Se usa 'String' aquí, pero está bien. Es una ruta de fallback
  // poco frecuente, no una ruta de alto rendimiento.
  String c = request->getParam("c")->value();
  c.toLowerCase();
  
  if (c == "forward") { moveForward(); }
  else if (c == "backward") { moveBackward(); }
  else if (c == "left") { turnLeft(); }
  else if (c == "right") { turnRight(); }
  else if (c == "stop") { stopMotors(); }
  else {
    request->send(400, "text/plain", "Comando no reconocido");
    return;
  }
  
  notifyClientsStatus(); // Sincronizar estado con todos
  request->send(200, "text/plain", "OK");
}

/**
 * @brief Maneja la ruta de 'fallback' para velocidad ("/speed").
 */
void onSpeedRequest(AsyncWebServerRequest *request) {
  if (!request->hasParam("v")) {
    request->send(400, "text/plain", "Missing value");
    return;
  }
  
  int v = request->getParam("v")->value().toInt();
  if (v < 0) v = 0;
  if (v > 100) v = 100;
  
  currentSpeedPercent = v;
  currentDuty = percentToDuty(currentSpeedPercent);
  if (runningState) applyDuty(); // Aplicar solo si está en movimiento
  
  notifyClientsStatus();
  request->send(200, "text/plain", String(currentSpeedPercent));
}

/**
 * @brief Maneja la ruta de estado ("/status") para depuración.
 */
void onStatusRequest(AsyncWebServerRequest *request) {
  jsonDocOut.clear();
  jsonDocOut["running"] = runningState;
  jsonDocOut["speed"] = currentSpeedPercent;
  jsonDocOut["dir"] = stateToString(currentDirection);
  
  // Esta ruta es solo para debug, se permite un String temporal.
  String json;
  serializeJson(jsonDocOut, json);
  request->send(200, "application/json", json);
}

// --- 11. Manejador de Eventos WebSocket ---

/**
 * @brief Callback principal para todos los eventos del WebSocket.
 * Aquí es donde se procesan los comandos JSON recibidos.
 */
void onWsEvent(AsyncWebSocket * serverPtr, AsyncWebSocketClient * client, AwsEventType type,
               void * arg, uint8_t *data, size_t len) {
  
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WS: Cliente %u conectado\n", client->id());
      // El JS enviará "get_status" al conectar
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("WS: Cliente %u desconectado\n", client->id());
      break;
      
    case WS_EVT_DATA: {
      AwsFrameInfo * info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        
        data[len] = 0; // Añadir terminador NUL para tratarlo como C-String
        Serial.printf("WS msg: %s\n", (char*)data);

        // Parsear con ArduinoJson
        jsonDocIn.clear();
        DeserializationError error = deserializeJson(jsonDocIn, (char*)data);
        
        if (error) {
          Serial.print(F("deserializeJson() falló: "));
          Serial.println(error.c_str());
          return; // Ignorar JSON mal formado
        }

        // Procesar JSON válido
        const char* msgType = jsonDocIn["type"];
        if (!msgType) return; // JSON válido, pero sin "type"

        // --- 1. Cliente pide estado ---
        if (strcmp(msgType, "get_status") == 0) {
          sendStatusToClient(client); // Enviar estado solo a este cliente
        
        // --- 2. Comando de movimiento ---
        } else if (strcmp(msgType, "cmd") == 0) {
          const char* cmd = jsonDocIn["cmd"];
          if (!cmd) return;

          if (strcmp(cmd, "forward") == 0) { moveForward(); }
          else if (strcmp(cmd, "backward") == 0) { moveBackward(); }
          else if (strcmp(cmd, "left") == 0) { turnLeft(); }
          else if (strcmp(cmd, "right") == 0) { turnRight(); }
          else if (strcmp(cmd, "stop") == 0) { stopMotors(); }
          
          notifyClientsStatus(); // Notificar a todos del cambio

        // --- 3. Comando de velocidad ---
        } else if (strcmp(msgType, "speed") == 0) {
          if (!jsonDocIn["value"].is<int>()) return;
          
          int v = jsonDocIn["value"];
          if (v < 0) v = 0; if (v > 100) v = 100;
          
          currentSpeedPercent = v;
          currentDuty = percentToDuty(currentSpeedPercent);
          if (runningState) applyDuty(); // Aplicar solo si se mueve
          
          notifyClientsStatus(); // Notificar a todos
        }
      }
      break;
    }
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      // Ignorar estos eventos
      break;
  }
}

// --- 12. Setup y Loop ---

void setup() {
  Serial.begin(115200);
  delay(200);

  // --- Configurar Pines de Motores ---
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);
  pinMode(IN3_PIN, OUTPUT);
  pinMode(IN4_PIN, OUTPUT);
  stopMotors(); // Asegurar que están detenidos al inicio

  // --- Configurar PWM (ledc) ---
  // 1. Configurar canal (canal, frecuencia, resolución)
  ledcSetup(LEDC_CHANNEL_A, LEDC_FREQ, LEDC_RESOLUTION);
  ledcSetup(LEDC_CHANNEL_B, LEDC_FREQ, LEDC_RESOLUTION);
  
  // 2. Asignar pin al canal
  ledcAttachPin(ENA_PIN, LEDC_CHANNEL_A);
  ledcAttachPin(ENB_PIN, LEDC_CHANNEL_B);
  
  // 3. Escribir valor inicial (apagado)
  ledcWrite(LEDC_CHANNEL_A, 0);
  ledcWrite(LEDC_CHANNEL_B, 0);
  Serial.println("Motores y PWM (10-bit) configurados.");

  // --- Iniciar WiFi Access Point ---
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress IP = WiFi.softAPIP();
  Serial.println("\n=== RobotPRO AP (Optimizado) Iniciado ===");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("IP:   "); Serial.println(IP);

  // --- Configurar Servidor ---
  // 1. Adjuntar el manejador de WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // 2. Adjuntar rutas HTTP
  server.on("/", HTTP_GET, onIndexRequest);      // Sirve la UI
  server.on("/cmd", HTTP_GET, onCmdRequest);     // Fallback de comandos
  server.on("/speed", HTTP_GET, onSpeedRequest); // Fallback de velocidad
  server.on("/status", HTTP_GET, onStatusRequest); // Debug

  // 3. Iniciar servidor
  server.begin();
  Serial.println("Servidor Async (HTTP + WS) iniciado en puerto 80.");
}

void loop() {
  // Nada aquí. Todo es asíncrono y se maneja por eventos (callbacks).
  // La librería AsyncWebServer/TCP maneja todo en sus propias tareas.
  
  // Pequeña pausa para permitir que otros procesos (como el WDT) se ejecuten.
  delay(10); 
}
