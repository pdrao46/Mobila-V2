# Referência de configuração

Todas as opções do Mobilador, uma por uma: o que fazem, por que o padrão é aquele e o que muda no comportamento. O arquivo fica em `%LOCALAPPDATA%\Mobilador\settings.ini` e pode ser editado à mão — o aplicativo relê o arquivo na inicialização e salva sozinho quando algo muda na interface.

> A mesma explicação aparece **dentro do aplicativo**, na tela DESEMPENHO, abaixo de cada opção. Esse é um requisito do projeto: nenhuma opção avançada existe sem uma frase dizendo o que ela faz em linguagem simples.

---

## [capture] — captura e codificação (lado do celular)

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `resolution_width` | `1920` | Largura capturada. Mais pixels significam mais bitrate, mais tempo de codificação e mais dados no cabo. 1920×1080 é o melhor equilíbrio na maioria dos aparelhos: acima disso o encoder do celular começa a atrasar o pipeline. |
| `resolution_height` | `1080` | Altura capturada. Use a resolução nativa do aparelho somente se o monitor justificar. |
| `target_fps` | `60` | Taxa alvo da captura no celular. `0` = **MAX FPS**: usa a maior taxa que o conjunto (painel do celular, monitor e decodificador) consegue sustentar. Nunca promete mais do que mede. |
| `bitrate_kbps` | `12000` | Quanto o encoder pode gastar. Muito baixo → blocos e borrões nos movimentos. Muito alto → o USB satura e o FPS do stream cai. 12 Mbps em 1080p60 é o ponto em que a imagem fica limpa sem sufocar o cabo. |
| `codec` | `h264` | `h264` codifica mais rápido no celular; `h265` ocupa ~30% menos banda, mas nem todo decodificador é rápido nele. Em latência pura, H.264 costuma ganhar. |
| `quality` | `1` (equilibrada) | Ajuste interno do encoder (perfil e limites). `0` = desempenho, `2` = alta, `3` = máxima. Perfis mais altos gastam mais tempo de codificação. |
| `color_format` | `0` (YUV 4:2:0) | Formato nativo dos encoders de tela. Converter para 4:4:4 ou RGB exigiria uma passada extra na CPU do celular e não melhora o que você vê. |
| `keyframe_interval` | `2` | Segundos entre quadros-chave. Intervalos maiores economizam banda, mas fazem a recuperação após uma perda de pacote demorar mais. 2 s é o equilíbrio. |
| `capture_mode` | `0` | `0` = VirtualDisplay (mais rápido, o caminho normal), `1` = MediaProjection (alternativa quando o aparelho bloqueia o primeiro). |

---

## [transport] — transporte USB

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `usb_mode` | `0` (automático) | Escolhe o caminho de maior banda disponível no cabo. Modos fixos existem para diagnóstico. |
| `socket_recv_buffer_kb` | `2048` | Tamanho do buffer de recepção. 2 MB absorve as rajadas de quadros-chave sem que o sistema descarte pacotes; valores muito altos apenas atrasam a detecção de falha. |
| `reconnect_auto` | `true` | Reconecta sozinho quando o cabo é mexido, quando o servidor do celular reinicia ou quando o Android suspende a depuração. Desligar só faz sentido em diagnóstico. |
| `reconnect_delay_ms` | `700` | Espera entre tentativas. Tempo curto demais gera tempestade de tentativas; tempo longo demais deixa o usuário esperando. |
| `usb_priority_process` | `true` | Sobe a prioridade do processo enquanto a sessão está ativa, para o transporte não competir com antivírus e indexadores. |

---

