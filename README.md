# PetFeeder IoT

Firmware do alimentador remoto de ração (ESP32 + servo + barreira de luz LED/LDR + Adafruit IO).
Especificação completa em `docs/` (Etapas 1 e 2).

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `auto-feeder.ino` | Máquina de estados, sensor, servo, Wi-Fi/MQTT, comandos Serial |
| `config.h` | Pinos, ângulos, limites, tempos e nomes dos feeds |
| `secrets.example.h` | Modelo de credenciais (copiar para `secrets.h`, que não vai para o git) |
| `diagram.json`, `wokwi.toml` | Simulação no Wokwi (o sensor de luz simula o LDR) |

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

Criar os feeds `pf-comando`, `pf-quantidade`, `pf-status`, `pf-mensagem`, `pf-contagem`, `pf-gramas`,
`pf-total-dia` e `pf-luz`. No dashboard:

- Botão momentâneo ligado a `pf-comando`, com valor de pressionar `1` e de soltar `0`.
- Slider em `pf-quantidade`: mínimo 10, máximo 200, passo 10.
- Bloco de texto em `pf-status` (só o estado), stream em `pf-mensagem` (código e mensagem), gráficos em `pf-contagem` e `pf-gramas`,
  indicador (gauge) em `pf-total-dia` e gráfico de linha em `pf-luz` (leitura do LDR a cada 10 s).

## Testes pela Serial (115200 baud)

| Comando | Ação |
|---|---|
| `d` | Dispensa a quantidade configurada |
| `d 60` | Pede uma dispensa de 60 g (mesmas regras R1 a R7 do comando remoto) |
| `q 80` | Define e salva a quantidade por dispensa (publica em `pf-quantidade`) |
| `abrir` / `fechar` | Abre e fecha a comporta manualmente e mostra a contagem (calibração) |
| `s` | Mostra estado, contagem, luz do LDR, total do dia e conexão |
| `ldr` | Liga/desliga a leitura contínua do LDR (alinhamento e ajuste) |
| `zerar` | Zera o total do dia |

Para testar sem rede, defina `HABILITAR_REDE false` em `config.h`.

### Sensor LDR

O FC-51 foi trocado por uma barreira de luz: um LED branco ilumina um LDR através do cano
e a ração que cai faz sombra. O LDR é lento para separar grãos, então o firmware soma a
sombra ao longo do tempo; cada `LDR_ESCALA` de sombra acumulada vira 1 unidade (`n`).

1. Monte LED e LDR frente a frente no cano e cubra o trecho contra a luz ambiente.
2. Rode `ldr` com o cano vazio: `luz` deve ficar estável e acima de `LDR_BASE_MIN`.
3. Passe ração na frente: `luz` deve cair bem abaixo de `base`. Se subir, troque
   `LDR_MAIS_LUZ_MAIOR`. Ajuste `LDR_ZONA_MORTA` para um pouco acima do ruído parado.
4. Se o sensor estiver sem luz (LED apagado, LDR solto), a dispensa é recusada com
   `recusado;motivo=sensor`.

### Calibração de `GRAMAS_POR_UNIDADE`

1. `abrir`, deixar cair uma porção, `fechar` e anotar `n`.
2. Pesar a ração e calcular `gramas / n`.
3. Repetir algumas vezes, usar a média em `config.h`.

## Wokwi

Compilar com `--output-dir build` e abrir o projeto com a extensão Wokwi do VS Code.
No simulador use `WIFI_SSID "Wokwi-GUEST"` e `WIFI_SENHA ""`. O módulo de luz do Wokwi é
invertido em relação ao esquema: use `LDR_MAIS_LUZ_MAIOR false`. Durante uma dispensa, baixe o
lux no sensor para simular a sombra da ração.

## Status e erros

`pf-status` recebe só o estado: `ocioso`, `dispensando`, `concluido`, `timeout` ou `erro`.
`pf-mensagem` recebe `<código>: <mensagem>` quando um pedido é recusado ou uma dispensa falha.
Um pedido recusado não muda o estado.

| Código | Quando |
|---|---|
| `E01` | Equipamento ocupado: dispensando, em cooldown após erro ou com comporta manual |
| `E02` | Quantidade fora da faixa (10 a 200 g) |
| `E03` | Limite diário seria ultrapassado |
| `E04` | Sensor LDR sem luz |
| `E05` | Tempo máximo atingido com quantidade parcial |
| `E06` | Sem fluxo de ração (reservatório vazio ou cano entupido) |

Depois de `concluido` o equipamento aceita um novo pedido na hora. Depois de `timeout`
ou `erro` ele aguarda o cooldown (`COOLDOWN_MS`) antes de aceitar outro.

## Licença

Distribuído sob a [Licença Apache 2.0](LICENSE).

Copyright 2026 Equipe PetFeeder IoT: Gustavo Gabriel Paterno, Jean Carlo Menezes Cunha,
Pedro Henrique Schuch, Gustavo Fontana Bertoldi e João Pedro Sumi Bebber.
