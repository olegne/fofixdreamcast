# Checkpoint do Frets on Fire clássico

## Contagem, teclado e efeitos de palco

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

### Diagnóstico de cortes de áudio

O Flycast 2.7 executou a CDI anterior (`f58e4d4f…`) em um replay integral de Defy com captura na entrada e na saída do backend SDL2. Sem vídeo contínuo, os 8.309.760 frames válidos do callback ficaram idênticos byte a byte à entrada da fila. O backend inseriu 36 blocos de silêncio de 1.024 frames em jogo, somando 835,92 ms. Dois grupos contínuos de 69,66 ms foram reproduzidos integralmente pela falta de amostras na fila. O silêncio da pausa já estava presente no PCM antes da fila. A deriva de 812,70 ms depois da pausa correspondeu exatamente a 35 dos blocos zerados.

A sonda identifica a rotina real do backend que preenche os blocos com zeros, além de guardar o PCM original. O resultado localiza o corte reproduzido no Flycast. A saída do Dreamcast físico, a latência de cada plataforma e a paridade global da mixagem continuam pendentes. [Código SDL do Flycast 2.7](https://github.com/flyinghead/flycast/blob/v2.7/core/audio/audiobackend_sdl2.cpp)

Na inicialização, o KallistiOS aceita carregar `jurgen1.wav`, `jurgen4.wav` e `jurgen5.wav` e avisa que excedem o limite de 65.534 amostras. A rotina `snd_sfx_play_ex` do KallistiOS limita a reprodução às primeiras 65.534 amostras. Os três efeitos de resultado serão migrados para um caminho que reproduza a duração integral, com teste a partir de uma partida de baixa pontuação.

### Música completa com a candidata

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

## Histórico do checkpoint anterior

### Proveniência

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

### O que mudou

- Menu, logo, fontes, cassetes, palco, câmera, pista, casas, notas e HUD baseados nos recursos originais de 1.3.110. A imagem é renderizada no PowerVR em 640 × 480.
- Biblioteca com as músicas do pacote. A dificuldade usa os nomes Amazing, Medium, Easy e Supaeasy quando disponível. Tutorial pelo menu próprio.
- Entrada Maple com F1 a F5, Enter e Shift direito. Os relatórios de teclado são enfileirados durante a partida. A rotina de teclado do KOS captura mudanças nas teclas comuns, e a rotina de VBlank amostra também o Shift.
- A/V em 100 ms, oito batidas de contagem inicial, Esc para pausar e retomar. Enter em Return to Song resume no port. O original usa Esc para fechar a pausa e dispara a retomada no fim da transição.
- Ganhos iniciais de acompanhamento e guitarra em 1,0. Erros de guitarra silenciam o instrumento. Um novo efeito de palhetada errada substitui o anterior em canais AICA dedicados. Final de música baseado na trilha de acompanhamento.
- Novas regras de acordes, tapping, HOPO, multiplicador, sustain, janela de palhetada sem casa, contagem de gemas e estrelas derivadas da implementação original.

### Validação interna

Os quatro charts oficiais totalizam 4.606 gemas em todos os níveis. Dificuldade, casa e marcação de HOPO coincidiram por evento com as classes de leitura do pacote. As diferenças de começo e fim foram menores que 0,001 ms para esses charts.

Em um relógio comum de 5 ms, o teste de regras C++ e o original de PC processaram 828 transições decisórias iguais de acertos, pontuação, sequência e ganho. O teste usa os handlers do jogo original e isola áudio, vídeo e transporte de entrada. Uma fixture adicional verificou acordes na janela de acerto, janela de 30 ms, pulso mantido, omissão, palhetada tardia e corte de sustain.

### Integração com música completa

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

### Pendências

- Medir relatórios Maple e eventos da primeira divergência restante com o mesmo ponto musical e determinar quanto vem da entrada. Pulsos que o teclado e o barramento nunca amostram seguem fora da fila. O Esc ainda é lido no laço principal.
- Alinhar a contagem visual ao atraso A/V desde o início. Nesta compilação anterior o port usava Esc para sair dessa etapa.
- Completar a animação das luzes, a forma de onda dos sustains, as transições das fitas e as telas de recordes, bem como as opções da [matriz de paridade](PARIDADE.md).
- Capturar o PCM antes do backend SDL do Flycast e no Dreamcast físico. Verificar boot, acorde de cinco teclas, alternância de palheta, Esc, sustain interrompido e Replay em uma música completa.
