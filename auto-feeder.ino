// =====================================================================
// PetFeeder IoT - alimentador remoto de ração (ESP32)
//
// Comando (dashboard -> Adafruit IO -> ESP32 -> servo) e telemetria
// (sensor IR -> ESP32 -> Adafruit IO -> dashboard), conforme a Etapa 2.
//
// Bibliotecas: ESP32Servo, PubSubClient (WiFi, Preferences e time são do core).
// Parâmetros em config.h; credenciais em secrets.h (ver secrets.example.h).
//
// Comandos pela Serial (115200 baud), para testes de bancada e calibração:
//   d <g>    dispensa <g> gramas (mesmas regras do comando remoto)
//   abrir    abre a comporta e zera a contagem (calibração)
//   fechar   fecha a comporta e mostra a contagem
//   s        mostra o estado atual
//   zerar    zera o total do dia
// =====================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <time.h>

#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "secrets.h nao encontrado: usando secrets.example.h (credenciais de exemplo)"
#include "secrets.example.h"
#endif

// ---------------------------------------------------------------------
// Estados do firmware (Etapa 2, seção 10)
// ---------------------------------------------------------------------
enum Estado { INICIANDO, OCIOSO, DISPENSANDO, CONCLUIDO, ERRO };
const char *NOMES_ESTADO[] = {"INICIANDO", "OCIOSO", "DISPENSANDO", "CONCLUIDO", "ERRO"};

Estado estado = INICIANDO;
unsigned long inicioEstadoMs = 0;

// ---------------------------------------------------------------------
// Sensor IR: contagem por interrupção com debounce
// ---------------------------------------------------------------------
volatile uint32_t contagemSensor = 0;
volatile unsigned long ultimoPulsoUs = 0;
portMUX_TYPE muxSensor = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR isrSensor() {
  unsigned long agora = micros();
  if (agora - ultimoPulsoUs >= DEBOUNCE_SENSOR_US) {
    portENTER_CRITICAL_ISR(&muxSensor);
    contagemSensor = contagemSensor + 1;
    portEXIT_CRITICAL_ISR(&muxSensor);
  }
  ultimoPulsoUs = agora;
}

uint32_t lerContagem() {
  portENTER_CRITICAL(&muxSensor);
  uint32_t n = contagemSensor;
  portEXIT_CRITICAL(&muxSensor);
  return n;
}

void zerarContagem() {
  portENTER_CRITICAL(&muxSensor);
  contagemSensor = 0;
  portEXIT_CRITICAL(&muxSensor);
}

uint32_t contagemParaGramas(uint32_t n) {
  return (uint32_t)lroundf(n * GRAMAS_POR_UNIDADE);
}

// ---------------------------------------------------------------------
// Servo / comporta
// ---------------------------------------------------------------------
Servo servoComporta;

void fecharComporta() { servoComporta.write(ANGULO_FECHADA); }
void abrirComporta()  { servoComporta.write(ANGULO_ABERTA); }

// ---------------------------------------------------------------------
// Dispensa e total diário (persistido em NVS para sobreviver a reinícios)
// ---------------------------------------------------------------------
Preferences prefs;

int metaGramas = QTD_PADRAO_G;     // último valor recebido do slider
int metaAtual = 0;                 // meta da dispensa em andamento
uint32_t totalDiaG = 0;
uint32_t diaDoTotal = 0;           // AAAAMMDD; 0 = data ainda desconhecida
bool comporta_manual = false;      // abertura manual pela Serial (calibração)

void salvarTotal() {
  prefs.putUInt("total", totalDiaG);
  prefs.putUInt("dia", diaDoTotal);
}

// Retorna a data local como AAAAMMDD, ou 0 se o NTP ainda não sincronizou.
uint32_t diaAtual() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year + 1900 < 2024) return 0;
  return (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100UL + t.tm_mday;
}

// ---------------------------------------------------------------------
// MQTT / Adafruit IO
// ---------------------------------------------------------------------
#if MQTT_USAR_TLS
WiFiClientSecure clienteRede;
const uint16_t MQTT_PORTA = 8883;
#else
WiFiClient clienteRede;
const uint16_t MQTT_PORTA = 1883;
#endif
PubSubClient mqtt(clienteRede);

String topico(const char *feed) {
  return String(AIO_USUARIO) + "/feeds/" + feed;
}

