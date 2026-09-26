# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.6.exe`](release/Mobilador-Setup-1.0.6.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.6.exe` |
| **Tamanho** | 43 905 841 bytes (41,9 MB) |
| **SHA-256** | `f2f57c279bc9db0e8da0c3a051595b496a6e5e04f2739fe820044e83b960c989` |
| **Sistema** | Windows 10/11, 64 bits |
| **Requisitos** | nenhum — sem Java, sem Visual C++, sem Android SDK, sem administrador |

> **Versão 1.0.1** — corrige um bug da 1.0.0 que impedia a instalação: a
> pasta de destino era testada antes de ser criada, então todo o
> mundo recebia `[ERRO] nao foi possivel criar a pasta de instalacao
> (permissao?)`. A lógica de criação de pastas agora tem teste
> automático (`tools/installer/test_payload.cpp`, roda em Linux) e o
> instalador ainda tenta pastas alternativas se a padrão falhar.

> **Versão 1.0.6** — corrige o crash ao mudar o tamanho da interface, o
> `adb.exe not found` e o layout das métricas.
>
> **1. O app fechava ao mexer na escala.** O ecrã de configurações chamava
> `text.set_scale()` **durante o desenho**: essa função liberta as texturas do
> atlas de glifos, mas os comandos de desenho já gravados neste mesmo frame
> apontavam para elas. O `Ui2D::end()` submetia então um recurso já libertado
> (device removed) e a janela fechava sem mensagem. A mudança de escala passa a
> ser **adiada para o fim do frame** (`App::request_ui_scale` → aplicada em
> `App::tick` depois de `draw()`), e o contexto de dispositivo GDI é reutilizado
> em vez de recriado (vazava 9 por cada mudança).
>
> **2. `adb.exe not found`.** O instalador colocava `adb.exe` na raiz da
> instalação e o `find_adb()` procurava apenas em `<exe>\tools` — o aplicativo
> nunca encontrava o adb que ele mesmo instalava, e nenhum celular podia ser
> alcançado. Agora o `adb.exe` e as duas DLLs vão para `tools\` (onde o app
> procura primeiro) e o `find_adb()` também aceita os ficheiros ao lado do
> executável, o que faz instalações antigas funcionarem sem reinstalar.
>
> **3. Métricas fora de lugar.** As seis métricas do painel usavam a geometria
> de **duas** colunas para **três** colunas: a primeira ocupava 74% da linha, a
> segunda saía cortada na borda direita e a terceira nunca aparecia
> (`FPS DISPLAY` e `CPU` não existiam na tela). Além disso a altura fixa de 70 px
> cortava 24 px da base dos números grandes. Agora: três colunas iguais,
> altura calculada a partir das métricas reais das fontes e o valor ancorado na
> base do cartão.
>
> **4.** No botão grande, rótulo e sub-rótulo eram posicionados por deslocamentos
> fixos a partir do centro e sobrepunham-se alguns pixéis; passam a ser
> empilhados pelas alturas de linha reais. E o `"%.2fx x"` do controlo
> deslizante imprimia `1.00x x`.
>
> O `tools/check_ui_invariants.py` cobre agora estes quatro casos (25
> verificações no total, todas validadas por sabotagem).

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.6.exe --portable [pasta]`

## Guias

- [`release/PASSO-A-PASSO.md`](release/PASSO-A-PASSO.md) — passo a passo ilustrado, do
  download ao telemóvel ligado, com problemas comuns e como desinstalar.
- [`release/COMO-INSTALAR.txt`](release/COMO-INSTALAR.txt) — resumo de uma página.

## Reconstruir o instalador

```bash
python3 tools/installer/build_installer.py
```

O pipeline compila o app C++ (`tools/build.py`), o módulo Android (`ECJ` → `D8` → `.dex`),
empacota os 23 ficheiros do payload, compila o stub do instalador com os recursos
(ícone + versão) e verifica o payload duas vezes antes de gerar o `.exe`.