## [decode] — decodificação e apresentação (lado do PC)

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `hardware_acceleration` | `true` | Decodifica na GPU. Desligado, o decode cai para a CPU e a latência de uma etapa inteira aparece. |
| `gpu_decoder` | `true` | Prefere os decodificadores de hardware da Media Foundation. Ligado, o quadro chega como textura D3D11 e é desenhado direto — sem cópia pela memória principal. |
| `vsync` | `false` | Sincroniza a apresentação com o monitor. É mais estável visualmente, mas pode adicionar até um quadro inteiro de atraso. Em jogo competitivo, desligado. |
| `frame_pacing` | `false` | Limita a apresentação à taxa alvo. Ajuda em notebook (menos consumo, menos calor) e não aumenta latência além de meio quadro. |
| `low_latency_render` | `true` | Um único frame em voo, com objeto de espera (nada de espera ocupada). É o que evita a fila de 2–3 quadros que faz o mouse parecer "flutuando". |
| `scaling_mode` | `0` (proporcional) | `0` mantém a proporção e centraliza; `1` preenche (pode deformar); `3` escala em números inteiros (pixel perfeito); `4` 1:1 sem ampliar (nitidez máxima, imagem menor em telas grandes). |
| `render_mode` | `0` (direto) | `0` = uma passada, sem pós-processamento. Modos com filtro melhoram a suavidade e custam tempo de GPU. |
| `allow_frame_dropping` | `true` | Permite descartar quadros antigos em vez de enfileirá-los. Ligado, a imagem pode "pular" um quadro em uma engasgada — mas nunca chega atrasada. |
| `latest_frame_priority` | `true` | Entrega sempre o quadro mais novo (mailbox de um slot). Desligado, vira fila: pior latência, melhor fluidez em rede instável. Como aqui o transporte é um cabo, o mais novo ganha. |
| `buffer_size` | `1` | Frames guardados antes de apresentar. `1` é o mínimo fisicamente possível. |
| `jitter_buffer_ms` | `0` | Reserva de tempo para absorver variação (jitter). `0` = desligado. Em USB, jitter é praticamente zero; ligar isso só adiciona atraso. |
| `smooth_video` | `false` | Filtragem extra do croma. Mais suave nas bordas coloridas, um toque a mais por pixel na GPU. |

---

## [input] — entrada

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `mouse_capture` | `true` | Mantém o mouse preso à janela. Sem isso, o ponteiro para nas bordas e o movimento fica limitado — é a diferença entre "mouse de jogo" e "mouse de janela". |
| `mouse_raw_input` | `true` | Lê o mouse pelo *raw input* do Windows: sem aceleração do cursor, sem suavização, sem interpolação. É a única forma de obter o movimento exato do sensor. |
| `input_priority` | `true` | Coloca a thread de entrada em prioridade de tempo crítico. Um pacote de mouse atrasado é um tiro perdido. |
| `mouse_sensitivity` | `1.00` | Multiplicador aplicado ao movimento antes de enviar. **Não é mapeamento**: é só ajuste de escala. O ajuste fino (quanto movimento equivale a quanto de mira) pertence ao GG Mouse Pro 3. |
| `input_batch_us` | `0` | `0` = cada evento é enviado no instante em que é lido. Qualquer valor acima disso agrupa eventos para economizar banda — e adiciona exatamente esse tempo de atraso. |
| `keyboard_passthrough` | `true` | Envia o teclado ao celular. Desligado, o Mobilador só usa o teclado para atalhos (útil quando o celular tem teclado próprio). |

---

## [experience] — janela e experiência

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `auto_fullscreen` | `true` | Entra em fullscreen ao iniciar a sessão. Menos cliques, menos altitude de janela para o Windows compor. |
| `auto_hide_cursor` | `true` | Esconde o cursor em modo jogo. Um cursor sobre a imagem de um jogo é ruído visual e uma distração. |
| `game_mode` | `false` | Estado inicial do modo jogo. Normalmente você liga com `F12`; deixar `true` serve para quem usa o PC só para isso. |
| `overlay_enabled` | `true` | Permite o overlay dentro do jogo (ligado/desligado com `F8`). |
| `show_stats_overlay` | `true` | Mostra FPS e latência no overlay. Sem isso, o overlay fica só com o resumo de conexão. |
| `overlay_scale` | `1.00` | Tamanho do overlay. |
| `topmost` | `false` | Mantém a janela sempre na frente. Útil com um segundo monitor; em modo jogo é dispensável (o fullscreen sem borda já fica na frente). |
| `keep_screen_awake` | `true` | Mantém a tela do celular ligada durante a sessão (`svc power stayon usb`). Sem isso, o Android apaga a tela no meio da partida. |

