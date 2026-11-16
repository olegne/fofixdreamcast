# Frets on Fire 1.3.110 no Dreamcast

Port em desenvolvimento do Frets on Fire clássico da Unreal Voodoo. A referência é o pacote Linux original da versão 1.3.110. A branch `dreamcast-classic` parte do port nativo que implementou a entrada Maple e o streaming Vorbis pela AICA.

## Teclado

No Flycast, selecione **Keyboard** como dispositivo da porta A.

| Ação | Tecla |
| --- | --- |
| Escolher opção | Setas, Enter ou F1 para confirmar |
| Cinco casas | F1 a F5 |
| Palhetar | Enter ou Shift direito |
| Abrir e fechar a pausa | Esc |

O jogo começa pelo menu principal. Em **Play Game**, selecione a biblioteca, a música e a dificuldade. Digite parte do título ou do artista para filtrar a lista. Escape volta à pasta anterior ou ao menu. **Tutorial** abre o tutorial clássico. No teclado, feche a pausa com **Esc** ou **F2**. Como no clássico de PC, **Enter** em **Return to Song** toca o som de confirmação e mantém o menu aberto. Quando a pausa é aberta durante a contagem inicial, a faixa começa ao final da contagem mesmo com o menu aberto.

Depois do resultado, **Enter** ou **F1** avança. Se a pontuação entrar entre as cinco melhores daquela faixa e dificuldade, digite o nome e confirme com **Enter** ou **F1**. **Esc** mantém o nome anterior. Depois escolha **Replay**, **Change Song** ou **Quit to Main Menu**. O jogo grava a tabela na primeira VMU disponível. No teste em Flycast, o teclado ficou na porta A e a VMU no primeiro slot do controle da porta B.

## Compilação

Requisitos do port. KallistiOS 2.2.1, SH4ZAM, libvorbis, libogg e zlib em kos-ports, mkdcdisc, Python 3 com Pillow e pycollada.

Extraia [FretsOnFire-1.3.110.tar.gz](https://sourceforge.net/projects/fretsonfire/files/fretsonfire/1.3.110/) em um diretório fora do checkout e use a pasta `data` como fonte dos gráficos, do áudio e dos charts.

```bash
source /caminho/para/kos/environ.sh
make -C dreamcast clean
make -C dreamcast -j4 cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data"
```

Por padrão, a CDI monta os arquivos de `CLASSIC_DATA/songs` como a biblioteca do jogo. `SONGS_DIR` permite indicar outra biblioteca durante desenvolvimento. O tutorial vem do pacote original e é incluído no ROM disk. Ao trocar a fonte de recursos, execute `make clean` antes de recompilar.

## Importação de músicas

Para criar uma música no editor, prepare os arquivos Ogg no PC. A faixa de instrumento é obrigatória. O acompanhamento e o ritmo são opcionais. O comando copia o áudio sem recodificar e prepara um chart vazio em 122 BPM. Exige Python 3 e FFprobe.

```bash
python3 dreamcast/tools/import_classic_songs.py ogg --library minhas-importacoes \
  --name "Minha música" --artist "Artista" --guitar /caminho/guitarra.ogg \
  --background /caminho/acompanhamento.ogg
make -C dreamcast cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data" \
  IMPORT_DIR="$PWD/minhas-importacoes"
```

Abra **Song Editor** e escolha a faixa importada. O editor grava o chart na VMU e lê o áudio da CDI. Para preparar uma faixa com chart existente, passe `--notes /caminho/notes.mid` e use a biblioteca gerada com `SONGS_DIR`.

Para usar uma etiqueta própria no seletor, coloque `label.png` na pasta da música ou na pasta da biblioteca. A compilação da CDI prepara a textura que o seletor usa na fita ou na caixa.

Para as edições de PS2 de Guitar Hero I, II e Encore reconhecidas pelo original, monte ou extraia o seu DVD no PC. O importador lê `gen/main.hdr` e `gen/main_0.ark`, preserva o MIDI e converte as trilhas VGS para Ogg a 44,1 kHz. Ele usa `oggenc` quando instalado ou FFmpeg com libvorbis.

```bash
python3 dreamcast/tools/import_classic_songs.py gh --disc /caminho/DVD-montado \
  --classic-data "/caminho/Frets on Fire-1.3.110/data" --library minha-biblioteca
make -C dreamcast cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data" \
  SONGS_DIR="$PWD/minha-biblioteca"
```

A [referência de PC](REFERENCIA_PC.md) identifica a versão, os recursos e as dependências. O [inventário do Linux](ESCOPO_LINUX.md) cobre os menus e modos originais. A [matriz de paridade](PARIDADE.md) registra os resultados por recurso. O [checkpoint de compilação](CHECKPOINT.md) identifica os binários testados.

O código do port segue a GPL, versão 2 ou posterior. O pacote clássico fornece os créditos e as licenças dos recursos usados em `copying.txt`.
