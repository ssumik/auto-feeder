// =====================================================================
// PetFeeder IoT - parâmetros do firmware
// Valores provisórios (Etapa 2, seção 11). Ajustar com os testes da Etapa 3.
// =====================================================================
#pragma once

// ---------------- Pinos (Etapa 2, seção 4) ----------------
#define PINO_SERVO   18   // PWM do servo da comporta
// LDR (divisor com resistor de 10 kΩ). Precisa ser pino do ADC1 (GPIO32 a 39):
// o ADC2 (ex.: GPIO27, o pino previsto para o FC-51) não funciona com o Wi-Fi ligado.
#define PINO_LDR     34
#define PINO_LED      2   // LED onboard de status

// ---------------- Servo / comporta ----------------
#define ANGULO_FECHADA   0    // graus
#define ANGULO_ABERTA   90    // graus
#define SERVO_PULSO_MIN 500   // us (SG90/MG90S)
#define SERVO_PULSO_MAX 2400  // us

// ---------------- Dispensa ----------------
// Gramas por unidade de sombra medida pelo LDR. VALOR PROVISÓRIO: medir na
// calibração (dispensar várias vezes, pesar a ração e dividir pela contagem).
#define GRAMAS_POR_UNIDADE   0.5f

#define QTD_MIN_G            10       // g
#define QTD_MAX_G            200      // g
#define QTD_PADRAO_G         60       // g, usado até chegar o valor do slider
#define LIMITE_DIARIO_G      400      // g
#define T_MAX_DISPENSA_MS    15000UL  // T_máx
#define COOLDOWN_MS          60000UL  // intervalo mínimo entre dispensas

// ---------------- Sensor LDR (barreira de luz com LED branco) ----------------
// Leitura de luz em 0..4095 (ADC de 12 bits). Use o comando "ldr" na Serial
// para ver os valores reais e ajustar os parâmetros abaixo.
#define LDR_PERIODO_MS       1        // intervalo entre leituras
#define LDR_ZONA_MORTA       40       // variação ignorada (ruído), em unidades do ADC
#define LDR_ESCALA           1000UL   // sombra acumulada (ADC x ms) por "unidade"
#define LDR_BASE_MIN         800      // abaixo disso: LED apagado ou LDR solto
// true: mais luz = leitura maior (LDR no 3V3 e 10 kΩ no GND, como no esquema).
// false: módulo/ligação invertida (ex.: sensor de luz do Wokwi).
#define LDR_MAIS_LUZ_MAIOR   true

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
#define FEED_STATUS      "pf-status"  // só o estado
#define FEED_MENSAGEM        "pf-mensagem" // código e mensagem de erro/recusa
#define FEED_CONTAGEM    "pf-contagem"
#define FEED_GRAMAS      "pf-gramas"
#define FEED_TOTAL_DIA   "pf-total-dia"
#define FEED_LUZ         "pf-luz"     // leitura do LDR (0..4095), para diagnóstico

// Intervalo de publicação de pf-luz. O plano gratuito aceita 30 mensagens/min
// no total: 10 s = 6/min, sobra folga para os outros feeds.
#define LUZ_INTERVALO_MS 10000UL
