# PetFeeder IoT

Firmware do alimentador remoto de ração (ESP32 + servo + sensor IR + Adafruit IO).
Especificação completa em `docs/` (Etapas 1 e 2).

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `auto-feeder.ino` | Máquina de estados, sensor, servo, Wi-Fi/MQTT, comandos Serial |
| `config.h` | Pinos, ângulos, limites, tempos e nomes dos feeds |
| `secrets.example.h` | Modelo de credenciais (copiar para `secrets.h`, que não vai para o git) |
| `diagram.json`, `wokwi.toml` | Simulação no Wokwi (o botão azul simula o sensor IR) |

## Dependências

- Core `esp32:esp32` (testado com 3.3.12), placa "ESP32 Dev Module"
- Bibliotecas: `ESP32Servo` e `PubSubClient`

```sh
arduino-cli lib install ESP32Servo PubSubClient
cp secrets.example.h secrets.h   # e preencher Wi-Fi e AIO Key
arduino-cli compile -b esp32:esp32:esp32 --output-dir build .
arduino-cli upload  -b esp32:esp32:esp32 -p /dev/ttyUSB0 .
```

## Adafruit IO

Criar os feeds `pf-comando`, `pf-quantidade`, `pf-status`, `pf-contagem`, `pf-gramas`
e `pf-total-dia`. No dashboard:

- Botão momentâneo ligado a `pf-comando`, com valor de pressionar `1` e de soltar `0`.
- Slider em `pf-quantidade`: mínimo 10, máximo 200, passo 10.
- Bloco de texto/stream em `pf-status`, gráficos em `pf-contagem` e `pf-gramas`,
  indicador (gauge) em `pf-total-dia`.

## Testes pela Serial (115200 baud)

| Comando | Ação |
|---|---|
| `d 60` | Pede uma dispensa de 60 g (mesmas regras R1 a R7 do comando remoto) |
| `abrir` / `fechar` | Abre e fecha a comporta manualmente e mostra a contagem (calibração) |
| `s` | Mostra estado, contagem, total do dia e conexão |
| `zerar` | Zera o total do dia |

Para testar sem rede, defina `HABILITAR_REDE false` em `config.h`.

### Calibração de `GRAMAS_POR_UNIDADE`

1. `abrir`, deixar cair uma porção, `fechar` e anotar `n`.
2. Pesar a ração e calcular `gramas / n`.
3. Repetir algumas vezes, usar a média em `config.h`.

## Wokwi

Compilar com `--output-dir build` e abrir o projeto com a extensão Wokwi do VS Code.
No simulador use `WIFI_SSID "Wokwi-GUEST"` e `WIFI_SENHA ""`. Cada clique no botão azul
conta uma unidade (0,5 g com o valor provisório).
