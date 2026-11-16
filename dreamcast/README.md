# Frets on Fire 1.3.110 no Dreamcast

Port em desenvolvimento do Frets on Fire clássico da Unreal Voodoo. A referência é o pacote Linux original da versão 1.3.110. A branch `dreamcast-classic` parte do port nativo que implementou a entrada Maple e o streaming Vorbis pela AICA.

## Teclado

No Flycast, selecione **Keyboard** como dispositivo da porta A.

| Ação | Tecla |
| --- | --- |
| Escolher opção | Setas, Enter ou F1 para confirmar |
| Cinco casas | F1 a F5 |
| Palhetar | Enter ou Shift direito |
| Pausar e retomar | Esc |

O jogo começa pelo menu principal. Em **Play Game**, selecione a música e a dificuldade. **Tutorial** abre o tutorial clássico.

## Compilação

Requisitos do port. KallistiOS 2.2.1, libvorbis e libogg em kos-ports, mkdcdisc, Python 3 com Pillow e pycollada, e FFmpeg com o encoder Yamaha ADPCM.

Extraia [FretsOnFire-1.3.110.tar.gz](https://sourceforge.net/projects/fretsonfire/files/fretsonfire/1.3.110/) em um diretório fora do checkout e use a pasta `data` como fonte dos gráficos, do áudio e dos charts.

```bash
source /caminho/para/kos/environ.sh
make -C dreamcast clean
make -C dreamcast -j4 cdi CLASSIC_DATA="/caminho/Frets on Fire-1.3.110/data"
```

Por padrão, a CDI monta os arquivos de `CLASSIC_DATA/songs` como a biblioteca do jogo. `SONGS_DIR` permite indicar outra biblioteca durante desenvolvimento. O tutorial vem do pacote original e é incluído no ROM disk. Ao trocar a fonte de recursos, execute `make clean` antes de recompilar.

A [referência de PC](REFERENCIA_PC.md) identifica a versão, os recursos e as dependências. A [matriz de paridade](PARIDADE.md) registra os resultados por recurso. O [checkpoint de compilação](CHECKPOINT.md) identifica os binários testados.

O código do port segue a GPL, versão 2 ou posterior. O pacote clássico fornece os créditos e as licenças dos recursos usados em `copying.txt`.
