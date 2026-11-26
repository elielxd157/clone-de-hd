# hdclone — clone de HD para imagem compactada

Ferramenta de linha de comando para fazer **backup de um disco inteiro** (todas as
partições, MBR/GPT, setor de boot) em um arquivo de imagem compactado `.hdc` e
**restaurar** essa imagem em outro HD.

- Roda do **Windows XP ao Windows 11** (um único `hdclone.exe`, 32 bits, sem DLLs).
- **Modo inteligente:** lê o bitmap do NTFS e a FAT do FAT32 e copia só os
  clusters em uso. O resto (outras partições, áreas desconhecidas) é copiado setor
  a setor, então qualquer sistema é preservado.
- Compressão zlib em blocos de 1 MB com **CRC32 por bloco**.
- Divide a imagem em partes de 4000 MB por padrão (cabe em pendrive FAT32).
- Setores defeituosos na origem são pulados (preenchidos com zero) e contados.
- Restaurar num disco **maior** funciona (em GPT, a tabela é ajustada sozinha).

## Uso (Prompt de Comando como Administrador)

```
hdclone list                          lista os discos (número, tamanho, modelo, letras)
hdclone backup 1 E:\backup\pc.hdc     disco 1 -> imagem
hdclone verify E:\backup\pc.hdc       confere a imagem (recomendado antes de restaurar)
hdclone restore E:\backup\pc.hdc 2    imagem -> disco 2 (APAGA o disco 2)
hdclone info E:\backup\pc.hdc
```

Dar dois cliques no `hdclone.exe` abre um **menu interativo**.

Opções: `-c 1..9` (compressão, padrão 6), `-s MB` (tamanho das partes, `-s 0` = arquivo
único), `--raw` (copia tudo, inclusive espaço livre), `-y` (sem confirmação),
`--zero-free` (zera o espaço livre no destino), `--force`.

## Importante

- **Disco do Windows em uso:** o clone funciona, mas arquivos abertos podem sair
  inconsistentes. Para um clone perfeito, inicie por um pendrive WinPE /
  Hiren's BootCD PE (o `hdclone.exe` roda neles) ou ligue o HD em outro PC.
- O disco de destino precisa ser **do mesmo tamanho ou maior** que o original.
- Depois de restaurar em disco maior, use o Gerenciamento de Disco
  ("Estender volume") ou `diskpart` para usar o espaço extra.
- Windows XP clonado para hardware **diferente** pode dar tela azul
  (0x7B, driver de controladora SATA/IDE) — isso é do XP, não da imagem.

## Compilar

Em Linux (Debian/Ubuntu): `apt install gcc-mingw-w64-i686 libz-mingw-w64-dev`, depois
`make` → `hdclone.exe`. `make linux` gera uma versão Linux (usada nos testes).
