# THE INDEX — Prescript Device
> *"As Hermes wills: Fulfill. Or face the consequence."*

Dispositivo inspirado no **Index Nursefather** do jogo **Limbus Company (Project Moon)**, construído em um **M5StickS3**.

![M5StickS3](https://img.shields.io/badge/Hardware-M5StickS3-blue)
![Arduino](https://img.shields.io/badge/Framework-Arduino-teal)
![ESP32S3](https://img.shields.io/badge/Chip-ESP32--S3-orange)

---

## 📱 Hardware

| Componente | Especificação |
|---|---|
| Placa | M5StickS3 |
| Chip | ESP32-S3-PICO-1 |
| Flash | 8MB |
| Display | 1.14" LCD 135x240 |
| Áudio | Codec ES8311 |
| IR | LED transmissor GPIO46 |
| Bateria | 250mAh LiPo |

---

## ⚡ Modos

### HERMES — Prescript Device
O modo principal. Recebe e exibe prescrições com efeito de scramble/decifrar, inspirado no dispositivo do Rien.
- Prescrições aleatórias do universo Limbus Company
- Servidor HTTP para envio remoto via browser
- Timeout com alerta sonoro
- Estados: INCOMING → SCRAMBLE → DISPLAY → CLEAR/ERROR

### HADES — TV Killer IR
Envia sequências de comandos IR para desligar TVs ao redor.
- Suporte a LG WebOS, Samsung, Sony, NEC, Panasonic, Sharp
- LED IR via GPIO46 usando driver RMT nativo do ESP32-S3
- Cada código enviado 3 vezes para garantir recepção

### ZEUS — Fake AP + Captive Portal
Cria um ponto de acesso WiFi falso com portal de login estilo Google.
- SSID configurável (padrão: "Google Wifi")
- Captive portal com página de login do Gmail
- Captura e exibe credenciais inseridas
- Contador de clientes conectados

### ARES — WiFi Scanner
Scanner passivo de redes WiFi ao redor com informações detalhadas.
- SSID, BSSID (MAC), canal, frequência (2.4/5GHz)
- Tipo de segurança (OPEN, WEP, WPA, WPA2, WPA3)
- RSSI em dBm com barra de sinal visual
- Navegação entre redes com botão A

### APOLLO — Strobe Display
Modo strobo usando o display LCD.
- 4 velocidades: 200ms → 100ms → 50ms → 30ms
- Útil como sinalizador ou efeito visual

---

## 🎵 Áudio

Dois arquivos WAV do jogo gravados na SPIFFS:

| Arquivo | Quando toca |
|---|---|
| `index_message_1.wav` | INCOMING, CLEAR, ERROR |
| `index_message_2.wav` | SCRAMBLE (loop) e REVEAL |

Fallback automático para bipes caso os arquivos não sejam encontrados.

---

## 🕹️ Controles

| Botão | Ação |
|---|---|
| **A** no menu | Navegar entre modos |
| **B** no menu | Selecionar modo |
| **A** no HERMES | Nova prescript aleatória |
| **A (hold 2s)** no HERMES | `.CLEAR/` |
| **B** em qualquer modo | Voltar ao menu |
| **A** no HADES | Repetir sequência IR |
| **A** no ZEUS | Ligar/desligar AP |
| **A** no ARES | Escanear / próxima rede |
| **A** no APOLLO | Iniciar / mudar velocidade |

---

## 🔧 Dependências

### Arduino IDE
- **Board:** M5Stack → M5StickS3
- **Partition Scheme:** 8M with spiffs (3MB APP/1.5MB SPIFFS)

### Bibliotecas
- [M5Unified](https://github.com/m5stack/M5Unified)
- ESP32 IDF `driver/rmt_tx.h` (nativo)

### Removidas
- ~~IRremoteESP8266~~ — substituída pelo driver RMT nativo
- ~~ESP8266Audio~~ — substituída pelo `M5.Speaker.playRaw()`

---

## 📁 Estrutura

```
prescript_device_final/
├── prescript_device_final.ino
└── data/
    ├── index_message_1.wav  (mono, 48000Hz, 16bit)
    └── index_message_2.wav  (mono, 48000Hz, 16bit)
```

---

## 🚀 Como gravar

### 1. Código
Abra o `.ino` no Arduino IDE e dê **Upload** normalmente.
> ⚠️ Pode ser necessário colocar o M5Stick em modo download (segurar reset) antes do upload.

### 2. Arquivos de áudio (SPIFFS)
Execute no CMD após o upload do código:

```bash
# Cria a imagem SPIFFS
"C:\...\mkspiffs.exe" -c "data" -s 1572864 -b 4096 -p 256 "spiffs.bin"

# Grava na flash
"C:\...\esptool.exe" --chip esp32s3 --port COM4 --baud 921600 write_flash 0x670000 "spiffs.bin"
```

> 📍 Endereço SPIFFS confirmado pela tabela de partições: `0x670000`

---

## ⚙️ Configuração

No início do código, ajuste:

```cpp
#define WIFI_SSID  "SEU_WIFI_AQUI"
#define WIFI_PASS  "SUA_SENHA_AQUI"
```

---

## 📸 Interface

```
┌─────────────────────────────┐
│ [ THE INDEX ]      BAT:85%  │
├─────────────────────────────┤
│ > HERMES  PRESCRIPT DEVICE  │
│   HADES   TV KILLER IR      │
│   ZEUS    FAKE AP + PORTAL  │
│   ARES    WIFI SCANNER      │
│   APOLLO  STROBE DISPLAY    │
├─────────────────────────────┤
│ [A]NAV  [B]SELECT           │
└─────────────────────────────┘
```

---

## ⚠️ Aviso

Este projeto foi desenvolvido para fins educacionais e criativos, inspirado em ficção científica. Use com responsabilidade e apenas em ambientes controlados onde você tem autorização.

---

## 🎮 Créditos

Inspirado no universo de **Limbus Company** e **Library of Ruina** da [Project Moon](https://projectmoon.studio/).

> *"The Index keeps records of all things. What is prescribed, must be fulfilled."*