// Publicações que ficam pendentes enquanto a rede está fora (R8).
// Guarda apenas o último valor de cada feed.
struct Pendente {
  const char *feed;
  String valor;
  bool ativo;
};
Pendente pendentes[] = {
  {FEED_STATUS, "", false},
  {FEED_CONTAGEM, "", false},
  {FEED_GRAMAS, "", false},
  {FEED_TOTAL_DIA, "", false},
  {FEED_COMANDO, "", false},
};
const size_t N_PENDENTES = sizeof(pendentes) / sizeof(pendentes[0]);

void publicar(const char *feed, const String &valor) {
  Serial.printf("[pub] %s = %s\n", feed, valor.c_str());
  if (HABILITAR_REDE && mqtt.connected() && mqtt.publish(topico(feed).c_str(), valor.c_str())) {
    return;
  }
  for (size_t i = 0; i < N_PENDENTES; i++) {
    if (strcmp(pendentes[i].feed, feed) == 0) {
      pendentes[i].valor = valor;
      pendentes[i].ativo = HABILITAR_REDE;
      return;
    }
  }
}

void enviarPendentes() {
  for (size_t i = 0; i < N_PENDENTES; i++) {
    if (!pendentes[i].ativo) continue;
    if (!mqtt.publish(topico(pendentes[i].feed).c_str(), pendentes[i].valor.c_str())) return;
    Serial.printf("[pub] (pendente) %s = %s\n", pendentes[i].feed, pendentes[i].valor.c_str());
    pendentes[i].ativo = false;
  }
}

void publicarStatus(const String &corpo) {
  publicar(FEED_STATUS, corpo + ";dev=" DEVICE_ID);
}

// ---------------------------------------------------------------------
// Máquina de estados
// ---------------------------------------------------------------------
void mudarEstado(Estado novo) {
  Serial.printf("[estado] %s -> %s\n", NOMES_ESTADO[estado], NOMES_ESTADO[novo]);
  estado = novo;
  inicioEstadoMs = millis();
}

// Regras R1 a R4: valida e inicia uma dispensa.
void pedirDispensa(int gramas) {
  if (estado != OCIOSO || comporta_manual) {                       // R2
    publicarStatus("recusado;motivo=ocupado");
    return;
  }
  if (gramas < QTD_MIN_G || gramas > QTD_MAX_G) {                  // R3
    publicarStatus("recusado;motivo=quantidade;meta=" + String(gramas));
    return;
  }
  if (totalDiaG + (uint32_t)gramas > LIMITE_DIARIO_G) {            // R4
    publicarStatus("recusado;motivo=limite_diario;meta=" + String(gramas) +
                   ";total=" + String(totalDiaG));
    return;
  }
  metaAtual = gramas;                                              // R1
  zerarContagem();
  abrirComporta();
  mudarEstado(DISPENSANDO);
  publicarStatus("dispensando;meta=" + String(metaAtual));
}

// Fecha a comporta e publica o resultado (R5, R6, R7).
void finalizarDispensa(const char *resultado) {
  fecharComporta();
  uint32_t n = lerContagem();
  uint32_t g = contagemParaGramas(n);

  totalDiaG += g;
  salvarTotal();

  String corpo = String(resultado);
  if (strcmp(resultado, "erro") == 0) corpo += ";motivo=sem_fluxo";
  corpo += ";meta=" + String(metaAtual) + ";g=" + String(g) + ";n=" + String(n);

  publicarStatus(corpo);
  publicar(FEED_CONTAGEM, String(n));
  publicar(FEED_GRAMAS, String(g));
  publicar(FEED_TOTAL_DIA, String(totalDiaG));
  publicar(FEED_COMANDO, "0");  // devolve o botão para "0"

  mudarEstado(strcmp(resultado, "concluido") == 0 ? CONCLUIDO : ERRO);
}

void atualizarEstado() {
  unsigned long decorrido = millis() - inicioEstadoMs;

  switch (estado) {
    case DISPENSANDO: {
      uint32_t n = lerContagem();
      if (contagemParaGramas(n) >= (uint32_t)metaAtual) {
        finalizarDispensa("concluido");                            // R5
      } else if (decorrido > T_MAX_DISPENSA_MS) {
        finalizarDispensa(n > 0 ? "timeout" : "erro");             // R6 / R7
      }
      break;
    }
    case CONCLUIDO:
    case ERRO:
      if (decorrido >= COOLDOWN_MS) {
        mudarEstado(OCIOSO);
        publicarStatus("ocioso;total=" + String(totalDiaG));
      }
      break;
    case OCIOSO: {
      static unsigned long ultimoOciosoMs = 0;
      if (millis() - ultimoOciosoMs >= STATUS_OCIOSO_MS) {
        ultimoOciosoMs = millis();
        publicarStatus("ocioso;total=" + String(totalDiaG));
      }
      break;
    }
    default:
      break;
  }
}

