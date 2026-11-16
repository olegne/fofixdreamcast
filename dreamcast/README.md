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
| Star Power | Page Down |
| Killswitch durante sustains | Page Up |
| Pausar | Ctrl esquerdo ou Esc |

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

Os testes atuais cobrem guitarra solo no Flycast. O FoF Tutorial em Expert teve 59 alvos concluídos usando o teclado Maple emulado e alternando Enter e Shift direito. O perfil de teste também exercitou acordes de cinco casas, HO/PO, pausa, Star Power e Killswitch. A comparação das três dificuldades do tutorial com o leitor do FoFiX de PC confirmou o começo, a duração e a classificação HO/PO dos 189 eventos de nota.

Ainda estão em desenvolvimento os modos com outros instrumentos, o multiplayer e a compatibilidade com todos os tipos de chart, efeitos de palco e regras da versão de PC. A validação em Dreamcast físico também está pendente.

O código do port segue a GPL, versão 2 ou posterior. As atribuições ficam na [licença](../COPYING), nos [créditos do FoFiX](../CREDITS) e nos [créditos do tema](../data/themes/MegaLight%20V4/CREDITS).
