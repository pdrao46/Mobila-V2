# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.1.exe`](release/Mobilador-Setup-1.0.1.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.1.exe` |
| **Tamanho** | 43 903 793 bytes (41,9 MB) |
| **SHA-256** | `3441e9ce5554f91431404d85f7640f4d3ee0a0eedbe6648fa1f0136e8bd76bab` |
| **Sistema** | Windows 10/11, 64 bits |
| **Requisitos** | nenhum — sem Java, sem Visual C++, sem Android SDK, sem administrador |

> **Versão 1.0.1** — corrige um bug da 1.0.0 que impedia a instalação: a
> pasta de destino era testada antes de ser criada, então todo o
> mundo recebia `[ERRO] nao foi possivel criar a pasta de instalacao
> (permissao?)`. A lógica de criação de pastas agora tem teste
> automático (`tools/installer/test_payload.cpp`, roda em Linux) e o
> instalador ainda tenta pastas alternativas se a padrão falhar.

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.1.exe --portable [pasta]`

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
