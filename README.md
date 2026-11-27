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
- Compressão **zstd** em blocos de 1 MB com **CRC32 por bloco**, usando todos os
  núcleos do processador, com leitura, compressão e gravação acontecendo ao mesmo tempo.
- Divide a imagem em partes de 4000 MB por padrão (cabe em pendrive FAT32).
- Setores defeituosos na origem são pulados (preenchidos com zero) e contados.
- **Clona disco para disco** direto, sem imagem intermediária (inclusive para disco menor).
- **Recupera arquivos soltos** da imagem (NTFS e FAT32), com **busca por nome**.
- **Prepara o Windows para outro hardware** (evita a tela azul 0x7B), do XP ao 11.
- **Verificação ao terminar** e relatório **.log** salvo junto da imagem.
- Deixa de fora `pagefile.sys` e `hiberfil.sys` (o Windows recria): imagem bem menor.
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

## Velocidade e compressão

- **zstd** no lugar do zlib: no mesmo processador, comprime ~6–8× mais rápido e
  descomprime ~4× mais rápido, com imagem do mesmo tamanho ou menor.
- **Ler, comprimir e gravar ao mesmo tempo.** Mesmo num PC de 1 núcleo, o disco
  lê o próximo bloco enquanto o processador comprime. Com HD lento, o backup
  passa a andar na velocidade do disco.
- **Usa todos os núcleos** (até 8) para comprimir.
- **Gravação direta**: a imagem é gravada sem passar pelo cache do Windows, o que
  evita que um XP com pouca memória fique lento ao gravar arquivos enormes.

Níveis na interface: **Rápida** (zstd 1), **Normal** (zstd 3, padrão), **Máxima**
(zstd 9) e **Compatível com versões antigas** (zlib 6).

**Compatibilidade:** esta versão lê imagens de todas as versões anteriores. Imagens
novas (zstd) **não abrem em versões antigas do hdclone**; a versão antiga mostra
"versão de imagem não suportada". Se precisar restaurar com uma versão antiga
(por exemplo, num pendrive de boot antigo), escolha **Compatível com versões
antigas** ou use `--compat`. Nesse modo, blocos que não comprimem (fotos, vídeos,
zip) são detectados rapidamente e gravados direto.

## Clonar disco para disco

Modo **Clonar disco** na interface (ou `hdclone clone <origem> <destino>`). Copia
direto de um disco para outro, sem arquivo de imagem. Tem as mesmas proteções e
recursos do backup + restauração:

- VSS na origem (clona o disco do Windows ligado);
- só copia o espaço usado e deixa `pagefile`/`hiberfil` de fora;
- destino **menor** (reduz a última partição, se couber) ou **maior** (GPT ajustada);
- opção de verificar (lê o destino de volta e confere bloco a bloco);
- opção de preparar para outro hardware.

**Depois de clonar:** antes de dar boot pelo disco novo, desligue o PC e
desconecte o disco antigo. Os dois discos ficam idênticos e o Windows pode se
confundir com os dois ligados.

## Verificação e relatório (.log)

- **Verificar ao terminar** (`--verify`): ao fim do backup a imagem é lida de volta
  do disco e cada bloco é conferido. No clone, o disco de destino é relido e
  comparado.
- Todo backup grava um **relatório** `imagem.hdc.log` ao lado da imagem: data,
  origem, partições, opções, tamanho, erros e resultado final.

## Preparar para outro hardware (tela azul 0x7B)

Ao restaurar ou clonar para **outro computador**, o Windows pode dar tela azul
`0x0000007B INACCESSIBLE_BOOT_DEVICE`: o driver da controladora de disco do PC novo
não está ativado no boot. A opção **"Preparar p/ outro PC"** (`--hwfix`, ou
`hdclone hwfix <disco>` para um disco já restaurado) ativa no boot os drivers de
disco **que já vêm no Windows**, editando o registro do disco de destino:

