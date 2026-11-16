# Matriz de paridade do Frets on Fire 1.3.110

Comparação de referência com o [pacote Linux de 2008](REFERENCIA_PC.md). A validação do modelo de regras usa o mesmo relógio e as mesmas transições nas duas implementações, com igualdade exata de eventos discretos. Em integração os horários efetivos de despacho e os horários do jogo são registrados separadamente.

As capturas de jogo usam 640 × 480, a mesma música e dificuldade e uma sequência que inclui omissões, erro, acorde, soltura antecipada, pausa e nova partida. Para imagem, inspecionar o mesmo ponto musical, sem esticar o quadro. Para áudio, conservar as saídas nativas e registrar qualquer alinhamento inicial. Medir descontinuidades, intervalos de quadro e recursos da máquina. Repetir no hardware antes de tratar o comportamento físico como validado.

| Recurso | Original | Port | Teste | Estado | Diferença pendente |
| --- | --- | --- | --- | --- | --- |
| Chart, dificuldades e BPM | `Song.MidiReader`, `Track.update` | `src/chart.cpp` | Os quatro charts originais, 4.606 gemas em todos os níveis contra o código Python 2 | Equivalência das fontes de teste demonstrada | MIDI de terceiros e outros metadados |
| Acerto, acorde, toque, sustain, erro e multiplicador | `Guitar.py`, `GuitarScene.py`, `Player.py` | `Round` | As 828 transições decisórias do replay Defy Amazing se igualaram no relógio de teste de 5 ms. Fixture de limite também produz 11 acertos e 660 pontos nos dois motores | Parcial | Diferenças na execução real, agenda Maple e mais fixtures |
| Contagem, precisão e estrelas | `Player.py`, `GameResultsScene.py` | `Round`, `View` | Contagem de gemas nos testes internos e registrados em integração | Parcial | Persistência e apresentação do resultado |
| Entrada Maple | `Player.Controls`, `Input.py` | `src/main.cpp` | Pulsos de 2 VBlanks enviados pelo caminho SDL → Maple em todos os estados, incluindo Esc, Enter e seta para reiniciar | Parcial | Pulso fora da cadência Maple, repetição e navegação múltipla entre quadros nos menus e hardware físico |
| Menu e seleção em cassetes | `MainMenu.py`, `Dialogs.SongChooser` | `View`, `song_library` | Fluxo de Play Game, seleção e dificuldade | Parcial | Interpolação do carrossel, prévia de áudio, seleção por texto e bibliotecas |
| Contagem e pausa com teclado | `GuitarScene.py`, `Menu.py`, `View.py` | `main.cpp`, `View`, `Audio` | PC nativo e CDI no Flycast. Esc/Enter/Esc durante a contagem e na faixa, reinício e faixa iniciada com o menu aberto | Validado no emulador com Defy Amazing | Rotação do texto na transição, ensaio de atraso do chart no Dreamcast, teste físico |
| Partida, resultado e Replay | `GuitarScene.py`, `GameResultsScene.py` | `src/main.cpp`, `Audio`, `Round` | Música completa com Esc e nova partida | Parcial | Diálogo de nome e recordes |
| Pista, modelos, sustains, HUD e palco | `Guitar.py`, `Stage.py`, `GuitarScene.py`, `data/` | `View`, `prepare_assets.py` | Comparação 640 × 480 no mesmo chart, incluindo o aviso 2×. Histórico de palhetadas alimenta as luzes | Parcial | Fase da iluminação em mudanças de BPM, caudas, fontes, reposicionamento e transições |
| Acompanhamento e guitarra | `Song.py`, `Audio.py` | `Audio` | PCM capturado em duas etapas no SDL. Zero faltas de amostras em jogo na candidata, com `aica.BufferSize=6144` | Parcial | Medir a saída do console e latência, adicionar trilha opcional de ritmo |
| Efeitos e áudio de menus | `Data.py`, `MainMenu.py`, `GameResultsScene.py` | `SoundEffects`, `Audio` | Recursos originais lidos no build | Parcial | Preservar a duração de `jurgen1`, `jurgen4` e `jurgen5`, conversão a 22,05 kHz Yamaha ADPCM, sobreposição e transições |
| Tutorial | `data/songs/tutorial`, `ScriptReader` | `main.cpp`, `prepare_assets.py` | MIDI verificado e carregamento preparado | Parcial | Execução integral com imagens, comandos e sincronismo do tutorial |
| Editor e importadores | `Editor.py`, `Importer.py`, `GHImporter.py` | Pendente | Pendente | Ausente | Criação, edição e importação |
| Configuração e recordes | `Settings.py`, `Config.py`, `GameResultsScene.py`, `SongInfo` | Ajuste básico A/V em memória | Navegação básica | Parcial | Perfis, controles configuráveis, ajustes de jogo, mods, recordes e persistência |
| Rede e serviços do original | `Network.py`, `Lobby.py`, serviços de placar | Pendente | Pendente | Ausente | Auditar alcance e preservar o fluxo aplicável |
| Créditos e localização | `Credits.py`, `Language.py`, `data/translations` | Pendente | Pendente | Ausente | Apresentação de créditos, idiomas e caracteres |
| Memória, quadros e interrupções | Linux original e console alvo | `Audio`, `View`, contadores de integração | Instrumentação de intervalos e recursos em Flycast | Parcial | Pico do heap, DAC e desempenho do hardware |
| Boot e teste físico | Dreamcast com teclado Maple e áudio estéreo | CDI de teste | Inicialização no Flycast | Parcial | Boot e execução completas no Dreamcast físico |

Star Power, Killswitch e falha por medidor de vida pertencem ao escopo da branch FoFiX. Os critérios desta matriz seguem o executável clássico 1.3.110.