// Zera o total à meia-noite (hora local via NTP).
void verificarVirada() {
  static unsigned long ultimaVerificacaoMs = 0;
  if (millis() - ultimaVerificacaoMs < 1000) return;
  ultimaVerificacaoMs = millis();

  uint32_t hoje = diaAtual();
  if (hoje == 0 || hoje == diaDoTotal) return;

  if (diaDoTotal != 0) {
    Serial.printf("[dia] novo dia %lu, total zerado\n", (unsigned long)hoje);
    totalDiaG = 0;
    publicar(FEED_TOTAL_DIA, "0");
  }
  // diaDoTotal == 0: total salvo antes de existir hora. Por segurança,
  // considera que é do dia atual em vez de zerar.
  diaDoTotal = hoje;
  salvarTotal();
}

// ---------------------------------------------------------------------
// Recebimento de mensagens MQTT
// ---------------------------------------------------------------------
void aoReceberMqtt(char *topicoRecebido, byte *payload, unsigned int tamanho) {
  String valor;
  valor.reserve(tamanho);
  for (unsigned int i = 0; i < tamanho; i++) valor += (char)payload[i];
  valor.trim();
  Serial.printf("[sub] %s = %s\n", topicoRecebido, valor.c_str());

  String t(topicoRecebido);
  if (t == topico(FEED_QUANTIDADE)) {
    metaGramas = (int)lroundf(valor.toFloat());
  } else if (t == topico(FEED_COMANDO)) {
    if (valor == "1") pedirDispensa(metaGramas);  // "0" é o próprio reset do ESP32
  }
}

// ---------------------------------------------------------------------
// Conexão Wi-Fi / MQTT com reconexão não bloqueante e backoff (R8)
// ---------------------------------------------------------------------
unsigned long intervaloWifiMs = RECONEXAO_MIN_MS, ultimaTentativaWifiMs = 0;
unsigned long intervaloMqttMs = RECONEXAO_MIN_MS, ultimaTentativaMqttMs = 0;
bool ntpConfigurado = false;

void iniciarRede() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);
  ultimaTentativaWifiMs = millis();

#if MQTT_USAR_TLS
#ifdef AIO_ROOT_CA
  clienteRede.setCACert(AIO_ROOT_CA);
#else
  clienteRede.setInsecure();  // criptografado, sem validar o certificado
#endif
#endif
  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(aoReceberMqtt);
}

bool conectarMqtt() {
  Serial.printf("[mqtt] conectando a %s:%u...\n", MQTT_BROKER, MQTT_PORTA);
  if (!mqtt.connect(MQTT_CLIENT_ID, AIO_USUARIO, AIO_CHAVE)) {
    Serial.printf("[mqtt] falha, rc=%d\n", mqtt.state());
    return false;
  }
  Serial.println("[mqtt] conectado");
  mqtt.subscribe(topico(FEED_COMANDO).c_str());
  mqtt.subscribe(topico(FEED_QUANTIDADE).c_str());
  mqtt.subscribe((String(AIO_USUARIO) + "/errors").c_str());
  mqtt.subscribe((String(AIO_USUARIO) + "/throttle").c_str());
  // Pede o último valor do slider (o Adafruit IO não usa mensagens retidas).
  mqtt.publish((topico(FEED_QUANTIDADE) + "/get").c_str(), "");
  enviarPendentes();
  return true;
}

void manterRede() {
  if (!HABILITAR_REDE) return;
  unsigned long agora = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (agora - ultimaTentativaWifiMs >= intervaloWifiMs) {
      Serial.printf("[wifi] reconectando (proxima em %lus)\n", intervaloWifiMs / 1000);
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_SENHA);
      ultimaTentativaWifiMs = agora;
      intervaloWifiMs = min(intervaloWifiMs * 2, RECONEXAO_MAX_MS);
    }
    return;
  }
  if (intervaloWifiMs != RECONEXAO_MIN_MS) {
    Serial.printf("[wifi] conectado, IP %s\n", WiFi.localIP().toString().c_str());
    intervaloWifiMs = RECONEXAO_MIN_MS;
  }

  if (!ntpConfigurado) {
    configTzTime(FUSO_HORARIO, NTP_SERVIDOR_1, NTP_SERVIDOR_2);
    ntpConfigurado = true;
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  // A conexão MQTT/TLS bloqueia por alguns segundos: não tentar durante a
  // dispensa, para o controle da comporta não atrasar (dispensa segue local).
  if (estado == DISPENSANDO) return;
  if (agora - ultimaTentativaMqttMs < intervaloMqttMs) return;
  ultimaTentativaMqttMs = agora;
  if (conectarMqtt()) {
    intervaloMqttMs = RECONEXAO_MIN_MS;
  } else {
    intervaloMqttMs = min(intervaloMqttMs * 2, RECONEXAO_MAX_MS);
  }
}

