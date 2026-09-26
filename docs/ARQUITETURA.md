# Arquitetura

Documento técnico: como o Mobilador é construído, quais decisões de latência foram tomadas e por quê.

---

## 1. Visão geral do pipeline

```
  CELULAR (servidor do Mobilador)                     PC (aplicativo)
  ──────────────────────────────                     ────────────────
  VirtualDisplay/MediaProjection
        │  superfície                     ┌─────────────── socket de vídeo ──┐
        ▼                                 │        (ler, decodificar)       │
  MediaCodec (H.264/H.265, sem B-frames)  │   ┌─────────────────────────┐   │
        │  bytes Annex-B                   │   │ Media Foundation (GPU)  │   │
        ▼                                 │   │  sample → textura NV12  │   │
  montagem do pacote (cabeçalho 18 B)     │   └────────────┬────────────┘   │
        │                                 │        mailbox (1 slot)          │
        ▼                                 │                │                 │
  socket TCP ──► adb forward ──► 127.0.0.1:27183 ─────────┘                 │
        ▲                                                                    │
        │                                                                    │
  InputManager/uinput ◄── 127.0.0.1:27184 ◄── raw input (mouse) + WM_KEY (teclado)
                                                       ▲
                                        D3D11: NV12 → uma passada → swapchain flip
                                                       │
                                                  apresentar
```

Três canais, todos por USB, todos `127.0.0.1` do lado do PC com `adb forward`:

| Canal | Porta | Direção | Conteúdo |
|---|---|---|---|
| Vídeo | 27183 | celular → PC | cabeçalho + bitstream H.264/H.265, configuração, estatísticas, relatório do dispositivo, ping/pong |
| Entrada | 27184 | PC → celular | mouse (relativo/absoluto), botões, scroll, teclas, toque, probes |
| Áudio | 27185 | celular → PC | AAC (opcional) |

---

## 2. Regras de engenharia do projeto

| Regra | Motivo |
|---|---|
| C++17 **sem STL** | Sem alocador global no caminho quente, sem surpresas de layout, sem exceções. `Str` (não proprietária), `Arena` (nunca realoca, ponteiros estáveis por toda a sessão), `Vec<T>`, `StrMap`. |
| Sem exceções, sem RTTI | Binário menor, caminho quente previsível. Falhas são valores de retorno. |
| Tudo da API do Windows em `src/platform/` | O resto do código não inclui `windows.h`; portar para outra plataforma é mexer em uma pasta. |
| Zero alocação por frame | Todas as estruturas de tamanho fixo; o batcher de UI usa um único VB/IB mapeado por frame. |
| Nada de espera ocupada | Todas as esperas usam eventos ou o objeto de espera do swapchain; o app não queima CPU parado. |
| Nada de número inventado | Toda métrica exibida existe em `Sampler` e vem de uma medição; quando não há medição, o campo fica vazio. |

---

## 3. Protocolo binário

Cabeçalho fixo de **18 bytes** (idêntico em `src/pipeline/protocol.h` e `CodecMath.java`; a igualdade é verificada por `tools/check_protocol.py`):

| Offset | Tamanho | Campo |
|---|---|---|
| 0 | 8 | magic `0x4D4F42494C41444F` ("MOBILADO") |
| 8 | 1 | tipo |
| 9 | 1 | flags (`1` = quadro-chave, `2` = configuração, `0`/`4` = H.264/H.265) |
| 10 | 4 | sequência |
| 14 | 4 | tamanho do conteúdo |

Todos os inteiros em **big-endian**.

| Tipo | Nome | Conteúdo |
|---|---|---|
| 1 | video-config | largura, altura, codec, fps + conjuntos de parâmetros (SPS/PPS/VPS) em Annex-B |
| 2 | video-frame | prefixo de 20 B + bitstream. Prefixo: `captureUs` (u64), `encodeUs` (u16), fila de rede (u16), largura (u16), altura (u16) |
| 3 | frame-meta | metadados por quadro |
| 4 / 5 | ping / pong | id + relógio do celular (µs) |
| 6 | keyframe-req | pedido de quadro-chave (recuperação rápida) |
| 7 / 8 | audio-config / audio-frame | audio |
| 9 | stats | backend de entrada ativo, contadores do celular |
| 10 | device-info | JSON: modelo, Android, SDK, ABI, resolução, taxa do painel, encoders, decoders |
| 20–25 | touch, key, mouse-move, mouse-button, scroll, gamepad | entrada |
| 30 | quit | encerramento pedido pelo PC |

Blocos de `str_fmt_temp` e buffers de linha são usados para montar os textos dos relatórios, sem alocação dinâmica.

---

## 4. Transporte (`src/pipeline/mirror.cpp`)

