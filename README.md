# MOBILADOR

**Espelhamento de tela de celular Android por USB, com latência mínima, para jogar Free Fire no PC com mouse e teclado.**

O Mobilador transporta a tela do celular para o monitor do PC e devolve os eventos de mouse e teclado para o celular — tudo pelo cabo USB, sem Wi-Fi, sem nuvem e sem intermediários. Ele **não** é um keymapper: não cria teclas, não edita HUD, não mexe no jogo. O mapeamento (qual tecla faz o quê dentro do jogo) é responsabilidade do **GG Mouse Pro 3**, que roda no próprio celular. O Mobilador entrega a ele um mouse e um teclado de verdade, com o menor atraso que o hardware permite.

```
  CELULAR (Android)            PC (Windows)
  ┌──────────────────┐        ┌──────────────────────────────────────────┐
  │ Free Fire        │        │  MOBILADOR                               │
  │ GG Mouse Pro 3   │◄──┐    │                                          │
  │ servidor do      │   │    │  decodifica na GPU ─► apresenta          │
  │ Mobilador (dex)  │   │    │  (frame mais novo, nunca enfileirado)    │
  └────────┬─────────┘   │    │                                          │
           │             │    └───────────────▲──────────────────────────┘
           │ vídeo H.264/H.265 (USB)          │ mouse + teclado (USB)
           └──────────────────────────────────┘
                    ADB (adb forward)
```

---

## Índice

