// =====================================================================
// PetFeeder IoT - alimentador remoto de ração (ESP32)
//
// Comando (dashboard -> Adafruit IO -> ESP32 -> servo) e telemetria
// (sensor LDR -> ESP32 -> Adafruit IO -> dashboard), conforme a Etapa 2.
// O sensor IR FC-51 previsto foi trocado por uma barreira de luz
// (LED branco + LDR); ver a seção do sensor abaixo.
//
// Bibliotecas: ESP32Servo, PubSubClient (WiFi, Preferences e time são do core).
// Parâmetros em config.h; credenciais em secrets.h (ver secrets.example.h).
//
// Comandos pela Serial (115200 baud), para testes de bancada e calibração:
//   d        dispensa a quantidade configurada
//   d <g>    dispensa <g> gramas (mesmas regras do comando remoto)
//   q <g>    define e salva a quantidade por dispensa (publica em pf-quantidade)
//   abrir    abre a comporta e zera a contagem (calibração)
//   fechar   fecha a comporta e mostra a contagem
//   s        mostra o estado atual
//   ldr      liga/desliga a leitura contínua do LDR (alinhamento e ajuste)
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
// Sensor: barreira de luz com LED branco + LDR (substitui o FC-51)
//
// O LED ilumina o LDR através do cano. A ração que cai faz sombra e a
// leitura de luz cai abaixo da linha de base. O LDR é lento demais para
// separar grãos que caem juntos, então em vez de contar grãos o firmware
// soma a sombra ao longo do tempo (área abaixo da linha de base). Essa
// soma é proporcional à quantidade que passou e vale também para fluxo
// contínuo. Cada LDR_ESCALA de sombra acumulada vira 1 "unidade", e
// GRAMAS_POR_UNIDADE converte unidades em gramas (calibração).
//
// A leitura roda numa tarefa própria, a cada LDR_PERIODO_MS, para não
// depender do tempo do loop (Wi-Fi/MQTT).
// ---------------------------------------------------------------------
volatile int luzAtual = 0;          // última leitura (0 = escuro, 4095 = muita luz)
volatile float luzBase = 0;         // linha de base (média móvel com o cano vazio)
volatile bool medindo = false;      // true enquanto a comporta está aberta
volatile int baseFixa = 0;          // linha de base congelada no início da medição
volatile uint64_t sombraAcumulada = 0;
portMUX_TYPE muxSensor = portMUX_INITIALIZER_UNLOCKED;

int lerLuz() {
  int v = analogRead(PINO_LDR);
  return LDR_MAIS_LUZ_MAIOR ? v : 4095 - v;
}

void tarefaSensor(void *) {
  luzBase = lerLuz();
  TickType_t proximo = xTaskGetTickCount();
  for (;;) {
    int luz = lerLuz();
    luzAtual = luz;
    if (medindo) {
      int sombra = baseFixa - luz - LDR_ZONA_MORTA;
      if (sombra > 0) {
        portENTER_CRITICAL(&muxSensor);
        sombraAcumulada += sombra;
        portEXIT_CRITICAL(&muxSensor);
      }
    } else {
      luzBase += (luz - luzBase) / 64.0f;  // acompanha a luz ambiente devagar
    }
    vTaskDelayUntil(&proximo, pdMS_TO_TICKS(LDR_PERIODO_MS));
  }
}

void iniciarSensor() {
  analogReadResolution(12);
  analogSetPinAttenuation(PINO_LDR, ADC_11db);
  xTaskCreatePinnedToCore(tarefaSensor, "sensor", 2048, nullptr, 2, nullptr, 1);
}

// Sensor com pouca luz na base = LED apagado, LDR solto ou desalinhado.
bool sensorOk() { return luzBase >= LDR_BASE_MIN; }

uint32_t lerContagem() {
  portENTER_CRITICAL(&muxSensor);
  uint64_t s = sombraAcumulada;
  portEXIT_CRITICAL(&muxSensor);
  return (uint32_t)(s / LDR_ESCALA);
}

// Zera a contagem, congela a linha de base atual e começa a medir.
void zerarContagem() {
  portENTER_CRITICAL(&muxSensor);
  sombraAcumulada = 0;
  baseFixa = (int)luzBase;
  medindo = true;
  portEXIT_CRITICAL(&muxSensor);
}

void pararMedicao() { medindo = false; }

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

