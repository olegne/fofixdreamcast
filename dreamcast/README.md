# Frets on Fire 1.3.110 no Dreamcast

Port em desenvolvimento do Frets on Fire clássico da Unreal Voodoo. A referência é o pacote Linux original da versão 1.3.110. A branch `dreamcast-classic` parte do port nativo que implementou a entrada Maple e o streaming Vorbis pela AICA.

## Controles

O teclado Maple assume o comando assim que é reconhecido em qualquer porta. O jogo ignora os botões do controle enquanto houver teclado. Ao remover o teclado, o primeiro controle conectado assume. Solte as teclas ou os botões do dispositivo que assumiu antes de voltar a tocar. Um controle conectado pode continuar com a VMU para gravação.

| Ação | Teclado Maple | Controle Dreamcast |
| --- | --- | --- |
| Navegar e confirmar | Setas, Enter ou F1 | Direcional, A |
| Voltar nos menus | Esc ou F2 | B |
| Cinco casas na ordem | F1, F2, F3, F4, F5 | L, X, Y, B, R |
| Palhetar | Enter ou Shift direito | A, direcional para cima ou para baixo |
| Pausar | Esc | START |
| Retomar pela pausa | Esc ou F2 | B ou A em **Return to Song** |

No Flycast, a configuração com **Keyboard** na porta A e **Controller** com VMU na porta B permite testar os dois. O controle também funciona sozinho.

O jogo começa pelo menu principal. Em **Play Game**, selecione a biblioteca, a música e a dificuldade. Digite parte do título ou do artista para filtrar a lista. Escape volta à pasta anterior ou ao menu. **Tutorial** abre o tutorial clássico. No teclado, feche a pausa com **Esc** ou **F2**. Como no clássico de PC, **Enter** em **Return to Song** toca o som de confirmação e mantém o menu aberto. Quando a pausa é aberta durante a contagem inicial, a faixa começa ao final da contagem mesmo com o menu aberto.

Depois do resultado, **Enter** ou **F1** avança. Se a pontuação entrar entre as cinco melhores daquela faixa e dificuldade, digite o nome e confirme com **Enter** ou **F1**. **Esc** mantém o nome anterior. Depois escolha **Replay**, **Change Song** ou **Quit to Main Menu**. O jogo grava a tabela na primeira VMU disponível. No teste em Flycast, o teclado ficou na porta A e a VMU no primeiro slot do controle da porta B.

## Vídeo

Em **Settings → Video Settings**, escolha **640 × 480** ou **320 × 240** e aplique. Para reduzir o trabalho do renderizador, selecione meia frequência. Em uma saída de 60 Hz isso apresenta cerca de 30 quadros por segundo. O jogo continua recebendo entrada e atualizando o PCM em frequência cheia. O PowerVR oferece antialiasing horizontal 2× em 320 × 240. A saída ocupa a tela inteira e a VMU guarda as escolhas. O padrão é 640 × 480 em frequência cheia. O modo 320 × 240 depende do cabo e da tela para a prova física.

## Nomes e recordes

O teclado Maple aceita caracteres acentuados conforme o mapa regional. Com o teclado identificado como US, segure **Alt direito** e pressione uma combinação da tabela. Solte as teclas e digite a letra. Use Shift na letra para obter a versão maiúscula. **Backspace** apaga um caractere inteiro.

| Acento | Tecla com Alt direito |
| --- | --- |
| Agudo | Apóstrofo |
| Grave | Crase |
| Til | Shift + crase |
| Circunflexo | 6 |
| Trema | Shift + apóstrofo |
| Cedilha | Vírgula |

Para trazer os recordes do Linux, compile a CDI com a sua biblioteca de músicas em `SONGS_DIR`, incluindo os arquivos `song.ini` do seu perfil Frets on Fire. A compilação valida os registros do 1.3.110 e inclui as tabelas no disco. Na seleção de músicas e no resultado, o jogo combina essas tabelas com as pontuações locais da VMU. A gravação local usa UTF-8 e lê os saves antigos. Uma edição que muda o MIDI recebe tabela própria. Alterações de nome, capa ou atraso mantêm a tabela do mesmo MIDI.

