# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.7.exe`](release/Mobilador-Setup-1.0.7.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.7.exe` |
| **Tamanho** | 43 910 979 bytes (41,9 MB) |
| **SHA-256** | `1bbe861a310f0d9c4f5eb1828d0fcc29929ce2073bd71ab07889eaeaaeda6996` |
| **Sistema** | Windows 10/11, 64 bits |
| **Requisitos** | nenhum — sem Java, sem Visual C++, sem Android SDK, sem administrador |

> *As notas estão por ordem crescente de versão; a última é a que está publicada.*
>
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
> O `tools/check_ui_invariants.py` cobre agora estes quatro casos.

> **Versão 1.0.7** — nova identidade visual (verde sobre quase-preto), rolagem
> em todas as abas e **todos** os botões a fazer o que prometem.
>
> **1. Nada mais fica cortado.** Cada ecrã (`draw_dashboard`, `draw_performance`,
> `draw_latency`, `draw_benchmark`, `draw_diagnostics`, `draw_settings`,
> `draw_about`) devolve agora **a altura que realmente desenhou**, e a barra de
> rolagem é calculada a partir daí (`screen_content_h[tela]`). As constantes
> fixas por tela (`SP(690)` no painel) foram removidas — eram a razão por que o
> `QUICK PERFORMANCE` e o `AUTO OPTIMIZE` ficavam a meio do botão, sem forma de
> descer.
>
> **2. Todos os botões funcionam, a otimizar a sério.**
> `QUICK PERFORMANCE` aplica o preset de menor latência **e** as capacidades
> reais do conjunto (descodificação na GPU/CPU, vsync, frame pacing) e mostra o
> resultado (`1920x1080 @ 120 fps - decode na GPU`). `AUTO OPTIMIZE` passa a
> correr o otimizador de verdade (`settings.auto_optimize()` com as capacidades
> medidas do PC e do celular) e só depois o diagnóstico — antes apenas relia o
> diagnóstico e não mudava nada. `RESTAURAR PADROES`, `BENCHMARK`, `EXPORTAR
> CSV`, `RESETAR MEDICOES`, `SALVAR`/`COPIAR` do diagnóstico, presets, grupos,
> controlos deslizantes, listas e interruptores foram todos revistos um a um.
> `tools/check_ui_invariants.py` ganhou uma verificação que falha se algum
> controlo for desenhado sem tratamento de clique (com auto-teste, para a
> verificação não poder passar em falso).
>
> **3. Design de produto.** Paleta escura `#0A0E14 / #111823 / #172030 /
> #1F2A3A` com acento **verde `#22C55E`** por omissão (os 8 presets continuam
> disponíveis), barra lateral com o item ativo em pílula cheia e o logotipo num
> quadrado suave, cartão de métricas com separadores finos e números grandes,
> cartões de 14 px, botões de 10 px, pílulas no topo com os valores medidos e um
> brilho ténue do acento sob a barra superior.
>
> **4. Verificação.** Compilação sem um único aviso (`-Wall -Wextra`), 146 testes
> automáticos e 36 verificações de UI/entrada (todas com teste negativo).

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.7.exe --portable [pasta]`

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
