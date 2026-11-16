# Paridade entre FoFiX e Dreamcast

A referência é o FoFiX de PC com o MegaLight V4. A primeira etapa funcional cobre o fluxo completo de guitarra solo com teclado Maple. Os demais recursos continuam no escopo.

## Critérios de validação

| Área | Critério |
| --- | --- |
| Referência de PC | Identificar commit, árvore de recursos, configuração efetiva, origem de cada mídia e bibliotecas carregadas. Confirmar caminhos e fallbacks do tema em uma instalação limpa. |
| Regras | Comparar a ordem dos eventos, alvos, erros, pontuação, sequência, multiplicador, estrelas e resultado. Usar igualdade exata para medidas discretas sob as mesmas condições. Comparar entradas na linha de tempo do jogo e registrar a resolução temporal de cada plataforma. |
| Entrada | Injetar pressionamentos e solturas na entrada usada pelo jogador. Verificar acordes, palhetas alternadas, tecla mantida, soltura de sustain, pausa e navegação. No Flycast, configurar um teclado Maple. |
| Imagem | Capturar os dois lados em 640 × 480, com o mesmo chart, modo, dificuldade e posição musical. Registrar a diferença temporal das capturas e usar sobreposição sem redimensionar o quadro. Inspecionar movimento, animações e transições em gravação contínua. |
| Áudio | Reter o PCM de cada execução. Registrar o instante das entradas e das mudanças de ganho, a duração, a velocidade, a afinação, os canais e os cortes. Se houver alinhamento para a análise, guardar e identificar as trilhas originais e informar o deslocamento aplicado. |
| Tempo e carga | Medir intervalo entre quadros, distribuição dos tempos, pior intervalo, interrupções de áudio e uso de RAM, VRAM e AICA. Registrar separadamente as condições do emulador e do console. |
| Artefato | Anotar commit, configuração de compilação, mídia e hashes de ELF e CDI. Testar a própria CDI com os mesmos recursos e guardar o log. |

Uma rodada comparável inclui erros deliberados, limites das janelas, mudança de BPM, sustain interrompido, pausa e uma música completa. A constatação de um desvio perceptível mantém o item em aberto, inclusive quando a diferença decorre da plataforma.

## Matriz

Os resultados abaixo identificam a evidência disponível a partir do checkpoint `20113e1`. Estado e resultado distinguem a existência de uma implementação da equivalência com o PC.

