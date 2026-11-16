# FoFiX Dreamcast · R1

Protótipo nativo para Dreamcast com uma faixa de guitarra de cinco pistas. A versão inclui a faixa original **Circuito 01**, com cerca de 23 segundos, mudança de andamento e 47 alvos entre notas e acordes. O MIDI e o Ogg ficam embutidos no executável.

## Jogar

Abra `FoFiX_Dreamcast_R1.cdi` no Flycast. Para carregar pelo `dc-load`, use `FoFiX_Dreamcast_R1.elf`.

| Ação | Controle Dreamcast |
| --- | --- |
| Começar ou repetir a faixa | START |
| Verde, vermelho, amarelo, azul, laranja | L, X, Y, B, R |
| Tocar | D-pad para cima ou para baixo, ou A |
| Ajustar a sincronia entre partidas | D-pad para esquerda ou direita |

Segure o botão da cor e toque quando a nota alcançar a linha branca. Em um acorde, segure todas as cores indicadas. O ajuste de sincronia é de 10 ms por toque. A direita atrasa a chegada das notas e a esquerda adianta.

## Compilar

Use KallistiOS 2.2.1 ou compatível, com `libvorbis` e `libogg` instalados via `kos-ports`. A compilação também usa Python 3 e FFmpeg com o encoder `libvorbis`. O alvo `cdi` requer `mkdcdisc`.

```bash
source /caminho/para/kos/environ.sh
make -C dreamcast
make -C dreamcast cdi
```

O gerador em `tools/make_demo.py` cria `notes.mid` e `song.ogg` dentro de `build/romdisk`. O jogo lê MIDI padrão tipo 0 ou 1 com resolução PPQN e interpreta as notas Expert da pista de guitarra. O tempo do jogo vem da posição em amostras do canal AICA.

O R1 pontua os ataques das notas e dos acordes, com janela de ±100 ms, sequência e multiplicador. Seleção de músicas, outras dificuldades, sustains, hammer-ons e pull-offs ficam para revisões posteriores.

O código do port segue a GPL, versão 2 ou posterior. Consulte a [licença e os créditos do FoFiX](../COPYING) e o arquivo [CREDITS](../CREDITS).
