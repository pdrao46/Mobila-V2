# Solução de problemas

O primeiro passo é sempre **DIAGNÓSTICO**, dentro do aplicativo. Ele mede doze itens e diz, para cada um, o que foi encontrado e o que fazer. Este documento explica as situações por trás de cada item, mais os problemas que não aparecem no diagnóstico.

---

## 1. Ler o DIAGNÓSTICO

Cada item tem três estados:

- **OK** — medido e dentro do esperado.
- **ATENÇÃO** — funciona, mas há um limite conhecido (por exemplo, decodificação por software, ou FPS alvo acima da taxa do monitor).
- **ERRO** — não é possível operar como esperado.

O relatório completo (botão **RELATÓRIO**) é salvo em `Documentos\Mobilador\diagnostico.txt` e inclui os valores medidos no momento.

---

## 2. Por item

### USB — ERRO: nenhum dispositivo

1. `adb devices -l` no Prompt de Comando. Se a lista estiver vazia, o problema é do sistema, não do Mobilador:
   - use um cabo de **dados** (cabos de carga não funcionam; teste com outro cabo);
   - conecte direto no PC, sem hub;
   - troque de porta (prefira USB 3.0 azul);
   - no celular, escolha "Transferência de arquivos (MTP)" quando perguntado sobre o uso do USB.
2. Verifique se o aparelho aparece como `unauthorized`: nesse caso, toque em **Permitir depuração USB** na tela do celular. Se a caixa não aparece, desconecte, ative/desative a depuração USB e reconecte.
3. Feche outros programas que usam ADB (scrcpy, emuladores, Android Studio). Dois servidores ADB disputando a porta 5037 é a causa mais comum de instabilidade.

### USB — ERRO: dispositivo `offline` ou aparece e desaparece

- Quase sempre é energia: as Opções do desenvolvedor têm "Manter tela ligada"; desligue também a economia de energia do USB nas configurações de energia do Windows (Modo de economia de energia USB → Desativado).
- Fonte de alimentação fraca ou hub sem energia causam exatamente esse padrão.
- Reinicie o servidor ADB: `adb kill-server && adb start-server`.

### ADB — ERRO: não encontrado

O Mobilador procura `adb.exe` em: pasta do executável → variável `ADB` → `PATH` → `%LOCALAPPDATA%\Android\Sdk\platform-tools` → `%USERPROFILE%\AppData\Local\Android\Sdk\platform-tools`. Copie `adb.exe`, `AdbWinApi.dll` e `AdbWinUsbApi.dll` para a pasta do `Mobilador.exe` — é o caminho mais simples.

### SERVIDOR NO CELULAR — ATENÇÃO: módulo ausente

- Coloque `mobilador.dex` em `server\mobilador.dex` ao lado do executável e abra o aplicativo novamente; ele envia sozinho.
- Ou compile: `android-server\build.bat` (precisa de JDK) e copie o `dist\mobilador.dex` gerado.
- Ou baixe o artefato gerado pelo CI do projeto (arquivo `tools/ci-build.yml`).
- Se o módulo está no PC mas o envio falha, verifique espaço em `/data/local/tmp` no celular e se o Windows tem permissão de escrita na pasta do executável.

### GPU — ATENÇÃO: GPU integrada / ERRO: adaptador de software

- Uma GPU integrada decodifica 1080p60 por hardware sem problema; a ATENÇÃO indica que taxas acima de ~90 Hz podem não se sustentar. Use `BALANCED`.
- **ERRO de software** significa que o Direct3D caiu para o adaptador WARP (sem GPU). Nesse caso a decodificação é feita na CPU: atualize o driver da GPU; se o problema persistir, a latência será alta por limitação da máquina — o diagnóstico está dizendo isso claramente.

### CPU — ATENÇÃO acima de 88%

- Feche o que não estiver em uso. Se a CPU ficar alta **durante** a sessão, confirme que `hardware_acceleration` e `gpu_decoder` estão ligados (sem eles, o decode vai para a CPU).
- Antivírus escaneando a saída do `adb` é um caso comum: exclua a pasta do Mobilador da verificação em tempo real.

### RAM — ATENÇÃO acima de 92%

- O Mobilador usa pouca memória (fileiras de amostras fixas). Se a RAM do sistema estiver cheia, a latência sobe por causa do *page file*.

### DECODIFICADOR — ATENÇÃO: sem decodificador de hardware

- Confirme o driver da GPU. Se o aparelho/máquina realmente não tem H.264 por hardware, use `codec = h264` (mais rápido por software que H.265) e reduza a resolução para 1600×900.

### RENDERIZADOR — OK com aviso de vsync

- Se o FPS de exibição ficou preso na taxa do monitor e você quer mais, desligue `vsync` e `frame_pacing`.

### DISPLAY — ATENÇÃO: FPS alvo acima da taxa do monitor

- O diagnóstico compara o alvo com a taxa real do monitor. Pedir 144 Hz em um monitor de 60 Hz só aquece a máquina: o modo **MAX FPS** faria isso por você.