| Recurso | Referência original | Implementação no port | Teste realizado | Resultado | Diferença pendente |
| --- | --- | --- | --- | --- | --- |
| Teclas da guitarra Maple | `defaultg.ini`, `core/Input.py`, `core/Player.py` | `src/main.cpp` | Fixture via Maple no Flycast com acorde de cinco casas, Enter mantido, Shift direito, teclas alternativas, Ctrl esquerdo, Esc e Page Up | Validado no emulador | Eventos em trechos densos, atalhos de navegação alternativos e console físico |
| Menu, perfil, setlist e dificuldade | `MainMenu.py`, `Lobby.py`, `SongChoosingScene.py`, `Dialogs.py` | `src/main.cpp`, `src/view.cpp`, `src/song_library.cpp` | Seleção até Expert em Defy no PC e no Flycast, a partir dos menus | Parcial | Carrossel, perfil, preview, cancelamento e visual das telas |
| MIDI e BPM | `game/song/song.py`, classes `MidiReader`, `TempoTrack` e `Track` | `src/chart.cpp` | Exportação das 662 gemas e 614 alvos de Defy Expert com tempos e durações a menos de 1 ms do PC por arredondamento. Circuito 01 tem dois tempos | Parcial | Metadados e marcadores. Comparar mais dificuldades e charts com mudanças de BPM |
| Palhetada, acerto, acorde e HOPO | `GuitarScene.py`, `instruments/Guitar.py`, `Track.markHopo` | `Round` em `src/chart.cpp` | 384 gemas com marca de HOPO coincidem em Defy Expert. Replay completo diverge nos acertos. Duplicação do tutorial reproduzida no PC e isolada a eventos no mesmo quadro | Parcial | Comparar decisões de acerto com timestamps de entrada Maple e testar limites. O port mantém um acerto por alvo na condição do HOPO duplicado |
| Sustains, miss e overstrum | `GuitarScene.endPick`, `screwUp`, `Instrument.getMissedNotes` | `Round` | Replay com sustains, soltura deliberada, palhetada errada e notas sem palhetada em Defy | Parcial | Sequência exata das penalidades, pontuação de soltura, efeitos e precedência de erros |
| Pontuação, estrelas, derrota e SP | `Scorekeeper.py`, `GuitarScene.py`, `instruments/Instrument.py` | `Round`, `View::draw_rockmeter` | Resultado completo em ambas as versões. Fixture Maple com Esc após duas frases e 1.051 pontos | Parcial | Pontuação divergente em Defy, confirmação de derrota e marcadores variados do chart |
| Killswitch e pitch bend | `GuitarScene`, `song.setInstrumentVolume`, `setInstrumentPitch` | `main.cpp`, `Audio::guitar_gain` | Page Up sobre sustain com stems separados | Parcial | Duração mínima, recarga, resposta audiovisual e modo pitch bend |
| Pausa, retomada, resultado e nova partida | `GuitarScene.pauseGame`, `resumeGame`, `GameResultsScene.py` | `src/main.cpp`, `Audio::pause/resume` | Contagem de três segundos e mesmo cursor AICA em Defy e na fixture. Chegada ao resultado e replay em ambas as versões | Parcial | Opções e desenho do menu de pausa, contagem durante pausa inicial e casos de fim de partida |
| Pista, trastes, notas 3D e sustains | `Theme.py`, `Neck.py`, `Instrument.py`, `MegaLight V4` | `src/view.cpp`, `tools/prepare_assets.py` | Capturas 640 × 480 e gravação contínua de Defy | Parcial | Brilho e orientação dos modelos, transparência, limites da pista, animação e sustentação |
| Palco, efeitos, HUD e fontes | `Stage.py`, `Rockmeter.py`, `rockmeter.ini`, fundos e fontes do tema | `src/view.cpp`, `tools/prepare_assets.py` | Tema e fallbacks auditados no PC. Diferenças visíveis lado a lado em Defy | Parcial | Luz de palco, chamas, relógio, estrelas e posição/estilo do texto |
| Streaming e mixagem | `song.Song`, `fretwork.audio.StreamingSound`, `MixStream.c` | `src/audio.cpp` | Capturas nativas em estéreo e dois Ogg durante Defy integral. Trechos estáveis batem com as amostras de origem. O retorno de pausa mantém o cursor | Parcial | Interrupções na captura do Flycast, diferença de ganho, início e fim em charts com stems distintos |
| Efeitos sonoros e mixagem interativa | `Data.py`, `GuitarScene.py`, `game/song/song.py` | `SoundEffects`, `Audio` | Medição dos stems em notas acertadas, erros e soltura antecipada | Parcial | Tempos das transições, efeitos de miss e Star Power, Killswitch digital e pitch bend |
| Biblioteca, charts e recursos das músicas | `game/song/song.py`, configuração e arquivos de cada música | `src/song_library.cpp`, `src/chart.cpp`, `Makefile` | Tutorial incluído e fixture na ISO | Parcial | Previews, múltiplas trilhas e instrumentos, vídeos, letras e metadados |
| Baixo, ritmo e baixo solo | `instruments/Guitar.py`, `BASS_PART`, `RHYTHM_PART` | Guitarra principal | Pendente | Ausente | Track, áudio, multiplicador, assets e fluxo |
| Bateria e variantes | `instruments/Drum.py`, `DRUM_PART`, `PRO_DRUM_PART` | Pendente | Pendente | Ausente | Charts, pads, fills, áudio e resultado |
| Vocais e microfone | `instruments/Vocalist.py`, `VocalTrack`, `core/Microphone.py` | Pendente | Pendente | Ausente | Dispositivo, tom, letras e regras |
| Multiplayer, cooperativo, frente a frente e batalha | `World.py`, `Lobby.py`, `GuitarScene.py` | Pendente | Pendente | Ausente | Renderização, sincronização, regras, entrada por jogador |
| Training, prática, carreira, Jurgen e editor | `MainMenu.py`, `Dialogs.py`, `SongChoosingScene.py`, `GuitarScene.py` | Entrada de Training reaproveita o setlist | Pendente | Parcial | Seleção de trecho, velocidade, carreira, automação e editor conforme o original |
| Configuração, opções especiais e persistência | `core/ConfigDefs.py`, `Settings.py`, `core/Player.py`, `SongInfo` | Atraso A/V e velocidade durante a execução | Verificação da navegação anterior | Parcial | Opções, VMU, perfis e recordes |
| Funcionamento e desempenho no console | Execução da referência mais critérios de temporização acima | CDI KallistiOS, PowerVR e AICA | Música completa em CDI no Flycast com trechos densos e pausa. Replay executado | Parcial | Tempos por quadro, memória e origem dos cortes. Teste em Dreamcast físico |

`Implementado, ainda não validado` e `Validado no console` também são estados da matriz quando houver evidência correspondente.

## Auditoria da referência

Veja a [auditoria da referência de PC](REFERENCIA_PC.md) e o [checkpoint de validação](CHECKPOINT.md).
