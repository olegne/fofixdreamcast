# Referência Linux do Frets on Fire clássico

| Item | Configuração |
| --- | --- |
| Jogo | Frets on Fire 1.3.110, Unreal Voodoo, pacote Linux oficial de 2008 |
| Arquivo | `FretsOnFire-1.3.110.tar.gz` |
| SHA-256 | `c8cca75a897a8c4cadac06f92fd85b417ac41fc19fc719e89ddcd91a3c29f9da` |
| Git legado relacionado | `fretsonfire/FretsOnFire@1017399a4b6fa659a90e2806e2d2f729e5cdc07d` |
| Recursos | Visual e palco padrão do pacote. Resolução 640 × 480, 4:3 |
| Áudio | 44,1 kHz estéreo, 16 bits. Ganhos padrão de acompanhamento e guitarra iguais a 1,0 |
| Teclado e tempo | F1 a F5, Enter/Shift direito, setas, Esc. Atraso A/V padrão de 100 ms |
| Referência de partida | Defy The Machine, Amazing, arquivos `notes.mid`, `song.ogg` e `guitar.ogg` do pacote |

O arquivo anexado coincide com o pacote oficial. Seu código e recursos são a referência exata. O Git legado também se identifica como 1.3.110 e contém ajustes em arquivos como `src/Audio.py`, `src/GameEngine.py` e `src/Dialogs.py`. [Lançamento oficial](https://sourceforge.net/projects/fretsonfire/files/fretsonfire/1.3.110/) · [Git legado](https://github.com/fretsonfire/FretsOnFire/tree/1017399a4b6fa659a90e2806e2d2f729e5cdc07d)

## Execução observada

CPython 2.7.18, Pygame 1.9.6, NumPy 1.16.6, Pillow 6.2.2, PyOpenGL 3.1.5, SDL_mixer 1.2.12 e libvorbisfile. Pygame usa a interface SDL 1.2 de sdl12-compat 1.2.68. OpenGL em Mesa 25.0.7 com perfil Compatibilidade 4.5. A execução usa um perfil limpo, sem compartilhamento de recordes pela internet.

A camada de compatibilidade expõe os nomes antigos `Image`, `PngImagePlugin`, `tostring` e `fromstring` através das funções correspondentes do Pillow. A versão oficial usa seu caminho PNG para os SVGs. Os arquivos do jogo foram preservados na cópia original. O driver de disco do SDL gravou a saída real do mixer.

Em uma medição isolada, a saída capturada ao reproduzir Defy no mixer do jogo obteve correlação acima de 0,999999969 e erro RMS de até 1,15 níveis PCM em quatro janelas de meio segundo. As janelas incluem os dois stems em 1,0, a guitarra silenciada e a volta de uma pausa. O relógio `mixer.music.get_pos()` apresenta um salto transitório ao retomar, antes do próximo callback. O PCM confirma a retomada no cursor original. O replay aguarda a estabilização desse relógio na retomada.

## Contagem e pausa

No pacote oficial executado, Esc abriu o menu durante a contagem de oito batidas. A cena continuou avançando e iniciou as duas trilhas ao chegar a zero com o menu aberto. Durante a faixa, Esc pausou as duas trilhas. Enter em **Return to Song** acionou a confirmação e manteve o menu aberto. Um segundo Esc iniciou a transição de 512 ms do código original. O callback do menu retomou o áudio quando a camada desapareceu.

O cursor da pista durante a contagem equivale ao cursor de áudio menos as batidas restantes convertidas em milissegundos, menos o atraso A/V. A captura usou os 100 ms padrão. Em uma cópia isolada da instalação com `delay = 75` no `song.ini` de Defy, a primeira contagem usou 175 ms e o fechamento da pausa recarregou os 100 ms globais. O MIDI, as trilhas e os fontes permaneceram os do pacote. A regra vem de `GuitarSceneClient.songLoaded`, `getSongPosition` e `resumeGame`.

## Aparência histórica

As sete capturas oficiais de 640 × 480 mostram o logotipo em chamas, os cassetes, as dificuldades, os refletores, os trastes, o placar, o editor e o resultado. A quarta imagem, de uma partida de guitarra com palco verde, está preservada desde 2007. A revisão interna dessas imagens é desconhecida. [Galeria oficial](https://fretsonfire.sourceforge.net/screenshots/) · [Arquivo da captura de jogo](https://commons.wikimedia.org/wiki/File:Fretsonfire4.png)

O vídeo oficial de 720 × 576 contém o menu em 00:00, a seleção em 01:16, guitarra em 01:40 e 01:52 e o resultado em 02:50. As cenas jogadas usam fundo vinho liso e a compilação permanece indeterminada. As capturas da execução Linux acima definem os elementos da versão escolhida para o port. [Vídeo oficial arquivado](https://commons.wikimedia.org/wiki/File:Fretsonfire-demo.ogv)