// ---------------------------------------------------------------------
// LED de status
//   aceso: dispensando | piscando rápido: erro/cooldown após erro
//   piscando lento: sem conexão | apagado: ocioso e conectado
// ---------------------------------------------------------------------
void atualizarLed() {
  bool ligado;
  unsigned long t = millis();
  if (estado == DISPENSANDO || comporta_manual) ligado = true;
  else if (estado == ERRO) ligado = (t / 150) % 2;
  else if (HABILITAR_REDE && !mqtt.connected()) ligado = (t / 1000) % 2;
  else ligado = false;
  digitalWrite(PINO_LED, ligado);
}

// ---------------------------------------------------------------------
// Comandos pela Serial
// ---------------------------------------------------------------------
void mostrarEstado() {
  Serial.printf("estado=%s meta_slider=%d g total_dia=%lu g dia=%lu contagem=%lu (%lu g)\n",
                NOMES_ESTADO[estado], metaGramas, (unsigned long)totalDiaG,
                (unsigned long)diaDoTotal, (unsigned long)lerContagem(),
                (unsigned long)contagemParaGramas(lerContagem()));
  Serial.printf("wifi=%s mqtt=%s\n",
                WiFi.status() == WL_CONNECTED ? "ok" : "off",
                mqtt.connected() ? "ok" : "off");
}

void tratarSerial() {
  static String linha;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (linha.length() < 40) linha += c;
      continue;
    }
    linha.trim();
    if (linha.length() == 0) continue;

    if (linha.startsWith("d ")) {
      pedirDispensa(linha.substring(2).toInt());
    } else if (linha == "abrir") {
      if (estado == OCIOSO) {
        zerarContagem();
        abrirComporta();
        comporta_manual = true;
        Serial.println("comporta aberta (manual), contagem zerada");
      } else {
        Serial.println("ocupado");
      }
    } else if (linha == "fechar") {
      fecharComporta();
      comporta_manual = false;
      uint32_t n = lerContagem();
      Serial.printf("comporta fechada: n=%lu, %lu g estimados (%.3f g/unid)\n",
                    (unsigned long)n, (unsigned long)contagemParaGramas(n), GRAMAS_POR_UNIDADE);
    } else if (linha == "s") {
      mostrarEstado();
    } else if (linha == "zerar") {
      totalDiaG = 0;
      salvarTotal();
      publicar(FEED_TOTAL_DIA, "0");
    } else {
      Serial.println("comandos: d <g> | abrir | fechar | s | zerar");
    }
    linha = "";
  }
}

// ---------------------------------------------------------------------
void setup() {
  // R9: estado seguro antes de qualquer outra ação.
  servoComporta.setPeriodHertz(50);
  servoComporta.attach(PINO_SERVO, SERVO_PULSO_MIN, SERVO_PULSO_MAX);
  fecharComporta();

  pinMode(PINO_LED, OUTPUT);
  digitalWrite(PINO_LED, LOW);

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== PetFeeder IoT (" DEVICE_ID ") ===");

  pinMode(PINO_SENSOR, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PINO_SENSOR), isrSensor, FALLING);

  prefs.begin("petfeeder", false);
  totalDiaG = prefs.getUInt("total", 0);
  diaDoTotal = prefs.getUInt("dia", 0);
  Serial.printf("total do dia salvo: %lu g (dia %lu)\n",
                (unsigned long)totalDiaG, (unsigned long)diaDoTotal);

  if (HABILITAR_REDE) iniciarRede();
  else Serial.println("modo local: rede desabilitada (config.h)");

  mudarEstado(OCIOSO);
}

void loop() {
  manterRede();
  tratarSerial();
  verificarVirada();
  atualizarEstado();
  atualizarLed();
}
