# FoFiX no Dreamcast

Port nativo em desenvolvimento. A referência é o FoFiX de PC no commit `f7e4e403`, com o tema padrão MegaLight V4. A compilação de desenvolvimento inclui **FoF Tutorial**, com as duas trilhas originais de 44,1 kHz, e **Circuito 01**, usada em testes do port.

## Teclado

O teclado Maple é a entrada principal. No Flycast, selecione **Keyboard** como dispositivo da porta A.

| Ação | Tecla |
| --- | --- |
| Navegar | Setas. Enter confirma e Esc volta |
| Cinco casas, da esquerda para a direita | F1 a F5 |
| Teclas alternativas para as cinco casas | F6 a F10 |
| Palhetar | Enter ou Shift direito |
| Star Power | Page Down ou Esc durante a partida |
| Killswitch durante sustains | Page Up |
| Pausar | Ctrl esquerdo. Escolha Resume para retomar após a contagem de três segundos |

O menu Settings permite ajustar o atraso de áudio/vídeo e a velocidade da pista. Um atraso positivo faz as notas chegarem mais tarde.

## Compilar

Use KallistiOS 2.2.1 com `libvorbis` e `libogg` do `kos-ports`. O conversor de recursos usa Python 3, Pillow, pycollada e FFmpeg com `libvorbis`. O alvo `cdi` usa `mkdcdisc`.

```bash
source /caminho/para/kos/environ.sh
python3 -m pip install Pillow pycollada
make -C dreamcast
make -C dreamcast cdi SONGS_DIR=/caminho/para/songs
```

`SONGS_DIR` é opcional. Cada música usa `notes.mid`, `song.ini` e `song.ogg` ou `guitar.ogg`. Quando os dois Ogg existem, eles devem ter a mesma taxa de amostragem. O jogo lê notas de guitarra MIDI tipo 0 ou 1 com PPQN e usa a posição em amostras da AICA como relógio. Os recursos do tema são convertidos para PowerVR a partir de `data/`.

## Validação

Os testes cobrem guitarra solo no Flycast com entrada pelo teclado Maple emulado. O tutorial em Expert teve 59 alvos concluídos em teste anterior. A faixa completa Defy The Machine já passa pelo fluxo até o resultado e inicia outra partida. Existem diferenças de regras, entrada sob carga, mixagem interativa e apresentação que impedem a aprovação da paridade.

A [referência de PC](REFERENCIA_PC.md) documenta o MegaLight V4 e as bibliotecas usadas. A [matriz de paridade](PARIDADE.md) registra os recursos, os testes e as diferenças pendentes. O [checkpoint](CHECKPOINT.md) vincula os resultados aos binários testados e descreve a próxima ação.

O código do port segue a GPL, versão 2 ou posterior. As atribuições ficam na [licença](../COPYING), nos [créditos do FoFiX](../CREDITS) e nos [créditos do tema](../data/themes/MegaLight%20V4/CREDITS).