- [O que o Mobilador é](#o-que-o-mobilador-é)
- [O que o Mobilador não é](#o-que-o-mobilador-não-é)
- [Requisitos](#requisitos)
- [Instalação](#instalação)
- [Primeiro uso](#primeiro-uso)
- [Telas](#telas)
- [Modo jogo e atalhos](#modo-jogo-e-atalhos)
- [FPS: origem, stream e exibição](#fps-origem-stream-e-exibição)
- [Latência: como cada número é medido](#latência-como-cada-número-é-medido)
- [Presets](#presets)
- [Configurações avançadas](#configurações-avançadas)
- [Aparência](#aparência)
- [Diagnóstico e benchmark](#diagnóstico-e-benchmark)
- [Onde ficam os arquivos](#onde-ficam-os-arquivos)
- [Compilando do código-fonte](#compilhando-do-código-fonte)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Estado de verificação](#estado-de-verificação)
- [Limites, segurança e uso responsável](#limites-segurança-e-uso-responsável)
- [Solução de problemas](#solução-de-problemas)

Documentos detalhados:

| Documento | Conteúdo |
|---|---|
| [docs/INSTALACAO.md](docs/INSTALACAO.md) | instalação passo a passo, preparação do celular, módulo do servidor, ADB |
| [docs/USO.md](docs/USO.md) | uso diário, telas, modo jogo, atalhos, perfis, benchmark |
| [docs/REFERENCIA_CONFIGURACAO.md](docs/REFERENCIA_CONFIGURACAO.md) | todas as opções do `settings.ini`, uma por uma, em linguagem simples |
| [docs/ARQUITETURA.md](docs/ARQUITETURA.md) | pipeline, protocolo, threads, decisões de latência |
| [docs/SOLUCAO_DE_PROBLEMAS.md](docs/SOLUCAO_DE_PROBLEMAS.md) | cada item do diagnóstico com o que fazer quando não está OK |

---

## O que o Mobilador é

- **Transporte USB real.** Detecta o celular, cria as portas com `adb forward`, inicia o servidor no aparelho, conecta, e reconecta sozinho quando o cabo é mexido ou o aplicativo do celular reinicia.
- **Pipeline de latência mínima.** Captura → encode (celular) → USB → decode (GPU do PC) → apresentação. Cada etapa foi escolhida para subtrair atraso: sem B-frames, sem fila de frames, swapchain flip com *1 frame em voo*, sem cópia desnecessária, sem conversão de cor na CPU.
- **Sempre o frame mais novo.** O quadro decodificado vai para um *mailbox* de um único slot: se um novo chega antes do anterior ser apresentado, o antigo é descartado. Nunca há atraso acumulando.
- **Entrada de baixíssima latência.** Mouse por *raw input* (sem aceleração do Windows), capturado em modo jogo, empacotado e enviado no mesmo instante. Teclado pelo *message loop*, com fila curta e thread prioritária.
- **Modo jogo de verdade.** Fullscreen sem borda, cursor escondido, mouse capturado, foco exclusivo, tela preenchendo o monitor com adaptação inteligente de proporção, e **zero animação** enquanto o jogo está na frente.
- **Números honestos.** FPS de origem, do stream e de exibição são medidos em pontos diferentes; cada etapa de latência que o Mobilador mostra vem de uma medição real. Nada é inventado, nada é simulado, nada é arredondado para parecer melhor.
- **Identidade própria.** Um único acento de cor governa botões, bordas, indicadores, sliders, ícones, estados ativos, gráficos e menus. Três temas (escuro, claro, AMOLED preto). Biblioteca de ícones própria, desenhada em uma só linguagem.

## O que o Mobilador não é

Isto é uma decisão de projeto, não uma limitação acidental:

- ❌ **Não tem keymapper.** Nenhum editor de teclas, nenhum editor de HUD, nenhuma tela de controles. GG Mouse Pro 3 faz isso, no celular.
- ❌ **Não tem aimbot, automação de mira, macro, auto-fire, controle de recuo ou qualquer assistência de jogo.**
- ❌ **Não injeta código no Free Fire, não lê nem altera a memória do jogo, não modifica APK, não contorna anti-cheat.**
- ❌ **Não usa Wi-Fi** como transporte principal. USB é o caminho.
- ❌ **Não mostra número falso.** Se não há medição, o campo fica vazio.

---

## Requisitos

**PC**

- Windows 10 64 bits (1903 ou superior) ou Windows 11.
- GPU com driver atualizado (decodificação por hardware do H.264/H.265).
- ADB (Android Platform Tools) instalado **ou** o `adb.exe` colocado ao lado do `Mobilador.exe` (o app procura nos dois lugares, e também em `%LOCALAPPDATA%\Android\Sdk\platform-tools`).
- Opcional: JDK 8 ou superior, **somente** se você quiser compilar o módulo do servidor no próprio PC (veja [Instalação](docs/INSTALACAO.md)).

**Celular**

- Android 7.0 (API 24) ou superior.
- Depuração USB habilitada.
- Cabo USB de dados (não use cabo de "somente carga").

**Rede**

- Nenhuma. Nada de Wi-Fi, nada de servidor externo, nada de conta.

---

## Instalação

### 1. Compilar (ou usar) o executável

```bash
python3 tools/build.py          # gera dist/Mobilador.exe
```

O script aceita `--debug` (console + símbolos), `--clean` e `--toolchain mingw`. O único requisito é um toolchain C++: ele usa `zig` automaticamente se estiver disponível, ou `g++`/MSVC (`cl`) no Windows.

### 2. Instalar o módulo do servidor no celular

O `Mobilador.exe` conversa com um pequeno módulo que roda no celular (arquivo `mobilador.dex`). Existem duas formas de consegui-lo:

- **Já existe:** coloque `mobilador.dex` em `server\mobilador.dex` ao lado do `Mobilador.exe`. O app envia para o celular automaticamente na primeira conexão.
- **Compilar:** com um JDK instalado,

  ```bash
  cd android-server && ./build.sh          # Linux/macOS
  cd android-server && build.bat           # Windows
  ```

  Isso produz `dist/mobilador.dex`. Sem JDK no PC, o módulo também é gerado automaticamente pelo GitHub Actions (arquivo `tools/ci-build.yml`, pronto para ser ativado como `.github/workflows/build.yml`).

### 3. Preparar o celular

1. Configurações → Sobre o telefone → toque 7× em "Número da versão".
2. Opções do desenvolvedor → **Depuração USB** → ativar.
3. Conecte o cabo e aceite **"Permitir depuração USB"** na tela do celular.
4. Recomendado: Opções do desenvolvedor → **Manter tela ligada** e desative o modo de economia de energia.

### 4. Rodar

1. `dist\Mobilador.exe`.
2. Se você compilou o módulo, deixe-o em `server\` ao lado do executável.
3. **START FREE FIRE** no painel — ou `F5` — para iniciar a sessão.
4. `F12` para entrar em **modo jogo**.

Passo a passo completo, com verificação de cada etapa: [docs/INSTALACAO.md](docs/INSTALACAO.md).

---

## Primeiro uso

1. O painel mostra o estado real da conexão: **● CELULAR CONECTADO** só aparece quando o dispositivo responde de fato ao ADB e o servidor abre as portas.
2. Confira **DIAGNÓSTICO**: os doze itens devem estar OK (ou ATENÇÃO, com explicação). Se o módulo do servidor não estiver instalado, o diagnóstico diz exatamente isso e o botão **INSTALAR** resolve.
3. Escolha um **preset**. Em uma máquina com GPU dedicada, `ULTRA LOW LATENCY` é o ponto de partida certo.
4. Inicie a sessão (`F5`) e depois o modo jogo (`F12`). O cursor desaparece, o mouse é capturado, o jogo não tem menus por cima.
5. `F9` solta o mouse, `F10` captura de novo, `F8` mostra/esconde o overlay, `F11` alterna fullscreen.

---

## Telas

**PAINEL** — nome do produto, estado da conexão, modelo e versão do Android do aparelho, USB, FPS (origem / stream / exibição), latência total, GPU, CPU, RAM e o botão principal **START FREE FIRE**. Botões secundários para DESEMPENHO, LATÊNCIA, CONFIGURAÇÕES e DIAGNÓSTICO.

**DESEMPENHO** — os cinco presets e todas as opções avançadas, agrupadas em três blocos (CAPTURA, PIPELINE, ENTRADA). Cada opção traz uma explicação em linguagem simples: o que faz, por que o padrão é aquele e o que muda na latência.

**ANÁLISE DE LATÊNCIA** — Captura, Encode, USB, Decode, Render e Entrada, cada uma com p50/p95/p99 e gráfico; Total estimado; FPS, Frame Time, Frames perdidos, Buffer, Bitrate, Resolução, CPU, GPU e RAM. Exportação para CSV.

**BENCHMARK** — sobe uma escada de configurações (720p60 → 1080p60 → 1080p90 → 1080p120 → 1440p60), mede cada degrau e mostra a comparação objetiva entre elas, indicando a configuração mais rápida que se mantém estável.

**DIAGNÓSTICO** — USB, ADB, servidor no celular, GPU, CPU, RAM, decodificador, renderizador, display, FPS, entrada e mouse/teclado, cada um com estado (OK / ATENÇÃO / ERRO), valor medido e uma explicação do que fazer. Inclui o log da sessão e o botão para gerar um relatório em texto.

**CONFIGURAÇÕES** — aparência (acento, tema, escala, animações), atalhos, transporte, decodificação, entrada, perfis salvos e o aviso explícito do que o Mobilador não faz.

---

## Modo jogo e atalhos

| Tecla | Ação |
|---|---|
| `F11` | Fullscreen (borderless) |
| `F8` | Mostra/esconde o overlay dentro do jogo |
| `F9` | Solta o mouse |
| `F10` | Captura o mouse |
| `F12` | Liga/desliga o modo jogo |
| `F5` | Inicia/para a sessão |
| `F6` | Captura de tela (BMP em Documentos\Mobilador) |
| `F7` | Mostra/esconde o log |

Todos os atalhos são reconfiguráveis em CONFIGURAÇÕES. As teclas de atalho são consumidas pelo Mobilador — nunca chegam ao celular, portanto nunca disparam nada dentro do jogo.

Com o modo jogo ligado: fullscreen sem borda, cursor oculto, mouse capturado com movimento contínuo (sem limite de borda da tela), foco exclusivo, overlay opcional e **sem nenhuma animação**. Ao perder o foco, o mouse é solto automaticamente; ao voltar, é recapturado. Com o modo jogo desligado, o cursor volta imediatamente e o mouse é liberado.

---

## FPS: origem, stream e exibição

Três números diferentes, medidos em três pontos diferentes — porque são coisas diferentes:

| Nome | O que significa | Como é medido |
|---|---|---|
| **FPS ORIGEM** | Quadros que o celular realmente produziu e codificou | Contagem dos *timestamps de captura* distintos enviados pelo celular, por segundo |
| **FPS STREAM** | Quadros que chegaram ao PC pelo USB | Contagem dos pacotes de vídeo recebidos, por segundo |
| **FPS EXIBIÇÃO** | Quadros efetivamente apresentados no monitor | Contagem dos `Present()` concluídos, por segundo |

Se o FPS de origem for maior que o do stream, o USB está sendo o gargalo. Se o stream for maior que a exibição, quem limita é a GPU ou o monitor. O modo **MAX FPS** escolhe automaticamente a maior taxa que o conjunto sustenta, sempre limitada pelo menor valor entre o painel do celular, o monitor e a capacidade real de decodificação — nunca anuncia um número que a máquina não entrega.

---

## Latência: como cada número é medido

O Mobilador mostra **apenas** o que mede. A frase correta nunca é "zero ms": é "o mais próximo possível de zero", e cada etapa abaixo existe para empurrar esse limite.

| Etapa | Definição exata | Fonte |
|---|---|---|
| **CAPTURA** | Do instante em que o celular capturou o quadro até ele chegar ao PC | `nanoTime()` do celular + alinhamento de relógio por RTT mínimo |
| **ENCODE** | Tempo que o codec do celular gastou para codificar o quadro | Informado pelo próprio encoder |
| **USB** | Da codificação pronta até a chegada dos bytes no PC | Diferença entre a chegada e o fim do encode |
| **DECODE** | Da entrega do pacote ao decodificador até a saída do quadro | Relógio local |
| **RENDER** | Do frame retirado do mailbox até o `Present()` concluído | Relógio local |
| **ENTRADA** | Ida e volta de um pacote de probe no canal de entrada | Medição ativa, 2× por segundo |
| **TOTAL** | Soma apenas das etapas que podem ser medidas com um relógio só | Soma das linhas acima |

O alinhamento de relógio entre celular e PC é feito com o **menor RTT** observado numa janela de tempo (o critério padrão para sincronizar dois relógios sem protocolo dedicado). Nada de média: a média carrega o desvio de fila e o número ficaria otimista.

---

## Presets

| Preset | Para quem | O que faz |
|---|---|---|
| **ULTRA LOW LATENCY** | Competitivo, PC com GPU dedicada | Sem vsync, sem pacing, 1 frame em voo, 1080p, taxa igual ao monitor, H.264/H.265 conforme o decodificador |
| **MAX FPS** | Monitor de 120/144 Hz | Maior taxa sustentável, bitrate e buffer ajustados para não engasgar |
| **BALANCED** | Uso geral | 1080p60, ~12 Mbps, imagem limpa e latência baixa |
| **QUALITY** | Ver a tela com qualidade | Mais bitrate, escala 1:1, sem descarte agressivo |
| **CUSTOM** | Ajuste fino | Nada é sobrescrito: o que você mudou fica |

Qualquer alteração manual em um preset muda o preset para CUSTOM automaticamente — o app nunca finge que você continua no preset anterior.

---

## Configurações avançadas

Todas as opções, com explicação em linguagem simples e o motivo do padrão, estão em
[docs/REFERENCIA_CONFIGURACAO.md](docs/REFERENCIA_CONFIGURACAO.md). Resumo:

| Opção | Padrão | O que faz |
|---|---|---|
| Resolução | 1920×1080 | Tamanho da captura no celular. Mais pixels = mais dados no cabo e mais tempo de encode |
| FPS | 60 | Taxa alvo da captura. `MAX` escolhe automaticamente a maior estável |
| Bitrate | 12 Mbps | Quanto o encoder pode gastar. Muito baixo gera blocos; muito alto satura o USB |
| Codec | H.264 | H.265 ocupa menos banda, mas nem todo decodificador é rápido nele |
| Buffer | 1 frame | Frames guardados antes de exibir. 1 é o mínimo que existe |
| Descarte de frames | Ligado | Permite jogar fora frames antigos em vez de enfileirar atraso |
| Aceleração de hardware | Ligado | Decodifica na GPU |
| Decodificador por GPU | Ligado | Prefere decodificadores de hardware da Media Foundation |
| VSync | Desligado | Sincroniza com o monitor (mais estável), mas adiciona até um quadro de atraso |
| Frame pacing | Desligado | Limita a apresentação à taxa alvo (ajuda em notebook com economia de energia) |
| Captura de mouse | Ligado | Mouse preso na janela, sem limite de borda |
| Prioridade de entrada | Ligado | Thread de entrada em prioridade de tempo crítico |
| Modo USB | Automático | Escolhe o caminho de maior banda disponível |
| Modo de renderização | Direto | Apresentação em uma passada, sem pós-processamento |
| Modo de escala | Proporcional | Mantém a proporção, sem deformar |
| Formato de cor | YUV 4:2:0 | Formato nativo dos encoders de tela; converter custaria tempo na CPU |
| Qualidade | Equilibrada | Ajuste interno do encoder (perfil/limites) |
| Priorizar frame mais novo | Ligado | Mailbox de um slot em vez de fila |
| Renderização de baixa latência | Ligado | Um frame em voo com objeto de espera (sem espera ocupada) |
| Sensibilidade do mouse | 1.00 | Multiplicador do movimento antes de enviar (o ajuste fino é do GG Mouse Pro 3) |

---

## Aparência

- **Acento configurável:** Azul, Roxo, Vermelho, Verde, Laranja, Ciano, Branco e **Personalizado** (matiz livre). O acento escolhido governa automaticamente botões, bordas, indicadores, sliders, ícones, estados ativos, gráficos e menus — não existe tela com cor "esquecida".
- **Temas:** DARK, LIGHT e AMOLED BLACK (preto puro, ideal para OLED).
- **Ícones:** biblioteca própria (`src/ui/Icons.generated.h`, gerada por `tools/gen_assets.py`), traço único de 1,75 unidades, 24×24, sem emojis e sem ícones de terceiros.
- **Logo:** símbolo, logo completa, ícone do executável (`.ico` multi-resolução), favicon e splash, todos em `assets/`, funcionando em fundo claro e escuro.
- **Movimento:** 120–180 ms, apenas onde ajuda a entender o que mudou — e **zero** em modo jogo.

---

## Diagnóstico e benchmark

**DIAGNÓSTICO** verifica doze itens e explica cada um: USB, ADB, servidor no celular, GPU, CPU, RAM, decodificador, renderizador, display, FPS, entrada e mouse/teclado. O relatório completo é salvo em `Documentos\Mobilador\diagnostico.txt`.

**BENCHMARK** compara configurações de forma objetiva: ele troca a configuração, deixa estabilizar, mede por alguns segundos e registra os números reais (FPS de exibição, latência total, frames perdidos). O resultado vai para `Documentos\Mobilador\benchmark.csv`.

**Log de desempenho**: opcional, salvo em `%LOCALAPPDATA%\Mobilador\logs\sessao.txt`, no mesmo formato dos números exibidos.

---

## Onde ficam os arquivos

| Caminho | Conteúdo |
|---|---|
| `%LOCALAPPDATA%\Mobilador\settings.ini` | todas as configurações |
| `%LOCALAPPDATA%\Mobilador\profiles\*.ini` | perfis salvos |
| `%LOCALAPPDATA%\Mobilador\logs\sessao.txt` | log da sessão |
| `Documentos\Mobilador\diagnostico.txt` | relatório de diagnóstico |
| `Documentos\Mobilador\latencia.csv` | exportação das métricas |
| `Documentos\Mobilador\benchmark.csv` | resultados do benchmark |
| `Documentos\Mobilador\captura-*.bmp` | capturas de tela |
| `server\mobilador.dex` (ao lado do .exe) | módulo do servidor para enviar ao celular |

---

## Compilando do código-fonte

```bash
python3 tools/build.py                  # Windows (zig, mingw ou MSVC) -> dist/Mobilador.exe
python3 tools/build.py --debug          # console + símbolos
python3 tools/build.py --clean
bash tools/run_tests.sh                 # testes do núcleo + verificações de protocolo/API
python3 tools/gen_assets.py             # regenera ícones, logo, splash e Icons.generated.h
```

O projeto é C++17 sem STL, sem exceções e sem RTTI (regras explicadas em [docs/ARQUITETURA.md](docs/ARQUITETURA.md)). O build é incremental e paralelo.

---

## Estrutura do repositório

```
src/core/        base (Str, Arena, Vec, StrMap), threads, log      — sem dependências externas
src/platform/    Win32: janela, monitor, fullscreen, cursor, adb   — toda a API do sistema fica aqui
src/render/      D3D11: dispositivo, swapchain, shaders, texto, batcher 2D
src/ui/          tema, widgets imediatos, ícones gerados, telas do aplicativo
src/pipeline/    protocolo binário, settings, telemetria, transporte (mirror)
src/video/       transformação de bitstream (Annex-B, SPS/PPS) e decodificador Media Foundation
src/adb/         cliente ADB: dispositivos, portas, envio do módulo
android-server/  módulo que roda no celular (Java: captura, encode, entrada, áudio)
tools/           build, geração de assets, testes, verificadores, workflow de CI
assets/          ícone, logo, splash
tests/           testes do núcleo (independentes de plataforma)
```

---

## Estado de verificação

Transparência total sobre o que foi verificado e o que ainda depende de uma máquina Windows:

| Item | Estado |
|---|---|
| Compilação completa do aplicativo Windows | ✅ `dist/Mobilador.exe` (PE32+ x64, subsistema gráfico, ícone e versão embutidos) |
| Testes do núcleo (Arena, Vec, StrMap, filas, threads, I/O) | ✅ 146 testes, ASAN limpo |
| Coerência do protocolo C++ ↔ Java | ✅ `tools/check_protocol.py` (18 bytes de cabeçalho, 23 tipos, magic) |
| Módulo Android contra a plataforma | ✅ `tools/check_java_api.py` (nenhum membro inexistente referenciado) |
| Execução real em Windows com celular conectado | ⏳ depende de um PC Windows com celular — não é possível executar neste ambiente |
| Compilação do `mobilador.dex` | ⏳ requer um JDK (ou o GitHub Actions); os fontes passam nas verificações estáticas |

Ou seja: o aplicativo **compila, liga e é coerente**, mas a validação final só existe no PC do usuário, com o celular na mão. O DIAGNÓSTICO existe exatamente para essa conversa: ele diz o que está medido e o que está faltando.

---

## Limites, segurança e uso responsável

- O Mobilador transporta **entrada** e **vídeo**. Ele não sabe o que é uma mira, um personagem ou um item; ele não lê nem escreve nada dentro do jogo.
- Não há injeção de código, hook, alteração de memória, modificação de APK, nem qualquer forma de burlar sistemas anti-cheat.
- Use com jogos e aplicativos que você tem o direito de usar dessa forma. Verifique os termos de uso do jogo e as regras do seu provedor: o uso de espelhamento de tela é responsabilidade do usuário.
- O acesso ao celular acontece exclusivamente por depuração USB, que você habilita e autoriza conscientemente. Nada trafega pela internet.

---

## Solução de problemas

Comece por **DIAGNÓSTICO** dentro do app: cada item diz o que está medido e o que fazer. As situações mais comuns estão detalhadas em [docs/SOLUCAO_DE_PROBLEMAS.md](docs/SOLUCAO_DE_PROBLEMAS.md):

- celular não aparece → ADB/depuração USB/cabo
- conecta e cai → economia de energia do USB, servidor reiniciando
- imagem engasgando → presets, bitrate, resolução, FPS
- mouse "preso" ou cursor reaparecendo → modo jogo, `F9`/`F10`
- imagem com atraso → confira as etapas no ANALISADOR DE LATÊNCIA para saber onde está o gargalo

---

*MOBILADOR — transporte, não mapeamento. O mapeamento pertence ao GG Mouse Pro 3.*