---

## [audio] — áudio (opcional)

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `audio_enabled` | `false` | Transporta o áudio do celular para o PC. Desligado por padrão porque adiciona banda no mesmo cabo — em latência pura, o vídeo tem prioridade. |
| `audio_bitrate_kbps` | `128` | Qualidade do áudio. |
| `audio_volume` | `0.70` | Volume de reprodução no PC. |

---

## [appearance] — aparência

| Chave | Padrão | O que faz / por que esse padrão |
|---|---|---|
| `theme` | `dark` | `dark`, `light` ou `amoled` (preto puro). |
| `accent_rgb` | `4C8DFF` | Cor de acento em hexadecimal. Ela governa botões, bordas, indicadores, sliders, ícones, estados ativos, gráficos e menus — em todas as telas, sem exceção. |
| `accent_name` | `Blue` | Nome do acento (Azul, Roxo, Vermelho, Verde, Laranja, Ciano, Branco ou Personalizado). |
| `animations` | `true` | Micro-transições de 120–180 ms nas telas de trabalho. **Em modo jogo, nada anima, independentemente desta opção.** |
| `ui_scale` | `1.00` | Escala da interface. Valores maiores ajudam em telas 4K; a escala de DPI do Windows é respeitada automaticamente por cima disso. |

---

## [hotkeys] — atalhos

Valores em hexadecimal, no código de tecla virtual do Windows (`VK_*`).

| Chave | Padrão | Tecla |
|---|---|---|
| `toggle_fullscreen` | `0x7A` | F11 |
| `toggle_overlay` | `0x77` | F8 |
| `release_mouse` | `0x78` | F9 |
| `capture_mouse` | `0x79` | F10 |
| `toggle_game_mode` | `0x7B` | F12 |
| `start_stop_stream` | `0x74` | F5 |
| `screenshot` | `0x75` | F6 |
| `toggle_stats` | `0x76` | F7 |

Todos são consumidos pelo aplicativo e **nunca** encaminhados ao celular.

---

## [general]

| Chave | Padrão | O que faz |
|---|---|---|
| `preset` | `2` (BALANCED) | Último preset aplicado. Qualquer alteração manual muda para `4` (CUSTOM). |
| `last_profile` | `Balanced` | Último perfil salvo/carregado. |
| `log_performance` | `false` | Grava o log de desempenho em `%LOCALAPPDATA%\Mobilador\logs\sessao.txt`. |
| `close_to_tray` | `false` | Fechar minimiza para a bandeja em vez de encerrar. |
| `check_updates` | `false` | Verificação de atualização (desligada por padrão: nada do Mobilador acessa a internet por conta própria). |

---

## Tabela rápida: quer menos latência?

Para **cada** ajuste possível, na ordem de impacto:

1. `vsync = false` e `frame_pacing = false`
2. `low_latency_render = true`, `latest_frame_priority = true`, `buffer_size = 1`, `jitter_buffer_ms = 0`
3. `hardware_acceleration = true` e `gpu_decoder = true`
4. `input_batch_us = 0` e `input_priority = true`
5. resolva o gargalo real olhando o ANALISADOR DE LATÊNCIA: se **CAPTURA** e **ENCODE** dominam, reduza resolução/taxa no `[capture]`; se **USB** domina, reduza bitrate; se **RENDER** domina, desligue o vsync e reduza a resolução de saída.

O que **não** ajuda: aumentar bitrate além do que o cabo aguenta, subir FPS acima da taxa que o celular sustenta, ou ligar buffer "para ficar mais suave". Tudo isso aumenta atraso.
