# hdclone — clone de HD para imagem compactada

Ferramenta de linha de comando para fazer **backup de um disco inteiro** (todas as
partições, MBR/GPT, setor de boot) em um arquivo de imagem compactado `.hdc` e
**restaurar** essa imagem em outro HD.

- Roda do **Windows XP ao Windows 11**, sem instalar nada e sem DLLs.
- **Copia o disco com o Windows ligado** de forma consistente, usando **VSS**
  (cópias de sombra), sem precisar dar boot por pendrive.
- **Modo inteligente:** lê o bitmap do NTFS e a FAT do FAT32 e copia só os
  clusters em uso. O resto (outras partições, áreas desconhecidas) é copiado setor
  a setor, então qualquer sistema é preservado.
- Compressão zlib em blocos de 1 MB com **CRC32 por bloco**.
- Divide a imagem em partes de 4000 MB por padrão (cabe em pendrive FAT32).
- Setores defeituosos na origem são pulados (preenchidos com zero) e contados.
- Restaura em disco **maior** ou **menor** que o original (em disco menor, a última
  partição é reduzida automaticamente quando os dados cabem).

## Arquivos — mantenha os quatro na mesma pasta

| Arquivo | Para quê |
|---|---|
| `hdclone-gui.exe` | interface gráfica (abra este) |
| `hdclone.exe` | linha de comando |
| `hdclone-gui64.exe`, `hdclone64.exe` | versões 64 bits |

No Windows de 64 bits, o VSS só funciona em programas de 64 bits. Por isso o
`hdclone-gui.exe`/`hdclone.exe` abrem sozinhos a versão 64 bits se ela estiver na
mesma pasta. Basta sempre abrir o `hdclone-gui.exe`.

## VSS: backup com o Windows ligado

Ao fazer backup, o hdclone cria um *snapshot* (foto instantânea) de cada volume
do disco e copia a partir dele. A imagem fica exatamente como o disco estava naquele
instante, mesmo com programas abertos e arquivos mudando durante a cópia. Os
snapshots são temporários e apagados ao terminar (ou ao cancelar).

- Funciona no XP, Vista, 7, 8, 10 e 11, e no Server 2008 em diante.
  Não funciona no Server 2003 nem no XP 64 bits; nesses, a cópia é feita sem VSS.
- Só volumes **NTFS** têm snapshot. Partições FAT32 ou Linux no mesmo disco são
  lidas direto (o log avisa).
- Precisa do serviço "Cópias de Sombra de Volume" ativo (padrão do Windows) e de
  algum espaço livre no volume (uns 10% é seguro).
- Usa o tipo de backup "cópia", que não mexe no histórico de backups de SQL Server
  ou Exchange.
- Se o VSS falhar, o backup continua sem ele e o motivo aparece no log. Na
  interface, a mensagem final avisa quando o disco do Windows foi copiado sem VSS.
- Para desligar: desmarque "Usar VSS" ou use `--no-vss`.

## Restaurar em disco menor

Exemplo: HD de 500 GB com 60 GB usados, restaurado num SSD de 120 GB.

1. Antes de gravar qualquer coisa, o hdclone lê a tabela de partições e o mapa de
   espaço usado de dentro da imagem e confere se tudo cabe.
2. Se couber, restaura e **reduz a última partição** (NTFS ou FAT32), ajustando a
   tabela de partições (MBR, partição lógica/estendida ou GPT).
3. Se não couber, avisa o **tamanho mínimo de disco necessário** e não altera o
   destino.

Detalhes:
- Os dados **não são movidos** dentro da partição. Só dá para reduzir até o último
  cluster em uso. O Windows (principalmente o XP) costuma deixar arquivos de
  sistema no meio do volume, então o mínimo pode ser maior que o espaço usado.
  Se o mínimo ficar grande demais: no Windows de origem (Vista ou mais novo), use
  *Gerenciamento de Disco > Diminuir volume*, que junta os dados no início, e
  faça o backup de novo.
- Só a **última** partição é reduzida; as anteriores precisam caber inteiras.
- No NTFS, o volume é marcado para verificação: no primeiro boot o Windows roda o
  chkdsk uma vez. É normal, deixe terminar.
- NTFS com clusters defeituosos registrados não é reduzido (rode chkdsk e refaça o backup).

## Interface gráfica — `hdclone-gui.exe`

Dê dois cliques no `hdclone-gui.exe` (ele pede permissão de Administrador sozinho).

1. Escolha **Backup**, **Restaurar** ou **Verificar imagem**.
2. Selecione o disco na lista (mostra tamanho, modelo, letras das unidades e qual é o
   disco do Windows em uso).
3. Escolha o arquivo de imagem em **Procurar...**.
4. Clique em **Iniciar**. Barra de progresso, velocidade, tempo restante e um
   registro detalhado aparecem na janela; dá para **Cancelar** a qualquer momento.

Proteções: pede confirmação mostrando modelo/tamanho/unidades antes de apagar um
disco, recusa restaurar sobre o disco do Windows em uso ou em disco menor que o
original, e avisa ao copiar o disco do sistema ligado.

## Linha de comando — `hdclone.exe` (Prompt de Comando como Administrador)

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
`--zero-free` (zera o espaço livre no destino), `--no-vss`, `--force`.

## Importante

- **Restaurar** o disco do Windows em uso não é possível (ninguém consegue
  sobrescrever o sistema que está rodando). Para isso, inicie por um pendrive
  WinPE / Hiren's BootCD PE e rode o hdclone dele, ou ligue o HD em outro PC.
- Depois de restaurar em disco maior, use o Gerenciamento de Disco
  ("Estender volume") ou `diskpart` para usar o espaço extra.
- Windows XP clonado para hardware **diferente** pode dar tela azul
  (0x7B, driver de controladora SATA/IDE) — isso é do XP, não da imagem.

## Compilar

Em Linux (Debian/Ubuntu): `apt install gcc-mingw-w64-i686 gcc-mingw-w64-x86-64 libz-mingw-w64-dev`,
depois `make` → os quatro `.exe`. `make linux` gera uma versão Linux (usada nos testes).
