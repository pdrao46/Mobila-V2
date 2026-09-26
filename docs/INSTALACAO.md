# Instalação

Guia completo, na ordem em que as coisas precisam acontecer. Se algo não funcionar, o item correspondente aparece no **DIAGNÓSTICO** do próprio aplicativo — e o que fazer está em [SOLUCAO_DE_PROBLEMAS.md](SOLUCAO_DE_PROBLEMAS.md).

---

## 1. PC: Windows

### 1.1 Requisitos

| Item | Mínimo | Recomendado |
|---|---|---|
| Windows | 10 64 bits (1903+) | Windows 11 64 bits |
| GPU | Direct3D 11 com decodificação H.264 por hardware | GPU dedicada (NVIDIA/AMD/Intel Arc) |
| USB | porta USB 2.0 | porta USB 3.0 (mais banda, mais estabilidade) |
| CPU | 4 núcleos | 6 núcleos ou mais |
| RAM | 8 GB | 16 GB |

### 1.2 Obter o executável

**Opção A — baixar pronto.** Copie `Mobilador.exe` para uma pasta sua (por exemplo `C:\Mobilador\`). Ele é um único arquivo, não precisa de instalador nem de runtime adicional: nada de .NET, nada de Visual C++ Redistributable.

**Opção B — compilar.**

```bash
python3 tools/build.py                # usa zig se estiver no PATH
python3 tools/build.py --debug        # versão com console e símbolos, para investigar problemas
```

O resultado fica em `dist/Mobilador.exe`. O build também embute o ícone e as informações de versão do Windows (arquivo `assets/mobilador.rc`).

### 1.3 ADB

O Mobilador fala com o celular pelo **Android Debug Bridge**. Ele procura o `adb.exe` nesta ordem:

1. `adb.exe` na **mesma pasta** do `Mobilador.exe`;
2. a variável de ambiente `ADB`;
3. `adb` no `PATH`;
4. `%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe`;
5. `%USERPROFILE%\AppData\Local\Android\Sdk\platform-tools\adb.exe`.

Se você não tem ADB instalado, a forma mais simples é baixar o "SDK Platform-Tools for Windows" do site oficial do Android e copiar `adb.exe`, `AdbWinApi.dll` e `AdbWinUsbApi.dll` para a pasta do Mobilador. O diagnóstico mostra o caminho exato que ele encontrou (ou que não encontrou).

> **Dica:** se você usa o ADB junto com outro programa (scrcpy, emuladores, IDE), feche-o antes. Dois servidores ADB disputando a mesma porta é a causa mais comum de "o celular aparece e desaparece".

---

## 2. Celular: preparar o Android

1. **Opções do desenvolvedor.** Configurações → Sobre o telefone → toque 7 vezes em "Número da versão". Em alguns aparelhos o caminho é Configurações → Sistema → Sobre o telefone.
2. **Depuração USB.** Opções do desenvolvedor → ative *Depuração USB*.
3. **Cabo de dados.** Conecte em uma porta USB do PC, **direto** na máquina (evite hubs e extensores) e use um cabo que transmita dados — cabos de "somente carga" não funcionam e são uma causa clássica de falha silenciosa.
4. **Autorize o PC.** Na tela do celular aparece "Permitir depuração USB?" com a impressão digital do computador. Marque "Sempre permitir" e toque OK. **Sem esse toque, nada funciona** — o celular fica visível mas `unauthorized`.
5. **Evite a suspensão.** Opções do desenvolvedor → *Manter tela ligada* (com o cabo conectado). Desative o "modo de economia de energia" / "otimização de bateria" para o app que você vai usar e para o GG Mouse Pro 3.
6. **Modo USB.** Se aparecer uma notificação de "uso do USB", escolha **Transferência de arquivos (MTP)** ou "Sem transferência de dados" — não escolha "Carregar apenas", porque em alguns aparelhos isso desliga o canal ADB.

---

## 3. Módulo do servidor no celular (`mobilador.dex`)

O executável do PC é só metade do programa: no celular roda um módulo pequeno, em Java, que captura a tela, codifica o vídeo, injeta mouse e teclado e envia o áudio. Ele **não** é instalado como aplicativo — é um arquivo `.dex` executado pelo próprio Android com permissão de depuração.

### 3.1 Caminho A — módulo pronto (jeito mais simples)

1. Obtenha `mobilador.dex` (do release do projeto ou gerado pelo CI — veja o item 3.3).
2. Coloque em `server\mobilador.dex`, ao lado do `Mobilador.exe`:

   ```
   C:\Mobilador\Mobilador.exe
   C:\Mobilador\server\mobilador.dex
   ```

3. Abra o Mobilador com o celular conectado. Na primeira sessão, ele envia o módulo automaticamente para `/data/local/tmp/mobilador.dex`. O diagnóstico mostra "Módulo enviado".

### 3.2 Caminho B — compilar no próprio PC (precisa de um JDK)

Se você tem o JDK instalado (`javac -version` funciona), dá para gerar o módulo direto do código:

```bash
cd android-server
./build.sh            # Linux/macOS
build.bat             # Windows (Prompt de Comando)
```

O script:

1. procura o `javac`;
2. procura um `android.jar` (SDK instalado **ou** o arquivo de stubs `tools/android-stubs/android-33.jar` que acompanha o projeto — ou seja, **não é preciso ter o Android SDK**);
3. compila as sete classes do módulo;
4. transforma em bytecode Dalvik com o `d8` (o `tools/d8.jar` que vem no repositório funciona com o `java` do JDK);
5. grava `dist/mobilador.dex`.

Depois copie para `server\mobilador.dex` ao lado do executável, como no item 3.1.

Você também pode deixar o próprio aplicativo fazer isso: **DIAGNÓSTICO → INSTALAR / ATUALIZAR MÓDULO**. Ele procura o `.dex` pronto, e se não existir tenta compilar a partir dos fontes Java.

### 3.3 Caminho C — gerar no GitHub Actions

O repositório inclui um workflow pronto em `tools/ci-build.yml`. Ele compila o `.dex` (e o executável do Windows) em máquinas do GitHub, sem precisar de JDK na sua. Para ativar:

1. copie `tools/ci-build.yml` para `.github/workflows/build.yml`;
2. faça commit;
3. baixe o artefato `mobilador-dex` na aba **Actions** e coloque em `server\mobilador.dex`.

> O arquivo ficou fora de `.github/workflows/` porque publicar um workflow exige um token com a permissão `workflows` — mantê-lo em `tools/` deixa a instalação sob seu controle.

### 3.4 Modo de compatibilidade (sem módulo)

Sem o módulo, o Mobilador **não** inventa uma conexão falsa: a sessão não inicia. Isso é intencional — a alternativa (usar `screenrecord` + `input` por linha de comando) teria latência alta, limite de tempo e seria desonesto chamar de "baixa latência". Para essa via, use o scrcpy; o Mobilador existe para entregar o caminho rápido.

---

## 4. Primeira execução

1. Abra `Mobilador.exe`.
2. O painel deve mostrar **● CELULAR CONECTADO** e o modelo do aparelho. Se mostrar "SEM CELULAR", veja o diagnóstico.
3. Abra **DIAGNÓSTICO** e confirme:
   - **USB** → OK (dispositivo autorizado, status `device`)
   - **ADB** → OK (com o caminho do executável)
   - **SERVIDOR NO CELULAR** → OK (módulo presente em `/data/local/tmp/mobilador.dex`)
4. Em **CONFIGURAÇÕES → PERFIS**, escolha o preset. Sugestão: `ULTRA LOW LATENCY`.
5. Clique **START FREE FIRE** (ou `F5`). O servidor do celular inicia, as três portas são encaminhadas (`adb forward`) e a imagem aparece.
6. `F12` para o modo jogo. Pronto para jogar.

### Verificação manual (se quiser conferir por fora)

```bash
adb devices -l                      # deve listar seu aparelho como "device"
adb shell ls -l /data/local/tmp/mobilador.dex
adb forward --list                  # deve mostrar tcp:27183, 27184, 27185
```

Se quiser iniciar o servidor à mão para ver o log dele:

```bash
adb shell "CLASSPATH=/data/local/tmp/mobilador.dex app_process /system/bin com.mobilador.server.Main \
  --port 27183 --input-port 27184 --audio-port 27185 \
  --width 1920 --height 1080 --fps 60 --bitrate 12000 --codec video/avc --no-audio"
```

E, em outro terminal, acompanhe as portas:

```bash
adb forward tcp:27183 tcp:27183
```

---

## 5. Configuração inicial recomendada

| Situação | Preset | Ajustes |
|---|---|---|
| PC bom, monitor 60 Hz | ULTRA LOW LATENCY | — |
| Monitor 120/144 Hz | MAX FPS | confira o FPS de exibição no painel |
| Notebook com vídeo integrado | BALANCED | resolução 1600×900, FPS 60 |
| Celular com tela 2K | ULTRA LOW LATENCY | resolução 1920×1080 (mais pixels que isso não traz vantagem) |
| Wi-Fi ruim (não use Wi-Fi) | — | conecte o cabo |

Depois do primeiro uso, salve um perfil em **CONFIGURAÇÕES → PERFIS** para não precisar mexer de novo.

---

## 6. Desinstalar

- Apague a pasta do programa.
- Apague `%LOCALAPPDATA%\Mobilador`.
- No celular: `adb shell rm /data/local/tmp/mobilador.dex`.
- Se quiser remover os encaminhamentos de porta: `adb forward --remove-all`.

Nada é instalado no registro do Windows, nada fica em `Program Files`, nada é instalado no celular como aplicativo.
