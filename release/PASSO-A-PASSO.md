# MOBILADOR 1.0.0 — PASSO A PASSO DA INSTALAÇÃO

Guia do zero, na ordem. O que está em **negrito** é clique ou tecla.

---

## ANTES DE COMEÇAR — o que você precisa

- Um PC com **Windows 10 ou 11, 64 bits**.
- Acesso ao ficheiro `Mobilador-Setup-1.0.2.exe` (41,9 MB).
- **Não** precisa de: administrador, Java, Visual C++ Redistributable, Android SDK,
  iTunes, ou qualquer driver extra.

---

## ETAPA 1 — Baixar o instalador para o PC

1. No painel do Arena, abra o ficheiro `release/Mobilador-Setup-1.0.2.exe`
   (é o arquivo que está no visualizador de ficheiros).
2. Clique em **Download** / **Baixar**.
3. O Windows vai guardar em `C:\Users\<seu_nome>\Downloads`.
   - Se aparecer aviso do navegador ("este tipo de ficheiro pode ser perigoso"),
     escolha **Manter** (Chrome/Edge: **Manter mesmo assim**).

> Se você copiar por pen drive em vez de baixar, pule para a Etapa 2 do mesmo jeito.

---

## ETAPA 2 — Conferir se o ficheiro chegou inteiro (opcional, 30 segundos)

1. Abra o **Menu Iniciar** e digite `cmd` → **Prompt de Comando**.
2. Digite (troque o nome da pasta se não for Downloads):

   ```
   certutil -hashfile "%USERPROFILE%\Downloads\Mobilador-Setup-1.0.2.exe" SHA256
   ```

3. Compare a linha de números com esta:

   ```
   1be80d74adff9f870c42ed6faa4e6540657a384bd210139f632de8473a67cec4
   ```

   Tem de ser **igual, carácter por carácter**. Se for diferente, o download
   corrompeu: baixe de novo.

---

## ETAPA 3 — Executar o instalador

1. Vá até `Downloads` e dê **duplo clique** em `Mobilador-Setup-1.0.2.exe`.
2. O Windows vai mostrar a tela azul **"O Windows protegeu o seu PC"**
   (SmartScreen). Isso acontece porque o programa não tem assinatura digital
   paga — é normal em programas novos.
   - Clique em **Mais informações** (texto pequeno, embaixo do aviso).
   - Depois clique em **Executar mesmo assim**.
3. Se em vez disso aparecer o **Controle de Conta de Usuário**, clique em **Sim**
   (só aparece em algumas configurações; o programa **não** precisa de admin).

> **O antivírus bloqueou / apagou o ficheiro?** Alguns antivírus são agressivos
> com `.exe` sem assinatura. Adicione uma exceção para esse ficheiro e execute
> de novo. Não há nada escondido: o código está todo no repositório, em
> `tools/installer/`.

---

## ETAPA 4 — A janela do instalador

Abre uma janela escura do **Mobilador Setup**. Nela:

1. Confira a **pasta de destino** — por padrão:
   ```
   C:\Users\<seu_nome>\AppData\Local\Programs\Mobilador
   ```
   Quer instalar em outro lugar? Clique em **Procurar...** e escolha a pasta.
   (Na 1.0.1, se a pasta padrão não puder ser criada, o instalador tenta
   automaticamente `%LOCALAPPDATA%\Mobilador` e depois `%USERPROFILE%\Mobilador`,
   e diz no log onde instalou.)
2. Escolha os atalhos que quiser:
   - ☑ **Atalho no Menu Iniciar** (recomendado)
   - ☑ **Atalho na Área de Trabalho** (opcional)
3. Clique em **INSTALAR**.
4. Acompanhe a barra de progresso e a lista de ficheiros sendo extraídos
   (são 23: Mobilador.exe, adb.exe, DLLs de USB, documentação, licenças e o
   módulo Android). Leva poucos segundos.
5. No fim aparece **"Instalação concluída"**. Você pode:
   - ☑ marcar **Abrir o Mobilador agora** e clicar em **CONCLUIR**, ou
   - só clicar em **CONCLUIR** para sair.

**Pronto — está instalado.** Ele já se registrou em *Configurações → Aplicativos*.

---