Três threads, cada uma com uma responsabilidade e uma prioridade:

| Thread | Prioridade | O que faz |
|---|---|---|
| `mirror-read` | acima do normal | lê o socket de vídeo, valida magic, faz o parse dos pacotes, alimenta o decodificador, publica no mailbox e mede captura/USB/encode |
| `mirror-input` | **tempo crítico** | drena o anel de entrada e envia; faz os probes de RTT a cada 500 ms; interpreta os pongs |
| `mirror-watch` | abaixo do normal | reconexão, presença do dispositivo, reinício do servidor do celular quando ele morre |

Decisões que importam:

- **Sem fila de frames.** O decodificador publica em um `Mailbox<VideoFrame>` de um único slot: o consumidor pega o mais novo e o antigo é liberado, nunca exibido atrasado. Se um frame é substituído antes de ser mostrado, ele conta como descartado — e isso aparece na interface.
- **Sem polling.** As esperas usam eventos (`Event`) no lado da entrada e o objeto de espera do swapchain no lado da apresentação; o watchdog usa intervalos de 1,5 s (parado) e 5 s (transmitindo), porque criar processo de ADB custa caro.
- **RTT mínimo para alinhar relógios.** O deslocamento entre o relógio do celular e o do PC é estimado pelo menor RTT de uma janela de 5 s. Menor RTT = menos fila = melhor estimativa. Nunca média.
- **Recuperação em camadas.** Socket morto → reconecta; servidor morto → reinicia via ADB; cabo fora → estado honesto na tela e reconexão automática quando volta.
- **Sem acumular entrada.** O anel de entrada tem 1024 posições e descarta quando cheio (com contador visível), porque um evento de mouse velho é pior que um evento perdido.

---

## 5. Decodificação (`src/video/`)

- **Media Foundation**, com decodificador de hardware obtido por `MFTEnumEx` e *device manager* D3D11: a saída já é uma textura NV12 na GPU, sem cópia pela memória principal.
- **Um único sample/buffer reutilizado** e modo assíncrono com *timeout* de 4 ms: o decodificador nunca bloqueia a espera por uma saída que não vem.
- **ICodecAPI ligado** (`AVLowLatencyMode`, `AVDecNumWorkerThreads = 2`): sem isso o decoder acumula quadros internamente.
- **Views planares memorizadas.** Cada textura tem suas views de SRV (Y como `R8_UNORM`, UV como `R8G8_UNORM`) guardadas em cache com contagem de referência e política circular — sem alocar objeto COM por quadro. (O `d3d11.h` do mingw não conhece `PlaneSlice`, então a struct do descritor é reproduzida localmente com o layout exato do SDK.)
- **Conjuntos de parâmetros** (SPS/PPS/VPS) vêm no pacote de configuração e também *in-band* nos quadros-chave; o caminho de software (quando não há decodificador por hardware) sobe Y e UV para duas texturas dinâmicas, mantendo o mesmo shader.

---

## 6. Apresentação (`src/render/`)

- Swapchain **flip model** (`DXGI_SWAP_EFFECT_FLIP_DISCARD`). O modelo *bitblt* copiaria por uma superfície intermediária — um quadro inteiro de atraso.
- `SetMaximumFrameLatency(1)` + objeto de espera: **exatamente um quadro em voo**; a espera é do kernel, não é laço ocupado.
- `ALLOW_TEARING` + `Present(0, TEARING)` no perfil de latência mínima: sem espera de vsync.
- Fullscreen **sem borda** (`WS_POPUP` sobre o monitor) em vez de fullscreen exclusivo: no flip model isso vira *independent flip*, o DWM é contornado e não há custo de composição.
- O quadro NV12 é desenhado em **uma passada** (shader `ps_video` amostra as duas texturas e escreve RGB direto), sem *shader* de conversão intermediário e sem render target extra.
- O batcher da interface usa um único par de buffers dinâmicos e encerra o desenho em um `cmd_start_index`; o quadro de vídeo é desenhado **antes** dos lotes de UI, na ordem correta, sem troca de estado desnecessária.

---

## 7. Entrada (`src/ui/app.cpp` + `android-server/InputServer.java`)

Caminho de ida:

1. `WM_INPUT` (mouse bruto, sem aceleração) → delta inteiro;
2. escala por `mouse_sensitivity`;
3. pacote `mouse-move` de 12 bytes com a idade do evento em ms;
4. anel de entrada → thread de tempo crítico → socket.

Caminho de volta (no celular), escolhido automaticamente:

- **uinput (preferido):** dispositivos virtuais de mouse/teclado/toque criados em `/dev/uinput`. O usuário `shell` do ADB pertence ao grupo `input`, então os eventos entram no kernel como eventos reais e o jogo vê um mouse de verdade. Custo: um `write()`.
- **InputManager (fallback):** `injectInputEvent(..., MODE_ASYNC)`, sem *spawn* de processo por evento (diferente do comando `input` do shell).

O tempo do evento é reconstruído no celular a partir da idade informada pelo PC (`eventTime = uptimeMillis() - idade`), sem depender de sincronismo entre as duas máquinas. Um `eventTime` errado faria o Android coalescer ou repetir movimento — e isso aparece como engasgo.

Não existe mapeamento de teclas em nenhum ponto do caminho: o que o PC lê é o que o Android recebe.

---

## 8. Telemetria (`src/pipeline/telemetry.cpp`)

- `MetricId`: CAPTURE, ENCODE, USB, DECODE, RENDER, INPUT, TOTAL, QUEUE.
- Cada métrica guarda um anel de 1024 amostras (p50/p95/p99 calculados sob demanda, cópia ordenada em buffer estático — sem alocar).
- `FpsCounters` separa origem, stream, exibição e descartes.
- `SystemStats` (4 Hz): CPU, RAM, e do lado da GPU o que o sistema informa; nada é estimado por "cara de número".
- `Sampler` mantém a sessão, escreve o log em CSV e alimenta o benchmark.

O benchmark não é uma simulação: ele troca a configuração real, espera estabilizar, mede por alguns segundos e registra os mesmos números que o analisador mostra.

---

## 9. Interface (`src/ui/`)

- **Tema:** um único acento governa todas as superfícies; `Theme::apply(mode, accent)` recalcula cores derivadas (fundos, bordas, texto, estados) a partir do acento e do modo (escuro/claro/AMOLED).
- **Widgets imediatos:** botão, botão com ícone, botão grande, toggle, segmentado, dropdown, slider, campo de texto, campo de atalho, cabeçalho de seção, cartão, linha chave-valor, indicador de estado, etiqueta, medidor, bloco de estatística, gráfico, estado vazio, barra de rolagem, toast, tooltip, modal.
- **Ícones:** `src/ui/Icons.generated.h`, 83 ícones gerados por `tools/gen_assets.py` — traço único, 24×24, sem emoji e sem ícone de terceiros.
- **Movimento:** 120–180 ms, e **zero** quando o modo jogo está ativo (o `dt` das animações é zerado, não escondido).

---

## 10. Threads do aplicativo

| Thread | Prioridade | Papel |
|---|---|---|
| UI / apresentação (principal) | normal | mensagens do Windows, desenho, apresentação |
| `mirror-read` | acima | rede + decodificação |
| `mirror-input` | tempo crítico | entrada |
| `mirror-watch` | abaixo | manutenção e reconexão |
| threads internas do Media Foundation | — | decodificação assíncrona |

Não há thread por conexão, nem thread ociosa, nem `Sleep` curto em laço.

---

## 11. Build

`tools/build.py`:

1. gera/atualiza os recursos (ícone, versão) com `zig rc` ou `windres`;
2. compila cada `.cpp` em paralelo, incremental por data (com um arquivo de dependências por objeto);
3. liga com `-municode -mwindows -static`, subsistema gráfico, com *strip* em release;
4. bibliotecas: `d3d11 dxgi d3dcompiler_47 mfplat mfreadwrite ole32 oleaut32 user32 gdi32 shell32 shlwapi advapi32 setupapi cfgmgr32 avrt winmm dwmapi shcore version ws2_32 iphlpapi userenv bcrypt crypt32 propsys psapi`.

Duas observações que economizam horas:

- a biblioteca do compilador de shaders é **`d3dcompiler_47`** (não existe `d3dcompiler` no mingw);
- `mfuuid`/`dxguid` **não** são usadas: o projeto declara localmente os poucos GUIDs de Media Foundation e DXGI de que precisa, então um toolchain sem essas bibliotecas continua ligando.

O CI (`.github/workflows/build.yml`, pronto em `tools/ci-build.yml`) faz o mesmo em Linux com `ziglang` e, em paralelo, compila o `mobilador.dex` com JDK 17 + `d8`.

---

## 12. Verificações automáticas

| Ferramenta | O que garante |
|---|---|
| `tools/run_tests.sh` | 146 testes do núcleo (Arena, Vec, StrMap, SPSC, mailbox, threads, I/O), compilados com ASAN |
| `tools/check_protocol.py` | os dois lados do protocolo (C++ e Java) com os mesmos magic, tipos, flags e tamanhos |
| `tools/check_java_api.py` | nenhuma referência a API da plataforma Android que não exista no `android.jar` (lê o `.class` do jar, sem precisar de SDK nem JDK) |
| `tools/build.py` | compilação e ligação do executável Windows |
