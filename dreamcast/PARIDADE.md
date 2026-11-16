# Matriz de paridade do Frets on Fire 1.3.110

Comparação de referência com o [pacote Linux de 2008](REFERENCIA_PC.md). A validação do modelo de regras usa o mesmo relógio e as mesmas transições nas duas implementações, com igualdade exata de eventos discretos. Em integração os horários efetivos de despacho e os horários do jogo são registrados separadamente.

As capturas de jogo usam 640 × 480, a mesma música e dificuldade e uma sequência que inclui omissões, erro, acorde, soltura antecipada, pausa e nova partida. Para imagem, inspecionar o mesmo ponto musical, sem esticar o quadro. Para áudio, conservar as saídas nativas e registrar qualquer alinhamento inicial. Medir descontinuidades, intervalos de quadro e recursos da máquina. Repetir no hardware antes de tratar o comportamento físico como validado.

| Recurso | Original | Port | Teste | Estado | Diferença pendente |
| --- | --- | --- | --- | --- | --- |
| Chart, dificuldades e BPM | `Song.MidiReader`, `Track.update` | `src/chart.cpp` | Os quatro charts originais, 4.606 gemas em todos os níveis contra o código Python 2 | Equivalência das fontes de teste demonstrada | MIDI de terceiros e outros metadados |
| Acerto, acorde, toque, sustain, erro e multiplicador | `Guitar.py`, `GuitarScene.py`, `Player.py` | `Round` | As 828 transições decisórias do replay Defy Amazing se igualaram no relógio de teste de 5 ms. Fixture de limite também produz 11 acertos e 660 pontos nos dois motores | Parcial | Diferenças na execução real, agenda Maple e mais fixtures |
| Contagem, precisão e estrelas | `Player.py`, `GameResultsScene.py` | `Round`, `View` | Contagem de gemas nos testes internos e registrados em integração | Parcial | Persistência e apresentação do resultado |
| Entrada Maple | `Player.Controls`, `Input.py` | `src/main.cpp` | Pressões e solturas no Flycast e adaptação de uma fila de relatórios Maple | Parcial | Pulso curto fora da cadência Maple, repetição dos menus e hardware físico |
| Menu e seleção em cassetes | `MainMenu.py`, `Dialogs.SongChooser` | `View`, `song_library` | Fluxo de Play Game, seleção e dificuldade | Parcial | Interpolação do carrossel, prévia de áudio, seleção por texto e bibliotecas |
| Partida, pausa, resultado e Replay | `GuitarScene.py`, `GameResultsScene.py` | `src/main.cpp`, `Audio`, `Round` | Música completa com Esc e nova partida | Parcial | Transições da pausa e diálogo de nome e recordes |
| Pista, modelos, sustains, HUD e palco | `Guitar.py`, `Stage.py`, `GuitarScene.py`, `data/` | `View`, `prepare_assets.py` | Comparação 640 × 480 em capturas do mesmo chart | Parcial | Iluminação, animações, fontes, caudas, reposicionamento e transições |
| Acompanhamento e guitarra | `Song.py`, `Audio.py` | `Audio` | Stems originais em jogo, verificação da mixagem nativa de referência | Parcial | Teste físico, interrupções e trilha opcional de ritmo |
| Efeitos e áudio de menus | `Data.py`, `MainMenu.py`, `GameResultsScene.py` | `SoundEffects`, `Audio` | Recursos originais lidos no build | Parcial | SFX convertidos a 22,05 kHz Yamaha ADPCM, sobreposição de efeitos, transições |
| Tutorial | `data/songs/tutorial`, `ScriptReader` | `main.cpp`, `prepare_assets.py` | MIDI verificado e carregamento preparado | Parcial | Execução integral com imagens, comandos e sincronismo do tutorial |
| Editor e importadores | `Editor.py`, `Importer.py`, `GHImporter.py` | Pendente | Pendente | Ausente | Criação, edição e importação |
| Configuração e recordes | `Settings.py`, `Config.py`, `GameResultsScene.py`, `SongInfo` | Ajuste básico A/V em memória | Navegação básica | Parcial | Perfis, controles configuráveis, ajustes de jogo, mods, recordes e persistência |
| Rede e serviços do original | `Network.py`, `Lobby.py`, serviços de placar | Pendente | Pendente | Ausente | Auditar alcance e preservar o fluxo aplicável |
| Créditos e localização | `Credits.py`, `Language.py`, `data/translations` | Pendente | Pendente | Ausente | Apresentação de créditos, idiomas e caracteres |
| Memória, quadros e interrupções | Linux original e console alvo | `Audio`, `View`, contadores de integração | Instrumentação de intervalos e recursos em Flycast | Parcial | Pico do heap, DAC e desempenho do hardware |
| Boot e teste físico | Dreamcast com teclado Maple e áudio estéreo | CDI de teste | Inicialização no Flycast | Parcial | Boot e execução completas no Dreamcast físico |

Star Power, Killswitch e falha por medidor de vida pertencem ao escopo da branch FoFiX. Os critérios desta matriz seguem o executável clássico 1.3.110.
