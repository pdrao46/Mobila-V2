# Uso

Este documento descreve o uso diário: as telas, o modo jogo, os atalhos, os presets, os perfis, o benchmark e a leitura correta dos números.

---

## 1. O fluxo em uma linha

`celular → USB → MOBILADOR → imagem no PC → mouse e teclado de volta pelo USB → GG Mouse Pro 3 (no celular) → jogo`

O Mobilador não sabe o que é uma mira, um botão de atirar ou um item. Ele entrega um mouse e um teclado reais ao Android, do jeito mais rápido que consegue.

---

## 2. Painel (tela inicial)

O que cada elemento significa:

| Elemento | Significado |
|---|---|
| **● CELULAR CONECTADO** | O ADB vê o aparelho **e** o servidor no celular está respondendo |
| **Modelo / Android** | Vem do próprio aparelho (relatório de dispositivo), não de uma tabela fixa |
| **USB** | Estado real do canal: `CONECTADO`, `CONECTANDO`, `SEM DISPOSITIVO`, `ERRO` |
| **FPS origem / stream / exibição** | Veja a seção 6 deste documento |
| **Latência** | Total estimado, somando as etapas medidas (seção 6) |
| **GPU / CPU / RAM** | Medições locais do PC no instante atual |
| **START FREE FIRE** | Inicia a sessão: sobe o servidor no celular, encaminha as portas, conecta |
| **DESEMPENHO / LATÊNCIA / CONFIGURAÇÕES / DIAGNÓSTICO** | As quatro telas de trabalho |

Se o celular for desconectado, o app **não** congela mostrando a última imagem como se estivesse tudo bem: aparece o estado real e a mensagem do que aconteceu, e a reconexão é automática quando o aparelho volta.

---

## 3. Modo jogo

**Ligar:** `F12` ou o botão **GAME MODE** no topo.

Com o modo jogo ligado:

- fullscreen sem borda (a janela cobre o monitor inteiro, sem barra de título);
- cursor oculto;
- mouse capturado (movimento contínuo, sem parar nas bordas da tela);
- foco exclusivo na janela;
- a tela do celular preenche o monitor com adaptação inteligente de proporção — sem deformar, sem faixas pretas desnecessárias, sem perda de qualidade;
- **nenhuma** animação, nenhum menu, nenhum controle, nenhum painel por cima do jogo;
- overlay opcional (pequeno, no canto, ligado/desligado com `F8`).

**Desligar:** `F12` de novo. O cursor volta imediatamente e o mouse é liberado.

**Soltar o mouse sem sair do modo jogo:** `F9`. **Recapturar:** `F10`. Isso é útil para responder uma mensagem sem perder o fullscreen.

Regras que o app garante:

- o cursor nunca reaparece sozinho durante o jogo (o cursor é substituído por um cursor transparente de 1×1 e o Windows é instruído a não restaurá-lo);
- o mouse nunca escapa da janela enquanto está capturado;
- ao perder o foco (Alt+Tab, notificação em tela cheia), o mouse é solto; ao voltar o foco, é recapturado — sem "mouse preso";
- teclas de atalho do Mobilador **nunca** vão para o celular;
- nada de entrada duplicada: mouse vai por *raw input* e teclado pelo *message loop*, cada um exatamente uma vez.

---

## 4. Atalhos

Padrões (todos reconfiguráveis em CONFIGURAÇÕES → ATALHOS):

| Tecla | Ação |
|---|---|
| `F11` | Fullscreen |
| `F8` | Overlay dentro do jogo |
| `F9` | Soltar o mouse |
| `F10` | Capturar o mouse |
| `F12` | Modo jogo |
| `F5` | Iniciar / parar a sessão |
| `F6` | Captura de tela (BMP em `Documentos\Mobilador`) |
| `F7` | Log da sessão |

Para trocar: clique no campo do atalho (fica "aguardando"), pressione a tecla desejada. `Esc` cancela.

---

## 5. Presets

| Preset | Pensado para | O que muda |
|---|---|---|
| **ULTRA LOW LATENCY** | Competitivo | sem vsync, sem pacing, 1 frame em voo, captura na taxa do monitor, buffer 1 |
| **MAX FPS** | Monitor de 120/144 Hz | taxa mais alta sustentável, bitrate maior, sem descarte agressivo |
| **BALANCED** | Uso geral | 1080p60, ~12 Mbps |
| **QUALITY** | Legibilidade da imagem | mais bitrate, escala exata, sem perda de detalhe |
| **CUSTOM** | Ajuste fino | o que você mexeu fica como está |

Ao alterar qualquer opção manualmente, o preset vira **CUSTOM**. O aplicativo não finge que você continua em um preset depois de mexer nele.

---

## 6. Como ler os números

### FPS