### Ranking online

Em **Settings → Game Settings → Upload Highscores**, altere **No** para **Yes** para enviar as tabelas de recordes da faixa após um novo recorde elegível. A opção começa desligada e é gravada na VMU. O envio roda em segundo plano e mostra o resultado na tela de recordes. O identificador da faixa usa o SHA-1 do MIDI em execução, inclusive se foi salvo no editor.

O cliente usa IPv4 com a pilha de rede do KallistiOS e requer um adaptador de rede compatível no Dreamcast. Na configuração padrão, o URL é o mesmo do Frets on Fire 1.3.110 no Linux. A disponibilidade desse servidor segue sem confirmação. Para usar um servidor HTTP compatível na sua rede, grave o endereço em uma linha de um arquivo de texto e passe `RANKING_URL_FILE=/caminho/ranking.url` ao compilar a CDI. Um endereço de exemplo é `http://192.168.1.20:8080/play`. O cliente envia o GET clássico com o nome de diretório da faixa, o SHA-1 do MIDI, a versão e os recordes Cerealizer. O teste no Flycast usou um servidor local e BBA emulado.

## Compilação

Requisitos do port. KallistiOS 2.2.1 com libkosfat, SH4ZAM, libvorbis, libogg e zlib em kos-ports, mkdcdisc, Python 3 com Pillow e pycollada.

