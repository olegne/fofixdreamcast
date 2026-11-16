# Checkpoint do Frets on Fire clássico

## SH4ZAM, leitura de CD, teclado e apresentação

A candidata parte de `62374a2e4f1136136ea9c8d051ce79b3500e9419` de `dreamcast-classic`. Usa o mesmo pacote Linux Frets on Fire 1.3.110 e os mesmos charts. A verificação da própria CDI confirmou os 26 arquivos nas quatro pastas de músicas.

| Arquivo da candidata de diagnóstico | SHA-256 |
| --- | --- |
| FretsOnFire_Dreamcast.elf | `6409775599d67a527db814f27315a9b6769d747ce77618244c7750962c92eb7e` |
| FretsOnFire_Dreamcast.cdi | `8d58e42aabfcc7dde779a8756d830215ede3f2df02847302466b4477f7c040ab` |
| Plano de teclado em Defy Amazing | `44045f428d1db20343ae31c782b9b5c9729838d8bccb155799a064374a3af14a` |

KallistiOS 2.2.1, GCC 15.1.0 SH-4, SH4ZAM 1.0.0 da toolchain, libvorbis e libogg do kos-ports. Para a compilação de diagnóstico, o comando do [README](README.md) recebeu `CXXFLAGS="-std=c++17 -Wall -Wextra -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. A instrumentação guarda relatórios e julgamentos em RAM, sem imprimir no caminho de cada palhetada. A automação lê esse diagnóstico e envia teclado por SDL e Maple.

### Desempenho

A trigonometria de desenho usa a instrução FSCA pelo SH4ZAM. Os trastes têm vértices preparados para repouso e tecla pressionada. As malhas de notas reutilizam a projeção das posições compartilhadas sem alterar a ordem de seus vértices nem a cor por face. A geometria vai em lotes e por Store Queues, com envio de novo cabeçalho apenas quando muda o material ou a lista. A rotina de cache e os limites de lote seguem as técnicas usadas nos Test15 e Test17 de Super Mario World. [SH4ZAM](https://sh4zam.com/guides/getting-started/) · [Test15](https://github.com/olegne/dcioram-bros/blob/2ebe34099ec6fc6382af01b19bb59a899c901344/docs/DESEMPENHO_TEST15.md) · [Test17](https://github.com/olegne/dcioram-bros/blob/5f5e52d29727aa2e1e8df8d887f3f75879a933f2/docs/DESEMPENHO_TEST17.md)

| Ensaio isolado de 1.200 quadros em Flycast 2.7 | Antes | SH4ZAM, lotes e cache |
| --- | ---: | ---: |
| Submissão média do renderizador | 20.102 µs | 16.942 µs |
| Submissão p95 | 26.026 µs | 19.962 µs |
| Submissão máxima | 28.402 µs | 22.466 µs |
| Espera média medida em `pvr_wait_ready` | 680 µs | 1.661 µs |

Esse ensaio compara os algoritmos de submissão no mesmo chart e estado visual, com áudio e interface fora da simulação. O mesmo quadro comparativo teve diferença de até 2 em 255 por canal, com 1.620 de 307.200 pixels afetados. As alterações de aparência dos sustains e do resultado têm ensaios próprios. A cadência da partida completa e o desempenho no console têm medições próprias.

Um buffer de leitura de 16 KiB, alinhado a 32 bytes, permite que o ISO9660 do KallistiOS leia vários setores de uma vez por DMA. No ensaio isolado de 15 segundos com os dois stems de Defy, os comandos de CD durante a decodificação foram de 436 para 108. Os hashes do PCM decodificado foram idênticos. Os dois buffers usam 32 KiB enquanto a música está aberta. A preparação e o preenchimento inicial da AICA foram movidos para antes das oito batidas, mantendo a hora exata da entrada da faixa.

### Comportamento reproduzido

- O teclado observa todos os relatórios `GETCOND` Maple completos, incluindo o Shift direito, e associa cada evento ao instante amostrado da AICA. O piloto de 36 segundos recebeu 622/622 transições e diagnosticou 170/170 alvos. As rotinas originais do Linux receberam esse traço Maple e o mesmo relógio. O resultado foi de 170 alvos, 173 gemas e 11.624 pontos nos dois motores, com pontuação idêntica nas 1.305 atualizações.
- No teste de pausa com o mesmo cursor de 7.821 ms, soltar as casas encerrou o sustain e mudou a pontuação de 550 para 633 nas duas implementações. A tecla F5 pressionada enquanto o menu estava aberto permaneceu fora do estado da guitarra.
- A forma de onda do sustain segue os passos adaptativos e os fins de nota por casa. As sete fixtures tiveram contagens de segmentos iguais ao código original e diferença de até 0,0088 pixel em uma projeção comum. As notas acertadas antecipadamente continuam visíveis até cruzarem a linha.
- A tela de resultado tem placar flamejado, entrada das estrelas em sequência, estatísticas e menu de ações à parte. O primeiro Enter abre as ações e o segundo confirma Replay. Diálogo de nome, recordes, contraste e parte da tipografia estão pendentes.
- Os nove efeitos de resultado são os Ogg estéreo originais de 44,1 kHz, com playback agendado após cinco segundos. O caminho usa RAM e canais AICA durante a reprodução. Um ensaio de pontuação zero confirmou a fala `jurgen1` de 5,5 segundos, incluindo áudio depois do limite anterior de 65.534 frames e coexistência com a música de menu e com a faixa da reprise. Os efeitos curtos ainda usam a conversão ADPCM de 22,05 kHz.

### Partida integral e comparação das regras

Foram gravadas duas partidas independentes de Defy The Machine em Amazing, com áudio das próprias execuções. A candidata no Flycast 2.7 usou fila SDL de 6.144 quadros de áudio, captura a 20 fps, renderização sem thread separada e afinidade de CPU 5–8. As duas sessões enviaram as 2.256 transições. O port registrou todas via Maple, com zero estouros de fila.

| Medida | Linux ao vivo | Dreamcast em Flycast | Linux com os relatórios e o relógio reais do Dreamcast |
| --- | ---: | ---: | ---: |
| Alvos acertados | 609 / 614 | 611 / 614 | 611 / 614 |
| Gemas acertadas | 656 / 662 | 658 / 662 | 658 / 662 |
| Pontuação | 99.480 | 100.735 | 100.735 |
| Estrelas | 5 | 5 | 5 |

No teste com o mesmo relógio, os 611 IDs dos alvos e o placar coincidiram em todas as 9.092 atualizações verificadas. Nas execuções independentes, os dois alvos distintos estavam em 155.409,748 ms e 155.532,699 ms. O PC entregou as palhetadas aos 155.723 e 155.725 ms, além da janela tardia de aproximadamente 140,5 ms. O Dreamcast julgou os alvos em 155.479 e 155.545 ms. O segundo acerto adiantado em relação à palheta veio de HOPO.

A candidata completou pausa, retomada, resultado e outra partida. O cursor da pausa ficou em 12.178 ms. O efeito original `myhero.ogg`, de dois segundos, foi reproduzido na tela de resultado. O log registrou 9.091 quadros em jogo, 2.939 intervalos acima de 25 ms, 425 acima de 50 ms e maior intervalo de 111,129 ms. A leitura pontual em jogo foi de 745.616 bytes no heap, 840.168 bytes livres de VRAM e 1.333.248 bytes livres de AICA. A compilação anterior mediu 5.811 quadros e 1.221 intervalos acima de 50 ms em outra captura. A comparação isolada do renderizador está na seção Desempenho.

O backend de áudio inseriu 83 blocos de silêncio durante jogo ativo, sendo sete antes e 76 depois da pausa. O maior grupo contínuo em jogo ativo durou 69,66 ms. A sonda conferiu byte a byte os 8.583.168 quadros válidos no callback contra o PCM que entrou na fila. O conjunto de provas preserva a saída SDL original e a latência inicial A/V de 263,14 ms medida no Flycast daquela gravação. O áudio no Dreamcast físico, os efeitos curtos e as diferenças de apresentação listadas na [matriz](PARIDADE.md) seguem pendentes.

### Repetição com renderização em thread

A mesma CDI, o mesmo plano de teclado e os mesmos 6.144 quadros de áudio no SDL foram usados numa repetição completa com `rend.ThreadedRendering=yes` e sem fixação de CPUs. O backend registrou 14 faltas de amostras na inicialização e zero do início da música até a reprise. Os 8.590.336 quadros válidos do callback coincidiram byte a byte com a entrada da fila. A mediana da entrega do PCM ao callback SDL foi de 160,38 ms. A mudança de configuração inclui dois fatores do host, e a latência do Dreamcast físico é uma medição pendente.

O Flycast acertou 611 alvos e 658 gemas e fez 99.933 pontos e cinco estrelas nessa execução. As rotinas originais do Linux produziram os mesmos 611 IDs, gemas e pontos com os relatórios Maple e o relógio desta sessão. O placar coincidiu em 9.094 de 9.094 atualizações. O jogo registrou 9.093 quadros, 2.964 intervalos acima de 25 ms e 431 acima de 50 ms, com máximo de 109,258 ms. A nova gravação lado a lado usa esta sessão Dreamcast, com áudio original do SDL e atraso A/V inicial medido de 144,92 ms, e a sessão Linux ao vivo da tabela anterior. O arquivo de provas registra a configuração e os resultados das duas execuções Dreamcast.

## Histórico até o checkpoint anterior

### Contagem, teclado e efeitos de palco

A etapa usa o mesmo pacote Linux 1.3.110 e parte do commit `4c484f10902f1e91cd0f47c3568c23439ac1dba9` de `dreamcast-classic`.

| Arquivo da candidata testada | SHA-256 |
| --- | --- |
| FretsOnFire_Dreamcast.elf | `d7d89517872d8cfc6291a68841a4143c88aee9ac092459f317fc327060dcf5c1` |
| FretsOnFire_Dreamcast.cdi | `c95db28083c4c36dc8173ccb009ed70006ba847b1ceb31df90b07a66417fd7f8` |

A compilação usa KallistiOS 2.2.1, GCC 15.1.0 SH-4, os mesmos recursos e o [comando do README](README.md). A leitura da própria CDI voltou a verificar os 26 arquivos das quatro pastas originais de músicas no Joliet.

- A fila Maple agora acompanha teclado durante a contagem, a pausa e os menus. No teste, Escape, Enter e a seta de navegação foram mantidos por dois VBlanks. Os mesmos relatórios continuam alimentando as casas e a palheta durante a partida. O ganho de guitarra acompanha as transições de acerto e falha lidas em ordem.
- A pista aplica o atraso A/V durante a contagem e na entrada da faixa. A contagem avança por baixo do menu de pausa e inicia a faixa ao vencer. O fechamento anima a camada no intervalo de 512 ms do original. A rotação das palavras no menu ainda falta. Os metadados de atraso são recarregados ao fechar a pausa conforme a [referência](REFERENCIA_PC.md). O ensaio com atraso adicional foi feito no PC.
- Em Defy Amazing no Flycast, Esc durante a contagem abriu com 7,692 batidas restantes. Enter manteve a pausa aberta. O segundo Esc fechou em 4,500 batidas. Na faixa, Esc/Enter/Esc manteve o cursor de áudio em 1.361 ms até a retomada. Em outra contagem, a música iniciou com o menu aberto. A captura observou a posição de chart de 857 ms sob o menu. Após Esc, a primeira leitura em controle de jogo foi 1.474 ms de chart, com o áudio ainda avançando.
- O palco usa o histórico de casas das quatro palhetadas mais recentes, uma janela de batidas e os recursos originais para as chamas de 2×, 3× e 4×. A fase das luzes ainda pode divergir em mudanças de BPM e o material da pista, as caudas e a animação dos resultados seguem na matriz.

#### Diagnóstico de cortes de áudio

O Flycast 2.7 executou a CDI anterior (`f58e4d4f…`) em um replay integral de Defy com captura na entrada e na saída do backend SDL2. Sem vídeo contínuo, os 8.309.760 frames válidos do callback ficaram idênticos byte a byte à entrada da fila. O backend inseriu 36 blocos de silêncio de 1.024 frames em jogo, somando 835,92 ms. Dois grupos contínuos de 69,66 ms foram reproduzidos integralmente pela falta de amostras na fila. O silêncio da pausa já estava presente no PCM antes da fila. A deriva de 812,70 ms depois da pausa correspondeu exatamente a 35 dos blocos zerados.

A sonda identifica a rotina real do backend que preenche os blocos com zeros, além de guardar o PCM original. O resultado localiza o corte reproduzido no Flycast. A saída do Dreamcast físico, a latência de cada plataforma e a paridade global da mixagem continuam pendentes. [Código SDL do Flycast 2.7](https://github.com/flyinghead/flycast/blob/v2.7/core/audio/audiobackend_sdl2.cpp)

Na inicialização, o KallistiOS aceita carregar `jurgen1.wav`, `jurgen4.wav` e `jurgen5.wav` e avisa que excedem o limite de 65.534 amostras. A rotina `snd_sfx_play_ex` do KallistiOS limita a reprodução às primeiras 65.534 amostras. Os três efeitos de resultado serão migrados para um caminho que reproduza a duração integral, com teste a partir de uma partida de baixa pontuação.

#### Música completa com a candidata

*Defy The Machine* em Amazing, resolução 640 × 480 e atraso A/V em 100 ms no jogo. Na CDI identificada acima, o Flycast 2.7 usou captura a 20 fps, llvmpipe, áudio estéreo de 44.100 Hz e `config:aica.BufferSize=6144` para testar a margem do backend SDL2. A mesma sequência planejada e identificada na etapa anterior enviou os 2.256 eventos de teclado. A fila Maple registrou zero estouros. A faixa avançou até o resultado e o replay iniciou nova partida.

| Medida | Linux 1.3.110 | Dreamcast no Flycast |
| --- | ---: | ---: |
| Alvos acertados | 609 / 614 | 610 / 614 |
| Gemas acertadas | 656 / 662 | 657 / 662 |
| Pontuação | 99.300 | 101.033 |
| Estrelas | 5 | 5 |

Os horários efetivos das teclas ainda divergem. A amostragem de estado por VBlank perde a identificação individual de dois alvos quando há múltiplos acertos entre leituras. A comparação de pontuação permanece aberta até reconciliar esses eventos e as janelas de entrada com o relógio do jogo nas duas plataformas.

A sonda registrou zero eventos de falta de amostras durante a partida completa com a fila maior e 11 no boot. Todos os 8.329.216 frames válidos do callback eram idênticos aos da entrada da fila. O silêncio da pausa e 69,66 ms imediatamente no início da partida estavam presentes antes da fila. O atraso mediano entre a submissão ao backend e o callback foi de 148,30 ms, contra 63,90 ms no diagnóstico com a fila anterior sem vídeo contínuo. Essa diferença entre ensaios não representa a latência de saída do console. O arquivo de captura e a montagem preservam os cortes, os relógios e as saídas originais. A montagem usa preenchimento inicial documentado, com o atraso A/V medido do Flycast preservado.

Durante a música, o cursor ficou em 12.121 ms na pausa e retomou no mesmo ponto. O port registrou 5.811 quadros, 2.407 intervalos acima de 25 ms e 1.221 acima de 50 ms, com máximo de 121,614 ms na captura instrumentada. No início da faixa, o heap alocado era de 570.040 bytes. A VRAM livre era de 709.096 bytes e a memória AICA livre de 827.392 bytes. Os maiores intervalos seguem pendentes de verificação sem a carga de captura e em console.

O aviso 2× aparece nas duas plataformas. A diferença de sequência faz a chama surgir mais cedo na execução do Dreamcast. A captura no mesmo instante musical e a captura em idades comparáveis do aviso registram essa diferença.

### Histórico do checkpoint anterior

#### Proveniência

Branch `dreamcast-classic` do fork `olegne/fofixdreamcast`, criada a partir do checkpoint `fe782a43259abab95525742fc9513c03a100ae8e`. O alvo é o Frets on Fire 1.3.110 do pacote Linux da Unreal Voodoo. A [referência de PC](REFERENCIA_PC.md) registra a identidade do arquivo anexado e a configuração usada.

| Arquivo desta compilação de teste | SHA-256 |
| --- | --- |
| FretsOnFire_Dreamcast.elf | `de2bd692a95c022dcca55e616804fa20a69086c4b05ebe2cbf1406a42e99ed51` |
| FretsOnFire_Dreamcast.cdi | `f58e4d4f401792ded6741b043f8e80d3691b605c8d001e35173392fea2b5de8c` |
| Sequência de teclado em Defy Amazing | `44045f428d1db20343ae31c782b9b5c9729838d8bccb155799a064374a3af14a` |

Compilação com KallistiOS 2.2.1, GCC 15.1.0 SH-4, Vorbisfile/ogg de kos-ports e mkdcdisc. O comando está no [README](README.md). O ROM disk contém os gráficos convertidos, o tutorial e os efeitos. Ogg de menu e trilhas em jogo vêm do original. Os efeitos foram adaptados para 22,05 kHz Yamaha ADPCM. A leitura direta de arquivos no sistema Joliet da CDI confirmou 26 arquivos das pastas das quatro músicas idênticos ao pacote original, incluindo os charts, as trilhas e as capas.

| Defy The Machine, arquivos dentro da CDI | SHA-256 |
| --- | --- |
| notes.mid | `866487093c06144adb62e3d0a9b3b0b3330aac2464b05474df6054a020bf4395` |
| song.ogg | `ace9ac477e98de687b2062a899ec7e60e34b2e136b4ff1e866a952852b02b502` |
| guitar.ogg | `a9e18c6c8f157ef1e4e55292f3755055c23d7ed9fc757f5b066adb0679642afe` |

#### O que mudou

- Menu, logo, fontes, cassetes, palco, câmera, pista, casas, notas e HUD baseados nos recursos originais de 1.3.110. A imagem é renderizada no PowerVR em 640 × 480.
- Biblioteca com as músicas do pacote. A dificuldade usa os nomes Amazing, Medium, Easy e Supaeasy quando disponível. Tutorial pelo menu próprio.
- Entrada Maple com F1 a F5, Enter e Shift direito. Os relatórios de teclado são enfileirados durante a partida. A rotina de teclado do KOS captura mudanças nas teclas comuns, e a rotina de VBlank amostra também o Shift.
- A/V em 100 ms, oito batidas de contagem inicial, Esc para pausar e retomar. Enter em Return to Song resume no port. O original usa Esc para fechar a pausa e dispara a retomada no fim da transição.
- Ganhos iniciais de acompanhamento e guitarra em 1,0. Erros de guitarra silenciam o instrumento. Um novo efeito de palhetada errada substitui o anterior em canais AICA dedicados. Final de música baseado na trilha de acompanhamento.
- Novas regras de acordes, tapping, HOPO, multiplicador, sustain, janela de palhetada sem casa, contagem de gemas e estrelas derivadas da implementação original.

#### Validação interna

Os quatro charts oficiais totalizam 4.606 gemas em todos os níveis. Dificuldade, casa e marcação de HOPO coincidiram por evento com as classes de leitura do pacote. As diferenças de começo e fim foram menores que 0,001 ms para esses charts.

Em um relógio comum de 5 ms, o teste de regras C++ e o original de PC processaram 828 transições decisórias iguais de acertos, pontuação, sequência e ganho. O teste usa os handlers do jogo original e isola áudio, vídeo e transporte de entrada. Uma fixture adicional verificou acordes na janela de acerto, janela de 30 ms, pulso mantido, omissão, palhetada tardia e corte de sustain.

#### Integração com música completa

A gravação integrada usa Flycast 2.7, teclado emulado na porta A, áudio SDL em 44,1 kHz e janela 640 × 480. A captura a 20 quadros por segundo e a renderização por software afetam a medição temporal no host.

| Defy The Machine, Amazing | Linux 1.3.110 | Dreamcast no Flycast |
| --- | ---: | ---: |
| Alvos acertados | 609 / 614 | 611 / 614 |
| Gemas acertadas | 656 / 662 | 658 / 662 |
| Pontuação | 99.300 | 100.849 |
| Estrelas | 5 | 5 |

As duas execuções passaram pelo menu, seleção, dificuldade, partida completa, pausa, retomada, resultado e início de outra partida. Foram enviados 2.256 eventos de teclado planejados, incluindo omissões, uma palhetada errada e soltura antecipada. A fila Maple registrou zero estouros. Na primeira divergência identificada, o Shift direito foi previsto para 7.008 ms e o PC só processou o evento aos 7.274 ms, fora da janela original. O Dreamcast registrou o alvo. A programação de entrada pode divergir em cada plataforma. Os testes internos isolam esse efeito usando o mesmo relógio.

O port registrou 5.788 quadros em jogo, 2.424 intervalos acima de 25 ms e 1.212 acima de 50 ms. O maior intervalo registrado foi de 125,713 ms. Na abertura da partida, o heap alocado somava 570.040 bytes, a VRAM livre 709.096 bytes e a memória AICA livre 827.392 bytes. Essas são leituras pontuais e métricas da execução instrumentada em Flycast.

As capturas preservam as saídas estéreo de cada plataforma. Em 35,99 s da música, ambas mantêm o acompanhamento em ganho próximo de 1 e silenciam a guitarra após uma soltura antecipada. Em 37,15 s ambas restauram as duas trilhas. Na captura do Flycast, há 11 trechos de silêncio estéreo de pelo menos 60 ms fora da pausa. O maior mede 162,54 ms. O código do backend SDL de Flycast 2.7 pode inserir um bloco de zeros sem consumir amostras da fila quando a entrada é insuficiente. Ainda falta aferir o áudio antes do backend e no Dreamcast físico. [Rotina SDL do Flycast 2.7](https://github.com/flyinghead/flycast/blob/v2.7/core/audio/audiobackend_sdl2.cpp)

#### Pendências

- Medir relatórios Maple e eventos da primeira divergência restante com o mesmo ponto musical e determinar quanto vem da entrada. Pulsos que o teclado e o barramento nunca amostram seguem fora da fila. O Esc ainda é lido no laço principal.
- Alinhar a contagem visual ao atraso A/V desde o início. Nesta compilação anterior o port usava Esc para sair dessa etapa.
- Completar a animação das luzes, a forma de onda dos sustains, as transições das fitas e as telas de recordes, bem como as opções da [matriz de paridade](PARIDADE.md).
- Capturar o PCM antes do backend SDL do Flycast e no Dreamcast físico. Verificar boot, acorde de cinco teclas, alternância de palheta, Esc, sustain interrompido e Replay em uma música completa.
