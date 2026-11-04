// =====================================================================
// Modelo de credenciais. Copie para "secrets.h" e preencha.
//   cp secrets.example.h secrets.h
// O arquivo secrets.h está no .gitignore e NÃO deve ir para o repositório.
// =====================================================================
#pragma once

#define WIFI_SSID     "nome-da-rede"
#define WIFI_SENHA    "senha-da-rede"

// Adafruit IO: https://io.adafruit.com -> ícone da chave ("My Key")
#define AIO_USUARIO   "seu-usuario"
#define AIO_CHAVE     "aio_xxxxxxxxxxxxxxxxxxxxxxxxxxxx"

// Opcional: certificado raiz (PEM) do io.adafruit.com para validar o TLS.
// Sem ele, a conexão é criptografada mas o servidor não é autenticado.
// #define AIO_ROOT_CA "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