| Número | Medido onde | Se estiver baixo… |
|---|---|---|
| **ORIGEM** | no celular, contando timestamps de captura distintos | o celular não está conseguindo capturar+codificar na taxa pedida → reduza a resolução ou a taxa |
| **STREAM** | no PC, contando pacotes de vídeo recebidos | o USB está saturado → reduza bitrate/resolução, ou use uma porta USB 3.0 |
| **EXIBIÇÃO** | no PC, contando `Present()` concluídos | a GPU ou o monitor é o limite → reduza a resolução de saída ou desligue o vsync |

O **MODO MAX FPS** olha os três e o menor entre monitor/celular/decodificador, escolhendo a maior taxa que o conjunto sustenta — e não anuncia nada acima disso.

### Latência

| Etapa | O que está sendo medido | Como melhorar |
|---|---|---|
| **CAPTURA** | do quadro capturado no celular até chegar ao PC | menos resolução, menos bitrate, USB 3.0 |
| **ENCODE** | tempo do encoder do celular | use um codec que o celular acelera (H.264 costuma ser o mais rápido) |
| **USB** | dos bytes prontos até chegarem ao PC | bitrate menor, cabo/porta melhor |
| **DECODE** | da entrega do pacote até a saída do quadro | aceleração de hardware ligada, decodificador por GPU ligado |
| **RENDER** | do frame retirado do mailbox até o `Present()` | vsync desligado, modo de renderização direto, resolução de saída |
| **ENTRADA** | ida e volta de um probe no canal de entrada | prioridade de entrada ligada, menos carga na CPU |
| **TOTAL** | soma das etapas acima | — |

Cada linha mostra **p50**, **p95** e **p99** e um gráfico ao longo do tempo. Olhe o **p99**: é ele que decide se aquele pico vai aparecer no meio de um confronto. O valor exato de cada etapa vem de medição; a frase correta nunca é "zero ms", e sim "o mais próximo possível de zero".

Exporte tudo em CSV (botão no ANALISADOR DE LATÊNCIA) para comparar duas configurações com números, não com impressão.

---

## 7. Benchmark

**BENCHMARK** executa uma escada de configurações, uma por vez:

| Degrau | Resolução | FPS | Bitrate | Codec |
|---|---|---|---|---|
| 1 | 1280×720 | 60 | 8 Mbps | H.264 |
| 2 | 1920×1080 | 60 | 12 Mbps | H.264 |
| 3 | 1920×1080 | 90 | 16 Mbps | H.265 |
| 4 | 1920×1080 | 120 | 20 Mbps | H.265 |
| 5 | 2560×1440 | 60 | 20 Mbps | H.265 |

Cada degrau mede por alguns segundos depois de estabilizar, e o resultado registra os números reais (FPS de exibição, latência total, frames perdidos). No fim, o relatório aponta a configuração mais rápida que se manteve estável. Resultado também em `Documentos\Mobilador\benchmark.csv`.

Interromper: botão CANCELAR. O app restaura a configuração que você tinha antes — o benchmark nunca deixa sua configuração alterada por acidente.

---

## 8. Perfis

Em **CONFIGURAÇÕES → PERFIS**:

- **SALVAR COMO…** cria um perfil com um nome (arquivo em `%LOCALAPPDATA%\Mobilador\profiles\NOME.ini`);
- a lista mostra os perfis encontrados; clicar em um carrega;
- perfis são arquivos INI comuns: você pode editá-los à mão, copiar para outro PC, versionar.

---

## 9. Capturas e relatórios

| Ação | Resultado |
|---|---|
| `F6` | `Documentos\Mobilador\captura-AAAAMMDD-HHMMSS.bmp` (imagem exata do que está na tela, com o overlay se estiver visível) |
| DIAGNÓSTICO → RELATÓRIO | `Documentos\Mobilador\diagnostico.txt` |
| LATÊNCIA → EXPORTAR | `Documentos\Mobilador\latencia.csv` |
| BENCHMARK | `Documentos\Mobilador\benchmark.csv` |
| Log de desempenho (se ligado) | `%LOCALAPPDATA%\Mobilador\logs\sessao.txt` |

---

## 10. Perguntas rápidas

**Por que o mouse não mexe o cursor no Windows no modo jogo?** Porque ele está capturado: o movimento vai todo para o celular, que é o objetivo. `F9` devolve o cursor.

**Por que não existe tela de teclas/HUD?** Porque o mapeamento pertence ao GG Mouse Pro 3, no celular. O Mobilador entrega o mouse e o teclado; a tradução para toques é do GG Mouse Pro 3.

**Posso usar Wi-Fi?** Não. O transporte é USB, por decisão de projeto: é o caminho deterministicamente mais rápido e não depende de rede.

**O que faço se a imagem travar?** O app tenta reconectar sozinho; se o celular tirou a autorização de depuração ou o servidor morreu, o DIAGNÓSTICO diz qual dos dois foi. Reiniciar a sessão com `F5` é sempre seguro (nada fica pendurado no celular).