Extraia [FretsOnFire-1.3.110.tar.gz](https://sourceforge.net/projects/fretsonfire/files/fretsonfire/1.3.110/) em um diretório fora do checkout e use a pasta `data` como fonte dos gráficos, do áudio e dos charts.

```bash
source /caminho/para/kos/environ.sh
make -C dreamcast clean
make -C dreamcast -j4 cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data"
```

Por padrão, a CDI monta os arquivos de `CLASSIC_DATA/songs` como a biblioteca do jogo. `SONGS_DIR` permite indicar outra biblioteca durante desenvolvimento. O tutorial vem do pacote original e é incluído no ROM disk. Ao trocar a fonte de recursos, execute `make clean` antes de recompilar.

## Importação de músicas

### Cartão SD no Dreamcast

Use um adaptador SD na porta serial e um cartão com tabela MBR e primeira partição FAT16 ou FAT32. O port monta o cartão somente para leitura. Conecte o adaptador e o cartão antes de abrir **Song Editor → Load SD Card**.

| Pasta na primeira partição | Arquivos | Onde abrir |
| --- | --- | --- |
| `songs/MinhaFaixa/` | `notes.mid` e `guitar.ogg` ou `song.ogg`. `song.ini` e `rhythm.ogg` são opcionais | **Play Game → SD Card** ou **Edit Existing Song → SD Card** |
| `imports/MinhaFaixa/` | `guitar.ogg`. `song.ogg`, `song.ini` e `rhythm.ogg` são opcionais | **Song Editor → Import New Song** |

Cada faixa pode ficar numa subpasta de biblioteca. Quando há `song.ogg` e `guitar.ogg`, o primeiro é o acompanhamento e o segundo fica sujeito aos acertos. Use arquivos Ogg Vorbis de um ou dois canais, de 8 a 48 kHz. Em **Import New Song**, o editor abre um chart vazio quando falta MIDI. Após gravar um chart com notas na VMU, a faixa também aparece em **Play Game → SD Card**. Mantenha o cartão conectado durante a sessão e os nomes das pastas e arquivos para reabrir os charts da VMU. Para trocar de cartão, reinicie o jogo.

As faixas e importações do cartão podem ser adicionadas sem recompilar a CDI. O Flycast não fornece o adaptador serial SPI. A leitura FAT e o caminho do editor foram exercitados no emulador com um dispositivo de blocos de teste, e a transferência do cartão por SPI ainda requer o Dreamcast físico.

### Preparação para a CDI

Para criar uma música no editor, prepare os arquivos Ogg no PC. A faixa de instrumento é obrigatória. O acompanhamento e o ritmo são opcionais. O comando copia o áudio sem recodificar e prepara um chart vazio em 122 BPM. Exige Python 3 e FFprobe.

```bash
python3 dreamcast/tools/import_classic_songs.py ogg --library minhas-importacoes \
  --name "Minha música" --artist "Artista" --guitar /caminho/guitarra.ogg \
  --background /caminho/acompanhamento.ogg
make -C dreamcast cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data" \
  IMPORT_DIR="$PWD/minhas-importacoes"
```

Abra **Song Editor** e escolha a faixa importada. O editor grava o chart na VMU e lê o áudio da CDI. Para preparar uma faixa com chart existente, passe `--notes /caminho/notes.mid` e use a biblioteca gerada com `SONGS_DIR`.

### Capas do editor

Na edição de uma música, pressione Esc e abra **Set Cassette Label**. O seletor mostra os três rótulos originais e as imagens adicionais incluídas na compilação. Escolha **Original cassette label** para restaurar o rótulo da faixa. Use **Save Song** para manter a escolha na VMU.

Para incluir imagens próprias, coloque os PNGs em uma pasta e indique-a por `LABEL_DIR` ao compilar a CDI. Use imagens de 256 × 128 e nomes de até 48 caracteres com letras ASCII, números, espaços, hífens ou sublinhados.

```sh
make -C dreamcast cdi CLASSIC_DATA="/caminho/data" LABEL_DIR="/caminho/capas"
```

A VMU guarda a referência à imagem convertida. Leve o PNG com o mesmo nome ao remontar a CDI. Quando a imagem escolhida estiver ausente, a fita volta a exibir o rótulo original.

Para usar uma etiqueta própria no seletor, coloque `label.png` na pasta da música ou na pasta da biblioteca. A compilação da CDI prepara a textura que o seletor usa na fita ou na caixa.

### Guitar Hero

Para as edições de PS2 de Guitar Hero I, II e Encore reconhecidas pelo original, monte ou extraia o seu DVD no PC. O importador lê `gen/main.hdr` e `gen/main_0.ark`, preserva o MIDI e converte as trilhas VGS para Ogg a 44,1 kHz. Ele usa `oggenc` quando instalado ou FFmpeg com libvorbis. Copie as pastas que ele gerar para `songs/` no cartão SD ou use `SONGS_DIR` para gravá-las na CDI.

```bash
python3 dreamcast/tools/import_classic_songs.py gh --disc /caminho/DVD-montado \
  --classic-data "/caminho/Frets on Fire-1.3.110/data" --library minha-biblioteca
make -C dreamcast cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data" \
  SONGS_DIR="$PWD/minha-biblioteca"
```

## Validação no Dreamcast físico

Use o mesmo código e o SHA-256 da CDI registrados no checkpoint. Ligue um teclado Maple na porta A e uma VMU no primeiro slot de um controle na porta B. Registre o modo de vídeo usado e mantenha a comparação com Linux em 4:3.

1. Inicialize com os dois dispositivos e verifique que os botões do controle são ignorados. Exerça a mesma CDI com o controle sozinho e confira menus, casas, palhetada e pausa. Abra o tutorial com o teclado, faça um acorde com três teclas F, alterne Enter e Shift direito, pause e retome durante uma nota longa e reinicie a faixa.
2. Termine as três músicas da biblioteca e confira som, resultado e nova partida. Escute os dois canais. Registre a posição na música se houver corte, atraso ou desvio.
3. Digite um nome com acento, grave os recordes, altere o atraso A/V e reinicie para ver se os valores voltaram. Para a prova do editor, salve também a seleção de capa e reabra o chart.
4. Para o ensaio de rede, conecte o adaptador e use a CDI com o endereço de um servidor compatível da rede local. Confirme uma partida com envio desligado e outra com envio ligado. Compare o resultado na tela com o que o servidor recebeu.

A [referência de PC](REFERENCIA_PC.md) identifica a versão, os recursos e as dependências. O [inventário do Linux](ESCOPO_LINUX.md) cobre os menus e modos originais. A [matriz de paridade](PARIDADE.md) registra os resultados por recurso. O [checkpoint de compilação](CHECKPOINT.md) identifica os binários testados.

O código do port segue a GPL, versão 2 ou posterior. O pacote clássico fornece os créditos e as licenças dos recursos usados em `copying.txt`.