int metaGramas = QTD_PADRAO_G;     // quantidade configurada (salva em NVS)
bool metaJaSalva = false;          // false = primeira vez: publicar o padrão no feed
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
  {FEED_MENSAGEM, "", false},
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

// pf-status recebe só o estado: ocioso, dispensando, concluido, timeout ou erro.
void publicarStatus(const char *estadoAtual) {
  publicar(FEED_STATUS, estadoAtual);
}

// Códigos publicados em pf-mensagem, no formato "<código>: <mensagem>".
#define ERRO_OCUPADO        "E01"  // R2: dispensa em andamento, cooldown ou comporta manual
#define ERRO_QUANTIDADE     "E02"  // R3: quantidade fora da faixa
#define ERRO_LIMITE_DIARIO  "E03"  // R4: total do dia + meta passa do limite
#define ERRO_SENSOR         "E04"  // sensor LDR sem luz (LED apagado ou LDR solto)
#define ERRO_TIMEOUT        "E05"  // R6: tempo máximo com quantidade parcial
#define ERRO_SEM_FLUXO      "E06"  // R7: tempo máximo sem nenhuma ração detectada

void publicarErro(const char *codigo, const String &mensagem) {
  publicar(FEED_MENSAGEM, String(codigo) + ": " + mensagem);
}