## ETAPA 5 — Abrir o Mobilador

- **Área de Trabalho**: ícone **Mobilador**, duplo clique.
- **Menu Iniciar**: digite `Mobilador` → Enter.
- Ou direto pelo caminho:
  `%LOCALAPPDATA%\Programs\Mobilador\Mobilador.exe`

---

## ETAPA 6 — Preparar o telemóvel (Android)

O Mobilador controla o telefone; para isso o telefone precisa autorizar o PC.
Isso é feito **uma vez**:

1. No telefone, abra **Configurações → Sobre o telefone**.
2. Toque **7 vezes** em **Número da versão** (ou "Número de compilação").
   Aparece "Você agora é um desenvolvedor".
3. Volte e abra **Configurações → Opções do desenvolvedor**
   (em Samsung/Xiaomi costuma ficar no fim da lista).
4. Ligue **Depuração USB**.
5. Ligue o telefone ao PC com o **cabo USB** (cabo de dados, não cabo
   só-de-carregamento).
6. No telefone aparece **"Permitir depuração USB?"** → marque **Sempre permitir**
   → **OK**. (Se não aparecer, troque o modo USB para "Transferência de arquivos".)
7. Abra o Mobilador no PC: o telefone deve aparecer na lista. Clique nele e
   comece a espelhar.

> Telefone não aparece? Veja `docs\SOLUCAO_DE_PROBLEMAS.md` (instalado junto com
> o programa) — normalmente é driver USB do fabricante ou depuração desligada.

---

## COMO DESINSTALAR (quando quiser)

- **Configurações → Aplicativos → Aplicativos instalados → Mobilador → Desinstalar**;
- ou `Uninstall.exe` na pasta de instalação;
- ou Menu Iniciar → **Mobilador → Desinstalar**.

Ele remove tudo e pergunta se você quer apagar também os perfis salvos.

---

## NÃO QUER INSTALAR? — Modo portátil (pen drive)

Se quiser só usar sem deixar rastro no PC:

```
Mobilador-Setup-1.0.2.exe --portable
```

Ele extrai tudo para `Mobilador\` ao lado do `.exe` e fecha — sem atalhos, sem
registo, sem instalação. Para escolher a pasta:

```
Mobilador-Setup-1.0.2.exe --portable D:\Mobila
```

---

## ONDE FICA O QUE

| O quê | Onde |
|---|---|
| Programa | `%LOCALAPPDATA%\Programs\Mobilador\Mobilador.exe` |
| Registro da instalação | `%LOCALAPPDATA%\Programs\Mobilador\instalacao.log` |
| Perfis e configurações | `%APPDATA%\Mobilador` |
| Documentação | `%LOCALAPPDATA%\Programs\Mobilador\docs\` |
| Desinstalador | `%LOCALAPPDATA%\Programs\Mobilador\Uninstall.exe` |

---

## SE O APLICATIVO ABRIR COM A TELA BUGADA

1. Dentro do app, pressione **F6** — ele salva uma captura em
   `Documentos\Mobilador\captura-<data>-<hora>.bmp`.
2. Envie essa captura junto com o log: `%LOCALAPPDATA%\Mobilador\logs\`,
   o ficheiro `mobilador-<numero>.log` mais recente (a primeira linha dele diz
   qual versão está a correr).
3. Com esses dois ficheiros é possível ver exatamente o que está errado.

## PROBLEMAS COMUNS

| Sintoma | Solução |
|---|---|
| "O Windows protegeu o seu PC" | Mais informações → Executar mesmo assim |
| Antivírus apaga o `.exe` | Adicione exceção e execute de novo |
| "Acesso negado" ao gravar arquivo | O Mobilador está aberto — feche-o e clique em **Reinstalar** |
| "não foi possível criar a pasta de instalação" | Corrigido na **1.0.1**: o instalador cria a pasta final e, se não puder, tenta pastas alternativas. Se ainda aparecer: **Procurar...** → escolha uma pasta sua, ou use `--portable` |
| Nada acontece no duplo clique | Botão direito → Executar como administrador |
| Telefone não aparece | Ligue a Depuração USB e autorize o PC; instale o driver do fabricante |
| Instalei em outro PC e quero sem instalar | Use `--portable` |
