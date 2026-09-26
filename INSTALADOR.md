# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.5.exe`](release/Mobilador-Setup-1.0.5.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.5.exe` |
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

> **Versão 1.0.5** — corrige os controles: **botões disparavam ao passar o
> mouse**. `UiInput::new_frame()`, que limpa `pressed`/`released`/`wheel` e as
> teclas de cada frame, existia desde a primeira versão mas **nunca era
> chamada**. Depois do primeiro clique, `pressed[left]` ficava `true` para o
> resto da sessão: todo controle sob o cursor entrava em estado "pressionado",
> os botões agiam sem clique, os interruptores ligavam/desligavam sozinhos
> enquanto o ponteiro estava em cima, a roda acumulava sem limite e as teclas
> (apagar, escape) repetiam a cada frame. Os flags agora são limpos no fim de
> cada frame, depois de a interface os consumir.
>
> Também nesta versão: `WidgetCtx::end_frame()` (declarada, nunca definida nem
> chamada) removida; `tools/run_tests.sh` tinha o bloco de verificações
> duplicado, rodando tudo duas vezes.
>
> Novidade que fica: **`tools/check_ui_invariants.py`** roda em `tools/run_tests.sh`
> e falha se algum destes erros voltar — input sem limpeza por frame, índices 3×
> por vértice, atlas de glifos `DYNAMIC` com `UpdateSubresource`, `ps_text`
> ausente, ou o instalador reutilizando um executável antigo. Cada verificação foi
> testada por sabotagem: reintroduzindo o erro, ela falha.

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.5.exe --portable [pasta]`

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