| Windows | Drivers ativados |
|---|---|
| XP / 2000 / 2003 | atapi, intelide, pciide (mesma ideia do MergeIDE) |
| Vista / 7 | + msahci, iaStorV (recomendação da Microsoft, KB922976) |
| 8 / 8.1 / 10 / 11 | + storahci, stornvme, e zera o `StartOverride` |

Só são alterados valores que já existem, no próprio lugar. Nenhuma chave é criada,
e o relatório lista cada alteração. Limites:

- **XP em PC com SATA no modo AHCI:** o XP não tem driver AHCI. Mude no BIOS para
  **IDE/Compatível**.
- **Windows 7 com SSD NVMe:** o 7 não tem driver NVMe. Instale a atualização
  KB2990941 antes do backup, ou use SATA.
- Drivers de fabricante (RAID, controladoras especiais) não vêm no Windows e
  continuam precisando ser instalados.
- A ativação do Windows/Office pode pedir reativação num hardware diferente.

## Recuperar arquivos da imagem

Para pegar só alguns arquivos ou pastas (ex.: os Documentos de alguém) sem
restaurar o disco:

- **Interface:** escolha **Recuperar arquivos**, selecione a imagem e clique em
  **Abrir imagem**. Abre uma janela parecida com o Windows Explorer: escolha a
  partição, navegue nas pastas (dois cliques; Backspace volta), selecione os
  itens (Ctrl/Shift para vários) e clique em **Extrair selecionados...**.
- **Linha de comando:**
  ```
  hdclone ls pc.hdc                          lista as partições NTFS
  hdclone ls pc.hdc 1 "Users\Ana"            lista uma pasta
  hdclone extract pc.hdc 1 "Users\Ana\Documents" D:\recuperado
  ```

**Busca:** digite parte do nome (ou use curingas: `*.docx`, `foto?.jpg`) e clique em
**Buscar**. A busca procura na pasta atual e em todas as subpastas; dois cliques num
resultado de pasta a abrem, e os resultados podem ser extraídos direto. Na linha de
comando: `hdclone find pc.hdc 1 "*.docx" "Users\Ana"`.

As datas de modificação são preservadas. Funciona com partições **NTFS e FAT32**
(nomes longos e acentos), inclusive arquivos grandes e fragmentados. Ficam de fora, com aviso:
arquivos **comprimidos pelo NTFS** (pastas com "Compactar conteúdo"), arquivos
**criptografados com EFS** e atalhos de pasta (*junctions*, como "Documents and
Settings" no Vista+, que apontam para outro lugar).

## pagefile.sys e hiberfil.sys

Por padrão o backup **não copia** o conteúdo de `pagefile.sys`, `hiberfil.sys` e
`swapfile.sys`. São arquivos temporários do Windows, recriados a cada boot, e
costumam somar de 4 a 32 GB. Os arquivos continuam existindo no disco
restaurado, com o mesmo tamanho; só o conteúdo antigo é descartado. O início
de cada um é gravado como zeros, para o Windows não tentar "voltar da
hibernação" (inclusive a Inicialização Rápida do Windows 8+) e fazer um boot
normal. Para copiar mesmo assim: desmarque a opção na interface ou use
`--keep-sys`.

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
hdclone clone 1 2 --verify             disco 1 -> disco 2 direto (APAGA o disco 2)
hdclone hwfix 2                        prepara o Windows do disco 2 para outro PC
hdclone info E:\backup\pc.hdc
```

Dar dois cliques no `hdclone.exe` abre um **menu interativo**.

Opções: `-c N` (compressão zstd 1–19, padrão 3), `--compat` (formato antigo zlib), `-s MB` (tamanho das partes, `-s 0` = arquivo
único), `--raw` (copia tudo, inclusive espaço livre), `-y` (sem confirmação),
`--zero-free` (zera o espaço livre no destino), `--no-vss`, `--keep-sys`, `--verify`, `--hwfix`, `--force`.

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
depois `make` → os quatro `.exe`. O zstd vem embutido em `zstd/` (licença BSD,
gerado como arquivo único e compilado **sem** `ZSTD_MULTITHREAD`, que exigiria Windows Vista+). `make linux` gera uma versão Linux (usada nos testes).
