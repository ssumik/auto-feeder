// =====================================================================
// PetFeeder IoT - parâmetros do firmware
// Valores provisórios (Etapa 2, seção 11). Ajustar com os testes da Etapa 3.
// =====================================================================
#pragma once

// ---------------- Pinos (Etapa 2, seção 4) ----------------
#define PINO_SERVO   18   // PWM do servo da comporta
#define PINO_SENSOR  27   // OUT do sensor IR FC-51 (nível baixo = detectou)
#define PINO_LED      2   // LED onboard de status

// ---------------- Servo / comporta ----------------
#define ANGULO_FECHADA   0    // graus
#define ANGULO_ABERTA   90    // graus
#define SERVO_PULSO_MIN 500   // us (SG90/MG90S)
#define SERVO_PULSO_MAX 2400  // us

// ---------------- Dispensa ----------------
// Gramas por unidade contada pelo sensor. VALOR PROVISÓRIO: medir na calibração
// (dispensar várias vezes, pesar a ração e dividir pela contagem).
#define GRAMAS_POR_UNIDADE   0.5f

#define QTD_MIN_G            10       // g
#define QTD_MAX_G            200      // g
#define QTD_PADRAO_G         60       // g, usado até chegar o valor do slider
#define LIMITE_DIARIO_G      400      // g
#define T_MAX_DISPENSA_MS    15000UL  // T_máx
#define COOLDOWN_MS          60000UL  // intervalo mínimo entre dispensas
#define DEBOUNCE_SENSOR_US   2000UL   // filtro de ruído do sensor (2 ms)

// ---------------- Rede ----------------
// false = modo local (sem Wi-Fi/MQTT), útil para testes de bancada via Serial.
#define HABILITAR_REDE       true

// true = porta 8883 com TLS (preferencial); false = porta 1883 (só testes).
#define MQTT_USAR_TLS        true

#define MQTT_BROKER          "io.adafruit.com"
#define MQTT_CLIENT_ID       "petfeeder-esp32-01"
#define DEVICE_ID            "pf01"
#define MQTT_KEEPALIVE_S     60

#define RECONEXAO_MIN_MS     1000UL   // intervalo inicial de reconexão
#define RECONEXAO_MAX_MS     60000UL  // intervalo máximo (backoff exponencial)
#define STATUS_OCIOSO_MS     60000UL  // publica "ocioso" a cada 60 s

// ---------------- Hora (NTP) ----------------
#define FUSO_HORARIO         "<-03>3"  // UTC-3, sem horário de verão
#define NTP_SERVIDOR_1       "pool.ntp.org"
#define NTP_SERVIDOR_2       "time.google.com"

// ---------------- Feeds do Adafruit IO (Etapa 2, seção 8) ----------------
#define FEED_COMANDO     "pf-comando"
#define FEED_QUANTIDADE  "pf-quantidade"
#define FEED_STATUS      "pf-status"
#define FEED_CONTAGEM    "pf-contagem"
#define FEED_GRAMAS      "pf-gramas"
#define FEED_TOTAL_DIA   "pf-total-dia"
