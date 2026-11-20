# Odyssey 2 / Philips Videopac G7000 — ESP32 (TTGO VGA32)

Emulador da **Magnavox Odyssey 2** (aka Philips Videopac G7000) para a placa **LilyGO TTGO VGA32 v1.2 / v1.4**. Núcleo **O2EM** (Daniel Boris / libretro-o2em). Mesma família de firmwares do Stella 2014 (Atari 2600) neste hardware: mesmo menu, SD, teclado PS/2 GPIO e áudio DAC.

Banner serial no boot: `FW 17  o2-boot`

### Referência de vídeo (FW 15 geometria) — não mexer

Confirmado no Come Come (K.C. Munchkin): zeros do placar inteiros e imagem tela cheia.

FW 16: o labirinto de baixo sumia porque `finish_display` rebobinava à linha 160 e repintava com o VDC do vblank (grade desligada, sprite solto). Agora só pinta linhas que o feixe não alcançou.

- VGA **320×240 DoubleScan** (`QVGA_320x240_60Hz`)
- Fonte **0–239** na VGA (`GAME_YOFF = 0`)
- **2× horizontal** (`blitO2Line4`, 160→320)
- Caracteres **2× vertical** no VDC
- **Não** voltar a 160 1:1, **não** recortar o topo com YOFF, **não** tirar DoubleScan, **não** pintar faixa preta embaixo

## Áudio

O som do chip **Intel 8244** está ligado: shift register de 24 bits (tom + ruído), um frame de 1056 samples, downmix para **22050 Hz** no DAC1 **GPIO 25** (jack da placa, timer hardware, LPF na ISR). Sem The Voice / G7400.

## Hardware

- Placa: LilyGO TTGO VGA32 (ESP32-WROVER ou PICO-D4), CPU **240 MHz**
- VGA **320×240 60 Hz** via FabGL `VGADirectController` — **sempre** `QVGA_320x240_60Hz` (NTSC/PAL não mudam o modo de vídeo)
- RGB222 (64 cores). Sem framebuffer 320×240: a ISR da VGA faz 2× horizontal a partir do buffer nativo do VDC (~160×240)

### Pinagem (v1.2 / defaults FabGL)

| Função | GPIO |
|---|---|
| VGA R0 / R1 | 21 / 22 |
| VGA G0 / G1 | 18 / 19 |
| VGA B0 / B1 | 4 / 5 |
| HSYNC / VSYNC | 23 / 15 |
| KBD DATA | 32 (override `bootl.rc` `kbddat`) |
| KBD CLK | 33 (override `bootl.rc` `kbdclk`) |
| Audio DAC | **25** (jack da placa, DAC1 — **não** I2S) |
| SD CS / MOSI / SCK | 13 / 12 / 14 |
| SD MISO | **2** (primário), **35** (fallback) |

**Nunca** use GPIO 25 como MISO: nesse firmware o 25 é o DAC. O fallback é 35.

## Cartão SD (FAT32)

O firmware **não cria pastas**. Crie `/ODYSSEY2` você mesmo.

```
/
├── bootl.rc          (opcional)
└── ODYSSEY2/
    ├── o2rom.bin     (BIOS 1 KB — obrigatório para jogar)
    ├── cache.txt     (gerado pelo firmware; não commitar)
    ├── K.C. Munchkin.bin
    └── …
```

Extensões: `.bin` `.rom` `.o2` (case-insensitive). Arquivo sem extensão é aceito se o tamanho estiver entre 2 KB e 16 KB e for múltiplo de 1024.

BIOS (não entra na lista de jogos): `o2rom.bin`, `o2bios.bin`, `odyssey2.bin`.

Sem `/ODYSSEY2` → `/ODYSSEY2 folder not found`. Sem cartão → `SD not found` (não reinicia). Sem BIOS → `BIOS missing (o2rom.bin)` no menu (não trava).

O firmware sonda CMD0 em MISO **2** e depois **35** antes de `SD.begin`. No serial você deve ver `SD probe`, `SD OK` ou `SD not found` — nunca um abort do task watchdog. Gravar **FAT32**, pasta exatamente `/ODYSSEY2` na raiz.

`cache.txt`: se faltar, o firmware mostra `Building file cache`, varre `/ODYSSEY2` e grava um nome por linha. **F7** reconstrói.

## `bootl.rc` (raiz do SD, opcional)

```
kbddat=32
kbdclk=33
magicb=36
```

`magicb` = GPIO extra ativo em LOW que abre o menu de ROMs. Compatível com [fg1998/ESP32_Bootloader](https://github.com/fg1998/ESP32_Bootloader).

## Teclado (PS/2)

- ISR GPIO no CLK (falling), scancode Set 2, core 0
- **Proibido** FabGL Keyboard / ULP nestes pinos
- No jogo, A–Z e 0–9 vão para a matriz 8×8 do Odyssey 2
- **QAOP + Espaço** e **setas + Espaço** = joystick (J1/J2, F6)
- F-keys são do firmware

### F-keys

| Tecla | Ação |
|---|---|
| F1 | Ajuda |
| F2 | Cor / P&B |
| F3 | Frameskip 0 / 1 / 2 (default 0) |
| F4 | Fire hold (jogador 1) |
| F5 | RESET do jogo (segure) |
| F6 | Joystick J1 / J2 |
| F7 | Rebuild do cache |
| F9 | Menu de ROMs |
| F10 | Recarrega o jogo |
| F11 | RESET do jogo |
| F12 | `ESP.restart()` (volta ao Bootloader) |

No menu: cima/baixo seleciona, esquerda/direita página (24), A–Z pula, Enter/Espaço carrega, ESC volta ao jogo.

## Compilar / gravar

Precisa de [PlatformIO](https://platformio.org/).

```bash
pio run -e vga32
```

Firmware em `.pio/build/vga32/firmware.bin` (flash em `0x10000`, partição `huge_app`).

Para o [ESP32_Bootloader](https://github.com/fg1998/ESP32_Bootloader):

```bash
pio run -e vga32_bootloader
```

Copie `.pio/build/vga32_bootloader/firmware.bin` para a pasta do emulador no SD (ex. `/O2EM/firmware.bin`) junto com `version.txt`. O firmware apaga `otadata` no boot para devolver o controle ao bootloader.

Upload USB (env padrão):

```bash
pio run -e vga32 --target upload
```

## Núcleo

O2EM — CPU Intel 8048 + VDC Intel 8244 (NTSC) / 8245 (PAL interno). VGA permanece 60 Hz; PAL roda o core a 50 Hz com vsync extra.

Limite deste slice: cartuchos clássicos até **16 KB**. Sem The Voice / Videopac+ G7400.

**Didi na Mina Encantada** é o *Pick Axe Pete* brasileiro (4 KB). O O2EM precisa de `evblclk=12000`. FW 17 aplica isso pelo CRC conhecido **e** pelo nome (`DIDI` / `ENCANTADA` / `PICKAXE`), e trata dump 8 KB duplicado como 4 KB. No serial: `ROM crc=… size=… bank=… evbl=12000`.

## Licenças

Host GPLv3 (FabGL). Núcleo O2EM: Clarified Artistic License (`COPYING.o2em`). BIOS e ROMs **não** estão neste repositório.
