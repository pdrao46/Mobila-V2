# 📥 Instalador do Mobilador (Windows)

O instalador **não** fica no código-fonte — ele é um artefacto binário construído pelo
pipeline deste repositório. Está publicado aqui mesmo, nesta pasta:

## ➡️ [`release/Mobilador-Setup-1.0.3.exe`](release/Mobilador-Setup-1.0.3.exe) — 41,9 MB

Clique no link acima **e depois no botão de download** que aparece no canto direito da
página (ícone ⬇ *Download* / *Download raw file*).

| | |
|---|---|
| **Ficheiro** | `Mobilador-Setup-1.0.3.exe` |
| **Tamanho** | 43 905 841 bytes (41,9 MB) |
| **SHA-256** | `e5268e6b51403a4773a3f28b5cab06a382bd1bfebd5a30bd1b51cc64ce7fd004` |
| **Sistema** | Windows 10/11, 64 bits |
| **Requisitos** | nenhum — sem Java, sem Visual C++, sem Android SDK, sem administrador |

> **Versão 1.0.1** — corrige um bug da 1.0.0 que impedia a instalação: a
> pasta de destino era testada antes de ser criada, então todo o
> mundo recebia `[ERRO] nao foi possivel criar a pasta de instalacao
> (permissao?)`. A lógica de criação de pastas agora tem teste
> automático (`tools/installer/test_payload.cpp`, roda em Linux) e o
> instalador ainda tenta pastas alternativas se a padrão falhar.

> **Versão 1.0.3** — corrige a renderização do texto, que aparecia como blocos
> pretos sólidos. Eram **duas** causas no caminho dos glifos:
>
> 1. o atlas de glifos é uma textura `R8_UNORM` (só cobertura em `.r`), mas o
>    pixel shader da UI lia `.rgb` e `.a` como se fosse ARGB — numa textura R8 o
>    `Sample` devolve `(cobertura, 0, 0, 1)`, ou seja blocos opacos sem os canais
>    verde e azul;
> 2. o atlas era criado como `D3D11_USAGE_DYNAMIC` e escrito com
>    `UpdateSubresource` — combinação que o Direct3D **não executa** (recurso
>    DYNAMIC exige `Map`/`Unmap`). O upload não acontecia, a cobertura ficava
>    zero e todo o texto saía preto opaco.
>
> Agora existe um shader `ps_text` dedicado a texturas de cobertura, o atlas usa
> `D3D11_USAGE_DEFAULT` (par correto do `UpdateSubresource`) e uma verificação no
> arranque lê o atlas de volta e escreve no log se ele vier vazio.
>
> Também nesta versão: `build.py` e `build_installer.py` deixaram de reutilizar
> artefatos antigos (o build ignorava alterações em headers e o empacotador
> reutilizava o `dist/Mobilador.exe` existente — foi assim que uma versão saiu
> com o executável anterior dentro).

## Como instalar

1. Baixe o `.exe`.
2. Duplo clique → *Mais informações* → *Executar mesmo assim* (aviso normal do
   SmartScreen para programas sem assinatura digital).
3. Clique em **INSTALAR**.
4. Abra o **Mobilador** e ligue a **Depuração USB** no telemóvel.

> Modo portátil (extrai sem instalar): `Mobilador-Setup-1.0.3.exe --portable [pasta]`

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