### FPS — ATENÇÃO: exibição abaixo do stream

- O gargalo é a apresentação (GPU, monitor ou vsync), não o USB. Desligue o vsync, reduza a resolução de saída ou use escala proporcional em vez de 1:1.

### ENTRADA — ATENÇÃO: latência de entrada alta

- Confirme `input_priority = true` e `input_batch_us = 0`.
- Se o celular usa o backend **InputManager** em vez do **uinput**, a latência é maior por natureza; alguns aparelhos bloqueiam `/dev/uinput`. Nada de errado com o PC nesse caso.

### MOUSE E TECLADO — ATENÇÃO: mouse não capturado

- O modo jogo está desligado, ou o mouse foi solto com `F9`. `F10` recaptura, `F12` alterna o modo jogo.

---

## 3. Problemas que não aparecem no diagnóstico

### A imagem aparece, mas o mouse não mexe nada no jogo

O mapeamento é do **GG Mouse Pro 3**, no celular. Confirme:

1. o GG Mouse Pro 3 está ativo e com o perfil do jogo carregado;
2. o backend de entrada do Mobilador está ativo (DIAGNÓSTICO → ENTRADA mostra o backend e a taxa de eventos);
3. o modo jogo está ligado (`F12`) e o mouse capturado (`F10`);
4. dentro do jogo, o cursor do Android está visível? Muitos jogos só processam movimento quando o ponteiro está ativo — o GG Mouse Pro 3 é quem cuida disso.

### O cursor reaparece no meio do jogo

- Algum aplicativo em primeiro plano (notificação do Windows, um overlay de gravador, o menu do Xbox Game Bar) roubou o foco. Desligue notificações e o Game Bar durante a partida.
- `Win+G` (Game Bar) e `Alt+Tab` soltam o cursor por decisão do sistema; o Mobilador recaptura ao voltar o foco.

### O mouse "escapa" ou o movimento fica com degraus

- Confirme `mouse_raw_input = true` (sem aceleração do Windows).
- Desligue "Melhorar a precisão do ponteiro" também nas configurações do Windows, se você usa o cursor no desktop.
- Ajuste `mouse_sensitivity` se a sensibilidade ficou estranha — mas o ajuste fino pertence ao GG Mouse Pro 3.

### Tecla presa (jogo continua andando depois de soltar)

- Sair do modo jogo com `F12` envia os eventos de soltura; se o foco foi perdido no meio de uma tecla pressionada, o Mobilador libera as teclas ao perder o foco.
- Se acontecer mesmo assim: `F12` duas vezes (liga/desliga) resolve; como último recurso, reinicie a sessão com `F5`.

### Imagem com atraso, mas os FPS estão bons

Abra o **ANALISADOR DE LATÊNCIA** e veja qual etapa está dominando:

| Etapa dominante | O que fazer |
|---|---|
| CAPTURA + ENCODE | reduzir resolução, reduzir FPS alvo, conferir se o celular está em modo de economia de energia |
| USB | reduzir bitrate, trocar para porta USB 3.0, trocar o cabo |
| DECODE | ligar decodificação por hardware, usar H.264 |
| RENDER | desligar vsync e frame pacing, reduzir a resolução de saída |
| ENTRADA | ligar prioridade de entrada, reduzir carga da CPU |

### A sessão cai sozinha a cada poucos minutos

- Android suspendendo a depuração: mantenha a tela ligada e desative a otimização de bateria.
- Cabo/hub instável: é o sintoma clássico.
- Otimizador de "jogos" do fabricante matando processos: adicione o GG Mouse Pro 3 e o app do jogo à lista de exceções.

### Áudio não sai no PC

- `audio_enabled` está **desligado** por padrão (o padrão prioriza latência). Ligue em CONFIGURAÇÕES → ÁUDIO e reinicie a sessão com `F5`.
- Em alguns aparelhos a captura de áudio do sistema exige Android 10+ e não pode ser combinada com certos aplicativos.

### O aplicativo abre mas a janela fica preta

- Se você iniciou a sessão sem o celular conectado, é o comportamento correto: a tela diz o motivo. Conecte o aparelho e use `F5`.
- Se o Windows acabou de trocar a GPU/driver, feche e abra novamente: o Mobilador recria o dispositivo D3D11 ao detectar a perda, mas uma reinicialização limpa é mais rápida.

### Antivírus reclamando

O executável é compilado do código-fonte e não empacota nada. Suspeitas acontecem porque ele executa `adb.exe` e fala com um dispositivo USB. Assine o executável com um certificado próprio (você tem os fontes) ou adicione a pasta à lista de exceções.

---

## 4. Se precisar de ajuda, colete isto

1. `Documentos\Mobilador\diagnostico.txt`
2. `%LOCALAPPDATA%\Mobilador\logs\sessao.txt` (se o log estiver ligado)
3. `Documentos\Mobilador\latencia.csv`
4. A saída de `adb devices -l`
5. O modelo do celular, a versão do Android e a versão do driver da GPU

Com isso, o gargalo aparece em minutos: cada número vem de medição, e cada medição diz de onde vem.