// Altera a quantidade por dispensa e a mantém após reinícios (NVS).
// Valores fora da faixa são ignorados e a quantidade anterior continua valendo.
void definirMeta(int gramas, bool publicarNoFeed) {
  if (gramas < QTD_MIN_G || gramas > QTD_MAX_G) {
    Serial.printf("[meta] %d g fora da faixa (%d a %d), mantendo %d g\n",
                  gramas, QTD_MIN_G, QTD_MAX_G, metaGramas);
    return;
  }
  if (gramas != metaGramas || !metaJaSalva) {
    metaGramas = gramas;
    prefs.putInt("meta", metaGramas);
    metaJaSalva = true;
    Serial.printf("[meta] quantidade salva: %d g\n", metaGramas);
  }
  if (publicarNoFeed) publicar(FEED_QUANTIDADE, String(metaGramas));
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
// Um pedido recusado não muda o estado: só publica o motivo em pf-mensagem.
// CONCLUIDO aceita um novo pedido na hora; ERRO aguarda o cooldown.
void pedirDispensa(int gramas) {
  bool livre = (estado == OCIOSO || estado == CONCLUIDO) && !comporta_manual;
  if (!livre) {                                                    // R2
    publicarErro(ERRO_OCUPADO, "recusado, equipamento ocupado (" +
                 String(NOMES_ESTADO[estado]) + ")");
    return;
  }
  if (gramas < QTD_MIN_G || gramas > QTD_MAX_G) {                  // R3
    publicarErro(ERRO_QUANTIDADE, "recusado, quantidade " + String(gramas) +
                 " g fora da faixa de " + String(QTD_MIN_G) + " a " + String(QTD_MAX_G) + " g");
    return;
  }
  if (totalDiaG + (uint32_t)gramas > LIMITE_DIARIO_G) {            // R4
    publicarErro(ERRO_LIMITE_DIARIO, "recusado, limite diario: " + String(totalDiaG) +
                 " g + " + String(gramas) + " g passa de " + String(LIMITE_DIARIO_G) + " g");
    return;
  }
  if (!sensorOk()) {                                               // sensor sem luz
    publicarErro(ERRO_SENSOR, "recusado, sensor sem luz (leitura " +
                 String((int)luzBase) + ", minimo " + String(LDR_BASE_MIN) + ")");
    return;
  }
  metaAtual = gramas;                                              // R1
  zerarContagem();
  abrirComporta();
  mudarEstado(DISPENSANDO);
  publicarStatus("dispensando");
}

// Fecha a comporta e publica o resultado (R5, R6, R7).
void finalizarDispensa(const char *resultado) {
  fecharComporta();
  pararMedicao();
  uint32_t n = lerContagem();
  uint32_t g = contagemParaGramas(n);

  totalDiaG += g;
  salvarTotal();

  publicarStatus(resultado);
  if (strcmp(resultado, "timeout") == 0) {
    publicarErro(ERRO_TIMEOUT, "tempo maximo atingido, liberados " + String(g) +
                 " de " + String(metaAtual) + " g");
  } else if (strcmp(resultado, "erro") == 0) {
    publicarErro(ERRO_SEM_FLUXO, "sem fluxo de racao (reservatorio vazio ou cano entupido)");
  }
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
        publicarStatus("ocioso");
      }
      break;
    case OCIOSO: {
      static unsigned long ultimoOciosoMs = 0;
      if (millis() - ultimoOciosoMs >= STATUS_OCIOSO_MS) {
        ultimoOciosoMs = millis();
        publicarStatus("ocioso");
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
    definirMeta((int)lroundf(valor.toFloat()), false);
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
  enviarPendentes();
  if (!metaJaSalva) {
    // Primeira execução: grava o padrão no feed para o slider começar nele.
    definirMeta(metaGramas, true);
  } else {
    // Pede o último valor do slider, caso tenha mudado com o ESP32 desligado
    // (o Adafruit IO não usa mensagens retidas).
    mqtt.publish((topico(FEED_QUANTIDADE) + "/get").c_str(), "");
  }
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
  Serial.printf("luz=%d base=%d (minimo %d) sensor=%s\n", luzAtual, (int)luzBase,
                LDR_BASE_MIN, sensorOk() ? "ok" : "SEM LUZ");
  Serial.printf("wifi=%s mqtt=%s\n",
                WiFi.status() == WL_CONNECTED ? "ok" : "off",
                mqtt.connected() ? "ok" : "off");
}

// Publica a leitura do LDR em pf-luz a cada LUZ_INTERVALO_MS. Valores antigos
// não ficam pendentes: sem conexão, a leitura é só mostrada na Serial.
void publicarLuz() {
  static unsigned long ultimoMs = 0;
  if (millis() - ultimoMs < LUZ_INTERVALO_MS) return;
  ultimoMs = millis();
  publicar(FEED_LUZ, String(luzAtual));
}

// Liga/desliga com o comando "ldr": mostra a leitura a cada 200 ms,
// para alinhar o LED com o LDR e escolher LDR_ZONA_MORTA / LDR_BASE_MIN.
bool monitorLdr = false;

void mostrarLdr() {
  static unsigned long ultimoMs = 0;
  if (!monitorLdr || millis() - ultimoMs < 200) return;
  ultimoMs = millis();
  Serial.printf("luz=%4d base=%4d sombra=%4d n=%lu\n", luzAtual, (int)luzBase,
                max(0, (int)luzBase - luzAtual), (unsigned long)lerContagem());
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

    if (linha == "d") {
      pedirDispensa(metaGramas);
    } else if (linha.startsWith("d ")) {
      pedirDispensa(linha.substring(2).toInt());
    } else if (linha.startsWith("q ")) {
      definirMeta(linha.substring(2).toInt(), true);
    } else if (linha == "abrir") {
      if (estado == OCIOSO) {
        if (!sensorOk()) Serial.println("aviso: sensor com pouca luz, contagem nao confiavel");
        zerarContagem();
        abrirComporta();
        comporta_manual = true;
        Serial.println("comporta aberta (manual), contagem zerada");
      } else {
        Serial.println("ocupado");
      }
    } else if (linha == "fechar") {
      fecharComporta();
      pararMedicao();
      comporta_manual = false;
      uint32_t n = lerContagem();
      Serial.printf("comporta fechada: n=%lu, %lu g estimados (%.3f g/unid)\n",
                    (unsigned long)n, (unsigned long)contagemParaGramas(n), GRAMAS_POR_UNIDADE);
    } else if (linha == "s") {
      mostrarEstado();
    } else if (linha == "ldr") {
      monitorLdr = !monitorLdr;
    } else if (linha == "zerar") {
      totalDiaG = 0;
      salvarTotal();
      publicar(FEED_TOTAL_DIA, "0");
    } else {
      Serial.println("comandos: d | d <g> | q <g> | abrir | fechar | s | ldr | zerar");
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

  iniciarSensor();

  prefs.begin("petfeeder", false);
  totalDiaG = prefs.getUInt("total", 0);
  diaDoTotal = prefs.getUInt("dia", 0);
  Serial.printf("total do dia salvo: %lu g (dia %lu)\n",
                (unsigned long)totalDiaG, (unsigned long)diaDoTotal);
  metaJaSalva = prefs.isKey("meta");
  metaGramas = prefs.getInt("meta", QTD_PADRAO_G);
  Serial.printf("quantidade por dispensa: %d g%s\n", metaGramas,
                metaJaSalva ? "" : " (padrao)");

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
  mostrarLdr();
  publicarLuz();
}
