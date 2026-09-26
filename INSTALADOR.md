# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.4.exe`](release/Mobilador-Setup-1.0.4.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.4.exe` |
| **Tamanho** | 43 905 841 bytes (41,9 MB) |
| **SHA-256** | `27d3e970180e2060f6e9f6d6aa2b7b42abb72626b01f1be4022d8a5a4f67d5e2` |
| **Sistema** | Windows 10/11, 64 bits |
| **Requisitos** | nenhum — sem Java, sem Visual C++, sem Android SDK, sem administrador |

> **Versão 1.0.1** — corrige um bug da 1.0.0 que impedia a instalação: a
> pasta de destino era testada antes de ser criada, então todo o
> mundo recebia `[ERRO] nao foi possivel criar a pasta de instalacao
> (permissao?)`. A lógica de criação de pastas agora tem teste
> automático (`tools/installer/test_payload.cpp`, roda em Linux) e o
> instalador ainda tenta pastas alternativas se a padrão falhar.

> **Versão 1.0.4** — corrige a geometria do quadro. O gerador de índices
> avançava o cursor **três** posições por vértice e escrevia `first+i*3+k`:
> cada quadrado (6 vértices) recebia 18 índices, ou seja desenhava **seis**
> triângulos em vez de dois, e quatro deles referenciavam vértices de *outros*
> elementos. Dentro do mesmo lote isso duplicava os glifos seguintes (texto mais
> escuro/grosso); na fronteira entre lotes, o lote anterior desenhava triângulos
> com a geometria do lote seguinte e com o *seu* shader — retângulos sólidos
> escuros por cima das primeiras letras e riscos pela tela. Um quadrado é
> `0,1,2 + 3,4,5`; agora é exatamente isso: um índice por vértice.
>
> O aplicativo também passou a **verificar-se**: no primeiro quadro confere que
> `icount == verts_used` e que os índices são `0,1,2,3,...`, e escreve no log
> `ui: geometry ok (N quads, M verts, pattern 0..n-1)` — ou um erro explícito se
> o padrão voltar a quebrar.

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.4.exe --portable [pasta]`

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
