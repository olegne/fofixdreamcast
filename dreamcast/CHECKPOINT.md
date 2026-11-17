# Checkpoint do Frets on Fire clássico

## VMU na porta 2 com teclado e rumble nos multiplicadores — 17/11/2026

Continuação da CDI `FretsOnFire_13110_Dreamcast_VMU.cdi` de SHA-256 `21b854bfc01acb8b60a432502094637ea3598c74af96340a8e70af09182c1cee`. Código na branch `dreamcast-classic`, commit `55e50a24709878490ce407850abbf29c2e3302c4`, árvore `99f37d1c95d98f92a032ade3653e9d5876eba36b`. Compilação com KallistiOS 2.2.1 (tag oficial `b125c930da1a7969c5b9760da98254c666b3eeba`), GCC SH-4 15.1.0, libvorbis, libogg, SH4ZAM, zlib, libkosfat e mkdcdisc 0.0.4. Comando `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data`, com `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. O build terminou sem erros ou avisos.

| Artefato final testado | SHA-256 |
| --- | --- |
| ELF | `694c0d2421a37d01e7fc7c35101927b7e062b072475124822d0821f7d9c4834f` |
| CDI | `ba6a20f629d08b347bbb91635b559b8d5e785a4d15e3da1a18c78f332b22eb35` |

A CDI tem 42.291.913 bytes. A inspeção da imagem confirmou os mesmos 26 arquivos de músicas, 21 efeitos Ogg e 111 arquivos do ROMFS, idênticos byte a byte aos da imagem indicada pelo usuário. Nenhum recurso foi acrescentado ou removido. A árvore ISO9660 muda somente em `1ST_READ.BIN`; o contêiner ROMFS pode ter organização/metadados de geração diferentes, com todos os conteúdos preservados.

### Comportamento

Com teclado Maple ativo, o LCD é selecionado exclusivamente entre os dispositivos da porta física B (porta 2, índice 1). A ausência de VMU em B não redireciona a animação a outro controle. Sem teclado, a seleção original do primeiro LCD disponível permanece. A mudança de modo cancela quadros ainda não enviados e invalida confirmações do modo anterior sem modificar buffers DMA em andamento.

O rumble acompanha os aumentos reais de `Round::multiplier()`: 1× → 2×, 2× → 3× e 3× → 4×. A pontuação continua limitada a 4×, conforme as regras existentes. Quedas, sustains e acertos que mantêm o multiplicador não produzem novos pulsos. Recuperar a sequência volta a gerar feedback. Com teclado, o rumble também usa B; sem teclado, segue a porta do primeiro controle que comanda o jogo.

O transporte usa um frame estático próprio, sem a espera bloqueante do helper de rumble do KOS 2.2.1. O hook Maple atende entradas, rumble e LCD nessa ordem. A parada tem prazo de 140 ms, atendido na próxima passagem do serviço Maple mesmo durante carregamentos. Os comandos finitos de frequência 7 e potências 5/6/7 correspondem a `0x01075010`, `0x01076010` e `0x01077010`; STOP é `0x00000010`. Os decodificadores reais dos pacotes RP G9A_RAMFIX1 e STOP_ZERO_SOURCE_FIX aceitaram as três condições com duração autônoma de 142 ms e aceitaram STOP com intensidade/duração zero. Pausa, reinício, saída da partida e troca de origem cancelam feedback pendente. Reconexão usa geração por dispositivo; retries e fila têm limites fixos.

### Validação dirigida da própria CDI

O Flycast oficial 2.7, sem alterações, executou dois ensaios. A prova de roteamento usou teclado em C, controles em A/B, VMUs A1/B1 e rumble A2/B2. O estado serializado independente dos periféricos confirmou A1 vazio e B1 animado. Remover B1 não causou envios a A1; recolocar B1 retomou a animação; remover o teclado restaurou a seleção A1.

O ensaio de jogo usou eventos SDL de teclado recebidos pelo Maple e o Jurgen já existente para produzir acertos reais do chart. Os estados independentes do PuruPuru B2 confirmaram os três valores ON, STOP e a recuperação de 2×. Foram quatro inícios, quatro paradas e oito ACKs. Depois de chegar a 4×, mais 14 acertos não geraram outro pulso; 22 erros permaneceram silenciosos; recuperar 2× gerou o quarto pulso. A pausa encerrou o primeiro em 100 ms, e os STOP normais foram despachados em 150 ms na cadência Maple observada. O LCD continuou animado durante os pulsos, com 293 envios e 293 ACKs. Não houve erro Lua nem falha de asserção.

Os testes de host em `tests/feedback/run.sh` exercitam o código real com barramento simulado: 11 cenários de rumble e uma sequência de transporte LCD, incluindo porta exclusiva, ambos os slots, falta de acessório, dispositivo ocupado, falha de fila, AGAIN, DMA em andamento, hotplug, cancelamento, pausa/reinício e descarte de imagens repetidas. Os 11 casos de rumble também passaram com ASan e UBSan. A sensação do motor e a execução desta revisão no Dreamcast físico ainda precisam de validação na bancada.

## Pista da VMU e crédito do port

Código na branch `dreamcast-classic`, commit `852c5b9f24c7c16f34fcf5669c976e8320f2a248`, árvore `651809e4683dffd07da3a644c8feead9d25fb214`. A CDI foi compilada com KallistiOS 2.2.1, GCC SH-4 15.1.0, SH4ZAM, libvorbis, libogg, libkosfat e zlib. A compilação isolada usou `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data` com `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. Os 26 arquivos do repertório e os 21 efeitos Ogg passaram na prova da imagem e do ROM disk.

| Artefato | SHA-256 |
| --- | --- |
| ELF | `9172f16ddd98d44785e373d79533ece632ead75c4db33ed5d36fd035ec7b1c97` |
| CDI | `21b854bfc01acb8b60a432502094637ea3598c74af96340a8e70af09182c1cee` |

A VMU tem cinco linhas, linha de acerto, acordes, caudas das notas e marcação de acertos. Usa as notas da dificuldade selecionada e o relógio de áudio com a compensação A/V. No LCD 48 × 32 do Flycast, a primeira nota de Defy Amazing desceu da linha 6 até a linha 23 entre 3,07 s e 3,80 s. Um acorde F2+F4 apareceu junto e a pausa mostrou um quadro estático. O menu e os créditos têm movimento próprio. A bandeira de chegada do resultado foi confirmada no LCD do Flycast. A rolagem na TV mostra `Dreamcast port by` e `angelo128bits`, usando o espaço depois da versão. A duração dos créditos permaneceu em 35,5 s.

A imagem usa um buffer fixo, tem trabalho limitado a 64 acordes por quadro e é atualizada no máximo uma vez a cada 67 ms. Cada transferência de LCD usa 200 bytes de dados. As imagens repetidas são descartadas e o envio fica atrás das leituras dos dispositivos no lote Maple. Os comandos são assíncronos e os salvamentos suspendem a animação. Na CDI testada, uma configuração foi gravada na VMU B1, com retomada do LCD em seguida. A retirada e a reinserção de B1 durante a reprodução mantiveram a posição musical, o teclado pausou e retomou, e a mesma VMU voltou a receber quadros. Outra tomada iniciou, pausou e retomou sem uma VMU.

A sonda de renderização em SH-4 emulado mediu cerca de 36,3 µs por atualização típica e 295,1 µs com 64 acordes de cinco notas. Esses valores referem-se à formação da imagem. Nos primeiros 30 s de Defy Amazing, a comparação entre a CDI Maple anterior e esta CDI usou o mesmo perfil de Flycast. Os intervalos de avanço no relógio da faixa tiveram mediana de 17 ms, p95 de 28 ms e p99 de 37 ms nas duas. Todos os relatórios de teclado enviados no trecho medido foram confirmados pelo jogo.

Defy Amazing completou 167,213 s na própria CDI entregue. Houve pausa, retomada, resultado, gravação na VMU e nova partida. O teclado confirmou 2.256 de 2.256 transições e a fila do jogo registrou zero estouros. Ao reaplicar os relatórios ao Linux no mesmo relógio, as 9.597 atualizações de placar e os 99.860 pontos foram iguais. 8.832.000 quadros PCM válidos coincidiram byte a byte entre a fila e a saída SDL do Flycast, sem faltas depois do boot. O maior intervalo entre iterações do convidado na partida foi de 77,06 ms.

O usuário relatou que a CDI Maple anterior funcionou no Dreamcast físico. A nova tela LCD e sua cadência Maple ainda precisam do teste físico.

## Prioridade Maple, vídeo, biblioteca SD e leitura de áudio

A CDI desta rodada foi compilada com KallistiOS 2.2.1, GCC SH-4 15.1.0, SH4ZAM, libvorbis, libogg, libkosfat e zlib. Código na branch `dreamcast-classic`, commit `58bd733d90410f6857cd1394041c0d32d0ec097a`, árvore `ec4d4392c8424970fbd87220165f020c6f1d4497`. O build isolado usou `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data` e `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. Os 26 arquivos originais do repertório e os 21 efeitos Ogg foram verificados na própria imagem, incluindo o ROM disk extraído do executável.

| Artefato | SHA-256 |
| --- | --- |
| ELF | `6bdb8a58052d4c8065e4dfc52a2ffec5d8726f5b22313f1ee0680e123a8f955f` |
| CDI | `7c1f32be108894b8665207edbdddf340bcb8f59b130328a28c11f8c6a1cf644c` |

### Controles e interface

O teclado Maple toma a entrada assim que identificado, mesmo antes do primeiro relatório de teclas. A desconexão deixa o primeiro controle assumir. Na troca, o jogo encerra o sustain da entrada anterior, descarta palhetadas pendentes e aguarda a soltura de teclas ou botões já pressionados. Os testes Flycast usaram o teclado nas portas A e C, controle na B e VMU no slot B1. A sonda tentou A, B, START e direcional simultâneos com teclado presente, passou por pausa, retomada e reconexão, e conferiu uma palhetada de cada fonte após a troca. O controle sozinho abriu o Tutorial no boot. A sonda dirigida também confirmou que um Esc mantido antes de conectar não pausa e que um B mantido ao fechar a pausa não vira casa na volta.

A abertura roda as barras cumulativamente como o clássico e eleva as caudas com as cabeças. A luminância foi calibrada em 732 amostras da versão Linux com suas opções de filtragem. Em Defy Amazing no Flycast, zero de 86 quadros perdeu a barra distante. A razão mediana da energia caiu de 1,007 para 0,501 no trecho medido e as médias correspondentes no Linux ficaram entre 0,48 e 0,53. As diferenças de amostragem entre PowerVR e Mesa ainda afetam pixels e contornos. O aviso de recorde com truques foi conferido lado a lado, depois de `fastforward` em Defy Easy. Os limites do texto foram x65–494 e y198–231 nos dois. A partida comum gravou `ACE` na VMU. A tela de resultado e sua locução aguardam a saída do palco.

### Leitura de áudio e vídeo

A leitura do cache de 16 KiB por trilha foi alinhada aos setores ISO9660 para manter o DMA de múltiplos setores depois de seeks Vorbis. Em 30 s de Defy no mesmo perfil instrumentado do Flycast, os intervalos acima de 50 ms foram de 47 para 4 e o p99 caiu de 55,384 para 36,960 ms. O maior intervalo foi de 89,213 para 59,029 ms. A leitura no pior quadro passou de 72,454 para 38,389 ms no máximo da amostra. Sete janelas estéreo da variante limpa antes e depois de pausar somaram 313.110 quadros iguais amostra por amostra depois do alinhamento por fase. Essa medição é do emulador e o limite de leitura óptica do Dreamcast físico segue pendente.

O menu aplica 640 × 480 ou 320 × 240, renderização a cada atualização ou à metade da frequência de vídeo, e antialiasing horizontal 2× no modo 320 × 240. O limite só pula renderização. Um ensaio mediu 90 apresentações em 90 VBlanks em frequência cheia e 44–45 com o limite. As opções voltaram da VMU após reboot; um save antigo conservou o atraso, o tamanho do texto, o modo canhoto e as teclas. O editor abriu nos dois modos. Em 320 × 240 no Flycast, o enquadramento capturado desceu 14 px quando apresentado em 640 × 480. 240p, compatibilidade dos cabos e velocidade no console precisam do ensaio físico.

### Biblioteca externa

O editor monta a primeira partição MBR FAT16/FAT32 do cartão somente para leitura. O jogo abre Ogg e MIDI já copiados ao cartão, ou deixa criar o chart e gravá-lo na VMU. O teste usou a montagem FAT e um dispositivo de blocos de teste no Flycast. A faixa com MIDI próprio, acorde, mudança de BPM e áudio de 32/44,1 kHz foi exercitada. O editor fez prévia, salvou na VMU e reabriu o chart após reiniciar. Cartão sem adaptador e partição com BPB inválido foram recusados sem interromper o acesso ao Tutorial. A transferência do adaptador serial SPI precisa de teste físico. A extração de DVD de Guitar Hero permanece no PC e sua biblioteca convertida pode ser copiada ao SD. Os discos originais e o ranking histórico seguem sem validação.

Dois novos MIDIs de teste, SMF 0 e 1, tiveram nove gemas, horários, sustains e marcações de HOPO iguais aos do leitor Linux. Os casos incluíram running status, nota desligada com velocidade zero, tempo variável, canal alternado e pista de baixo. Quatro arquivos inválidos ou com tempo SMPTE foram rejeitados. A cobertura ainda é a dos formatos de chart ensaiados.

### Replays integrais da CDI entregue

As quatro faixas passaram por partida completa, pausa, retomada, resultado, gravação na VMU e replay. Foram observadas as 5.142 transições de entrada pelo Maple e 41.762 atualizações de pontuação iguais ao Linux com as mesmas entradas e o relógio da partida.

| Faixa · dificuldade | Transições Maple/SDL | Pontos | Atualizações iguais ao Linux | Faltas SDL durante o fluxo | Intervalos acima de 50 ms | Maior intervalo no convidado |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Defy The Machine · Amazing | 2.256/2.256 | 99.945 | 9.603 | 16 | 180 | 75,67 ms |
| Bang Bang, Mystery Man · Medium | 852/852 | 116.177 | 11.320 | 3 | 131 | 76,69 ms |
| This Week I've Been Mostly Playing Guitar · Amazing | 1.804/1.804 | 164.299 | 12.092 | 8 | 162 | 75,78 ms |
| Tutorial · Medium | 230/230 | 3.232 | 8.747 | 0 | 0 | 41,79 ms |

O Flycast 2.7 conservou 37.104.640 quadros PCM válidos, idênticos entre a fila e a saída SDL. O perfil de captura usou fila de 16.384 amostras e dois workers para o render. A coluna de faltas exclui o boot e soma 27 blocos zerados na saída SDL. As medidas de iteração do convidado ainda precisam da mesma prova no Dreamcast físico.

Os primeiros replays das três músicas concorreram por recursos do host com três instâncias do Flycast. Na repetição com até duas instâncias e o mesmo perfil de áudio, os blocos de falta após o boot foram 0 em Defy The Machine, 0 em Bang Bang, Mystery Man, 0 em This Week I've Been Mostly Playing Guitar. A prova conservou e analisou a saída original de cada execução.

## Linhas da pista, transições do palco e pausa dos efeitos

Código na branch `dreamcast-classic`, commit `fb08b08a56fd443312316024839bd6394c9f1534`, árvore `66ee63e224e83e020ff0283bde6c110892f770a1`. Build isolado com KallistiOS 2.2.1, GCC SH-4 15.1.0, SH4ZAM, libvorbis, libogg e zlib. Comando `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data` com `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. Os 26 arquivos do repertório e os 21 efeitos Ogg foram conferidos contra o pacote Linux 1.3.110 diretamente na imagem. O ROM disk extraído do executável da CDI coincide com o do ELF.

| Artefato | SHA-256 |
| --- | --- |
| ELF | `b20da44054ce763ad5c41a9aee70a512ce2bf19b6aef3de6876d43a63d6b6359` |
| CDI | `8b083e6e4b4aac8c7a87316d3c31e07c8f4b8c670e554b742b366fc83594f8d9` |

### Linhas horizontais da pista

A região opaca da textura original fica abaixo de um pixel quando a linha está distante. O filtro bilinear da CDI anterior podia amostrar somente as margens transparentes. O desenho integra a cobertura vertical do pixel na posição projetada e preserva a textura no sentido horizontal. As linhas móveis também continuam atrás da linha de acerto e desvanecem até `z=-1`, como na referência.

No A/B do trecho de 1,7 a 5,8 s de Defy Amazing a 640 × 480, o limiar de energia verde abaixo de 8 sinalizou 86 de 215 quadros da versão anterior e zero de 209 com a correção. A energia da linha distante dividida pela energia prevista pela projeção passou de p10/p50/p90 de 0,008/1,593/2,074 para 0,984/1,005/1,026. Na retirada atrás dos trastes, os mesmos percentis na candidata foram 0,983/0,996/1,004 em 44 quadros. Repetindo na própria CDI entregue, foram zero desaparecimentos em 118 amostras de 20 fps, com p10/p50/p90 de 0,983/1,007/1,029.

### Outros ajustes

- O palco usa a duração original de 512 ms, a curva anterior de visibilidade e o deslocamento lateral das luzes. O plano de fundo mantém uma única alocação de VRAM. A sequência gravada no Flycast mostrou a saída dos trastes e das cordas até o resultado. No ensaio de 150 quadros contra `Layer.render` do Linux, o alfa coincidiu e o desvio máximo do centro foi de 0,000123 px.
- Ao pausar efeitos curtos, o mixer envia os comandos de parada em lote e estima a posição de retomada pelo cursor e pela confirmação do ARM. Na sonda Flycast com 1, 3 e 8 vozes, o lote parou em cerca de 10 ms. A voz medida, `fiba6`, repetiu até seis amostras (0,14 ms). Em Defy, com erro, pausa e retomada pelo Maple, o efeito fiba3 repetiu entre 256 e 261 amostras na imagem anterior e entre uma e seis amostras após a correção. O cursor musical retomou em 1.040 ms nas duas imagens.
- Os modos Chilly e LightGraphics ativados pelo teclado Maple abriram o Tutorial na CDI final. As sondas capturaram quadros de jogo em 1,5 e 3 s para os dois modos.
- A caixa de recorde quebra o texto dentro da mesma faixa horizontal do original e posiciona nome e cursor após a última linha. Com o remapeamento X carregado da VMU, foi possível abrir a caixa com X, confirmar `ACE-1` com F1, gravar e voltar à seleção.

### Partidas completas na CDI testada

As quatro sessões passaram por pausa, retomada, resultado, gravação do recorde na VMU e nova partida. As 5.142 transições enviadas pelo SDL foram confirmadas pelo estado recebido no Maple. Os mesmos horários e estados produziram os mesmos alvos e placares no Linux em 40.211 atualizações.

| Faixa · dificuldade | Transições Maple / SDL | Pontos | Avanços iguais ao Linux | Faltas SDL do início ao fim | Maior intervalo do convidado |
| --- | ---: | ---: | ---: | ---: | ---: |
| Defy The Machine · Amazing | 2256/2256 | 99.866 | 9.057/9.057 | 0 | 110.66 ms |
| Bang Bang, Mystery Man · Medium | 852/852 | 116.172 | 10.874/10.874 | 0 | 98.01 ms |
| This Week I've Been Mostly Playing Guitar · Amazing | 1804/1804 | 155.766 | 11.530/11.530 | 1 | 101.31 ms |
| Tutorial · Medium | 230/230 | 3.434 | 8.750/8.750 | 0 | 41.55 ms |

A captura no Flycast 2.7 usou dois workers de renderização, fila SDL de 16.384 amostras e PCM estéreo a 44,1 kHz. Os 37.158.912 quadros válidos do callback coincidiram byte a byte com a saída anterior à fila. A sonda isolou um bloco de 1.024 amostras (23,22 ms) zerado pelo callback SDL após o início das partidas. O bloco apareceu em This Week aos 139,55 s da captura. Na repetição completa da mesma CDI, com o mesmo perfil do Flycast, o callback não inseriu cortes durante partida, pausa ou resultados. As 1.804 transições chegaram ao Maple e os 11.530 avanços coincidiram com o Linux, com placar de 156.902 pontos nas duas execuções comparadas. A fila do host acrescenta latência de captura. As medidas de tempo do convidado não medem a cadência do console físico.

Também foram analisadas 18 janelas das saídas PCM originais de Defy e outras 18 de Bang Bang, do início até os últimos segundos. O alinhamento estimado ficou constante dentro de cada fase da partida. A janela final de Defy teve correlação de 0,99999999 e ganho de 0,99973 no acompanhamento e 0,99976 na guitarra. Em Bang Bang, todas as janelas passaram de 0,9954 de correlação. A comparação de Defy incluiu erros e transições do volume da guitarra, com instantes efetivos de entrada diferentes nas sessões independentes. O alinhamento foi usado apenas na análise. As capturas conservaram o PCM de cada execução.

### Limites da validação

O giro das barras e a elevação dos sustains durante a breve entrada do palco, a luminância das barras comparada ao filtro de textura do PC, diferenças de tipografia e o aviso de recorde após uso de truque ainda diferem do Linux. A matriz também registra a validação física, o endpoint histórico de recordes e os DVDs opcionais. Em Defy, os maiores saltos do relógio do convidado reaparecem nas mesmas posições musicais nas capturas com e sem vídeo e coincidem com a passagem de fronteiras de reabastecimento do PCM. Na análise com os Ogg originais, 133 de 133 saltos de cursor acima de 50 ms seguiram um reabastecimento com pedido de bytes comprimidos. Um modelo do cache de 16 KiB previu carga em 74 desses casos. A simulação não separa o custo de decodificar, ler o disco e obter a posição do ARM. A cadência no console requer instrumentação própria. Consulte o [roteiro de bancada](README.md#validação-no-dreamcast-físico).

## Editor, ranking, efeitos e qualificação da execução

Código na branch `dreamcast-classic`, commit `d302454082be640b2bd004d22635d178c75c52f8`, árvore `f6e75f09c9b82e2d9fe2d102f2e2e273ba8e46da`. Compilação com KallistiOS 2.2.1, GCC SH-4 15.1.0, SH4ZAM, libvorbis, libogg e zlib. Comando `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data` com `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. Foram verificados os 26 arquivos originais do repertório e os 21 efeitos Ogg. O ROM disk do executável extraído da CDI foi comparado ao do ELF.

| Artefato de diagnóstico | SHA-256 |
| --- | --- |
| ELF | `c66f534fed0a0173af6493497d3f4963c7983b4c92e0c8a697848b5951239bc3` |
| CDI | `bf852ccc884847c1d929f3bc8e0fa2f6b314f7b65919d95f05688894144f27ba` |

### Partidas completas no Flycast

Os quatro replays na mesma CDI passaram pela pausa, retomada, resultado e nova partida. As 40.290 atualizações de placar e todos os alvos coincidiram com o Linux no relógio efetivo do jogo. O replay verificou as 5.142 transições do teclado do SDL pelos estados recebidos no Maple. A espera no teste pode deslocar a entrada de alguns trechos muito densos. A comparação reaplica esses horários e há um ensaio separado de regras com sequência comum.

| Música e dificuldade | Bordas no Maple / SDL | Pontos | Atualizações iguais | Faltas SDL após começar a música | Quadro mais longo |
| --- | ---: | ---: | ---: | ---: | ---: |
| Defy The Machine · Amazing | 2256/2256 | 99.889 | 9125/9125 | 0 | 110,75 ms |
| Bang Bang, Mystery Man · Medium | 852/852 | 116.101 | 10882/10882 | 0 | 98,25 ms |
| This Week I've Been Mostly Playing Guitar · Amazing | 1804/1804 | 163.174 | 11536/11536 | 0 | 103,36 ms |
| Tutorial · Medium | 230/230 | 3.143 | 8747/8747 | 0 | 41,46 ms |

Uma tomada de controle do Tutorial com pulsos fixos na mesma CDI recebeu 230/230 bordas, sem faltas durante a execução. O placar foi 4.293 em ambos e os 8.751 avanços de placar coincidiram depois de reaplicar o relógio.

O perfil de captura usou dois workers de renderização, fila SDL de 16.384 amostras, pontos de captura e PCM estereofônico. A fila ampliada cobre hiatos do host e acrescenta latência de captura. A mediana observada ficou por volta de 0,4 s no host. O perfil sem vídeo contínuo reduz a carga. A sonda encontrou zero faltas após o início da música e 15 blocos durante o boot das quatro sessões. O teste de controle com um hiato de produtor de 210 ms isolou os cortes da fila do host. Os 37.146.624 quadros de PCM válido coincidiram byte a byte na entrada e na saída do callback.

### Verificação das mudanças

- O relógio visual acompanha a suavização original nas mudanças de andamento. Em 2.681 quadros com mudança de BPM, pausa, sustain e reinício, os 53 gatilhos coincidiram e o erro máximo de período foi 0,001282 ms. Em torno de 3 s de Defy, o dither reduziu a diferença média de verde nas duas luminárias e nos dois cones examinados, sem trocar a quantidade de VRAM. Permanecem diferenças finas de filtragem, contorno e deslizamento na entrada e saída do palco.
- O mixer aceita stems entre 8 e 48 kHz no relógio de 44,1 kHz. O ensaio simultâneo de 48, 32 e 22,05 kHz reproduziu os cinco canais físicos de stem no AICA emulado. O seek de 127 ms teve 4.032 amostras por canal iguais às da reprodução contínua e o maior erro de ganho contra o Linux foi 0,109 ponto percentual. O FFmpeg gerou uma trilha curta com EOF limpo 12 amostras antes do total declarado. Com a correção, Bang Bang de teste terminou aos 3.000 ms. O ajuste experimental da retomada de efeitos, exercitado separadamente, reduziu a repetição de cerca de 1,8 ms para até cinco amostras na sonda e ainda precisa de validação com as vozes simultâneas e o ARM físico.
- O editor mostra e grava capas na VMU, conserva a edição de atraso A/V e usa a janela de 12 intervalos do estimador original. Um teste a 120 BPM deu 119,721 BPM com as batidas do Flycast. Após reiniciar, o port restaurou a capa e −137 ms de A/V, com os mesmos 49 bytes do MIDI. Nove janelas de PCM antes da fila SDL tiveram zero amostras de deriva no ensaio de gravação.
- A compilação importa as tabelas clássicas do perfil Linux. No teste, os dois recordes importados apareceram junto ao resultado novo digitado via Maple como `João Ç Í`. O nome voltou da VMU após reiniciar, e um save antigo também abriu. Uma alteração de notas usa seu próprio ranking identificado pelo MIDI ativo.
- O ranking começa desligado. O servidor local recebeu o mesmo formato do original e respondeu com posição 17 numa partida de 98 pontos. Com o envio desligado, a mesma CDI registrou o recorde e fez zero requisições. O serviço histórico e o adaptador físico ficam pendentes.
- Os menus usam 95 métricas ASCII iguais ao SDL_ttf de referência, rotação nas transições e fita filtrada. O título de dificuldade mediu 314 × 36 px tanto no Linux quanto no Flycast. Os 16 idiomas, incluindo inglês, foram exercitados com quatro combinações de temas. Segurar Down por 21 VBlanks fez o menu avançar do primeiro ao quarto item. As dez combinações de regras passaram novamente com 5.662 eventos iguais e a prova de autoplay passou nos quatro charts em 168.084 atualizações.

O [roteiro de bancada](README.md#validação-no-dreamcast-físico) reúne os testes físicos ainda pendentes. A [matriz](PARIDADE.md) conserva as diferenças de apresentação, o limite de comparação das fontes de sistema e o estado dos DVDs comerciais opcionais. O site oficial do original ainda documenta o [servidor de recordes para rede própria](https://fretsonfire.sourceforge.net/documentation/charts/).


## Perspectiva das notas e integração de recursos clássicos

Código da candidata de diagnóstico na branch `dreamcast-classic`, commit `641314b904cea46e7d9c083d94fc72e0e4fedb97`, árvore `963894227e50ef3230616b0736df07dc5c12d58f`. O build usa KallistiOS 2.2.1, GCC SH-4 15.1.0, SH4ZAM, libvorbis, libogg e zlib. Foi gerado com `make -C dreamcast cdi CLASSIC_DATA=/caminho/para/data`, `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"` e a pasta `songs` do pacote 1.3.110 para Linux. O pacote de teste distribui as três faixas originais e o Tutorial. A extração da própria CDI bateu byte a byte com os 26 arquivos originais do repertório. O ROM disk do ELF coincide com o do executável extraído da CDI. Os 21 efeitos sonoros Ogg mantêm os bytes do pacote Linux.

| Artefato de diagnóstico | SHA-256 |
| --- | --- |
| ELF | `7d7a66291c9428cf22906892202dbdeff17d5ee75c9d7808dbc228ac6288fb27` |
| CDI | `2be92484f3fe642b7a8cf8743e85148405b93affcd5c7c0d9fa20c6d16ecadfb` |

O renderizador trocava a malha por uma imagem plana após a 14ª casa visível ou no fim da pista. A imagem usava a projeção da linha de acerto, o que deixava mais altas e verticais as notas distantes. Em z=11,5, o bitmap tinha 12,5 px de altura e a malha original projetada tinha 5,5 px. A candidata mantém a mesma malha 3D em todo o percurso, inclusive ao aparecer no horizonte com alfa reduzido. Notas perdidas passam pela linha em cinza, com a altura comprimida como no Linux.

Os strips reduziram o envio de uma nota de 426 para 170 vértices e o marcador HOPO de 180 para 84. A reconstrução a partir dos strips preservou os 142 triângulos da nota e os 60 do HOPO, com as posições e fatores de luz originais. No caso de 40 cabeças e 39 marcadores do Tutorial, são 322.432 bytes de vértices de nota, além dos trastes, caudas e demais efeitos. O quadro de Defy Amazing de 3,008 s no Flycast mostra a nova nota deitada no mesmo plano da pista que o Linux em 3,010 s. [Comparação dos três quadros](https://chatgpt.com/library?file_id=libfile_4375eb9b59048191b337ccc88efa0dae&account_id=personal).

O seletor ganhou navegação por biblioteca, filtro, rótulos convertidos para PVR e prévia das faixas depois de 1.024 ms. O áudio oferece backing, guitarra e rhythm, além de volumes separados e fade nas transições. O editor abre charts existentes ou cria um chart a partir de Ogg preparado no PC. A VMU preserva configurações, recordes e alterações do editor. O importador de Guitar Hero roda no PC e prepara a pasta que entra no disco, com prova do decodificador original em 15 canais de fixtures. Os 15 catálogos de tradução foram incorporados, e Chilly e LightGraphics podem ser habilitados nos ajustes.

O ensaio de regras executado novamente após as mudanças passou nas dez combinações originais, com 5.662 transições idênticas no Linux e Dreamcast com o mesmo relógio. O teste do editor por Maple gravou um acorde F1+F2 na VMU. Após reiniciar, o seletor abriu a faixa importada com a dificuldade Amazing. O leitor do Linux encontrou nos dois MIDI o acorde de 0 a 122,950750 ms. A configuração testada foi restaurada após reiniciar e, durante a pausa, a alteração no volume da guitarra persistiu sem mudar o cursor de 1.553 ms na retomada.

As quatro sessões da mesma CDI chegaram ao resultado e iniciaram uma segunda partida. As 40.273 atualizações de pontuação e todos os alvos confirmados coincidiram com o Linux quando o original recebeu os relatórios Maple e horários efetivos de cada ensaio.

| Música e dificuldade | Alvos | Pontos | Atualizações iguais ao Linux | Blocos SDL em falta durante a música | Intervalo máximo no jogo |
| --- | ---: | ---: | ---: | ---: | ---: |
| Defy The Machine · Amazing | 611 / 614 | 100.032 | 9.115 / 9.115 | 0 | 113,60 ms |
| Bang Bang, Mystery Man · Medium | 244 / 247 | 116.160 | 10.873 / 10.873 | 24 | 110,15 ms |
| This Week I've Been Mostly Playing Guitar · Amazing | 547 / 550 | 164.116 | 11.533 / 11.533 | 12 | 96,99 ms |
| Tutorial · Medium | 44 / 59 | 4.242 | 8.752 / 8.752 | 1 | 43,73 ms |

Os replays incluem erros intencionais e pausa durante a música. Os quatro registraram zero estouros da fila de entrada do jogo. O SDL recebeu todos os 2.256, 852 e 1.804 eventos instrumentados em Defy, Bang Bang e This Week. O Maple emulado observou 2.254, 852 e 1.802, respectivamente. Uma segunda tomada de Defy com a gravação de vídeo desligada observou 2.252 de 2.256. A passagem de todos os pulsos de teste pela emulação permanece um ponto aberto. A igualdade da tabela usa o horário e a entrada observados no jogo.

A análise de áudio guarda o PCM de entrada na fila SDL e a saída original da captura. A sonda identifica os blocos de 1.024 amostras que o callback insere quando a fila fica vazia. As amostras válidas coincidiram byte a byte com a entrada da fila nos quatro ensaios. O vídeo contínuo de Defy conserva duas faixas de áudio, uma de cada execução, com atraso inicial registrado de 141,67 ms no lado Dreamcast. A montagem aplica um deslocamento inicial e conserva a duração, o ganho e a afinação. As outras três referências Linux completas têm PCM e registros de partida, com cobertura visual em quadros nas sessões que conseguiram gravá-los.

As sessões com gravação registraram 441, 409, 500 e 0 intervalos acima de 50 ms no jogo, na ordem da tabela. O pico de heap na entrada das quatro faixas foi inferior a 4,72 MB. A consulta a `pvr_get_stats` não mediu bytes no caminho direto por Store Queues. As medidas de quadro e áudio são do Flycast e do host usado nos ensaios.

Na candidata, o português foi restaurado da VMU após reiniciar. Os modos Chilly e LightGraphics foram aplicados pelo menu e renderizaram o Tutorial no Flycast. As capturas foram comparadas aos temas correspondentes do Linux. O teste de `uptomytempo` ligou e desligou Jurgen, registrou 22 alvos, e `fastforward` abriu o resultado sem caixa de recorde.

A [matriz](PARIDADE.md) relaciona as diferenças conhecidas. Para encerrar os ensaios virtualizados, além das partidas integrais, conferir a fase da luz em mudanças de andamento, os temas, a interface completa do editor e os serviços de classificação. O teste físico requer Dreamcast em vídeo 4:3, teclado Maple na porta A e VMU em outro controle. Verificar boot a frio, executar as quatro faixas, alternar Enter e Shift direito com acordes e sustains, pausar e retomar em sustain, conferir AICA pelos dois canais e confirmar recorde e configuração em um segundo boot.


## Cores, efeitos originais, recordes e repertório

A candidata parte de `d28932b2096461dbae8f741bda1b2171ddb20fb4` de `dreamcast-classic`. Mantém SH4ZAM, Store Queues, projeções em cache e leitura de CD alinhada. O build usa KallistiOS 2.2.1, GCC 15.1.0 e o comando do [README](README.md), com `CXXFLAGS="-std=c++17 -Wall -Wextra -Werror -MMD -MP -DFRETSONFIRE_INPUT_TRACE"` para a instrumentação de entrada. A sintaxe do build sem a macro de rastreamento também foi verificada.

| Arquivo de diagnóstico | SHA-256 |
| --- | --- |
| FretsOnFire_Dreamcast.elf | `2cca7a32d40dfb0daa91918b243cc0b7552f97d77f5d32e71317db186578e73b` |
| FretsOnFire_Dreamcast.cdi | `ec4fe1535b5fa14d49e8315b543d46c4975d87cbea1fc0e02b7cc11e61c1174c` |

A extração da própria CDI confirmou os 26 arquivos das quatro pastas de músicas. O ROMFS do ELF e o ROMFS do executável extraído da CDI coincidiram. Os 21 efeitos Ogg do pacote incluídos no ROMFS são idênticos byte a byte aos do Linux.

### Luz e cor durante a partida

As malhas de notas, HOPO e trastes usam a lâmpada e os fatores de material de `note.dae` e `key.dae`. No teste com feedback OpenGL da versão 1.3.110, os 1.938 vértices amostrados das quatro partes de nota e dos três estados de traste ficaram a menos de um nível RGB8 da cor original. Antes, o maior desvio nesse teste era de 177,1 níveis. Os trastes também usam o pressionamento, o retorno progressivo após soltar e as três camadas de luz aditiva de `Guitar.renderFrets`.

Os refletores usam as posições, intensidades e gatilhos de `stage.ini`, a fase do `Stage.run` e uma textura de contribuição luminosa em RGB565 direto. A intensidade da textura é o produto da luminância com o alfa do PNG original. No quadro de Defy perto de 3,0 s, o erro absoluto médio no canal verde da faixa abaixo do HUD foi de 5,48 para 3,82 níveis. O ensaio de 704 VBlanks pré-pausa registrou p95 de 29,32 ms no controle, 49,77 ms no piloto PAL8 e 27,25 ms na candidata RGB565. O piloto PAL8 foi substituído pelo RGB565. Posicionamento do cone direito, iluminação ao redor de 1,5 s, fase dos elementos do palco e transições visuais continuam em verificação.

### Efeitos curtos

Os 12 efeitos curtos agora são decodificados a partir dos Ogg originais para PCM estéreo de 44,1 kHz. A voz dedicada de erro substitui a anterior com ganho nominal de 25%. A navegação pode tocar duas vozes simultaneamente junto à música. A amostra combinada do ensaio isolado teve correlação superior a 0,999999998 nos dois canais e erro RMS abaixo de 0,72 nível PCM nos trechos fora da saturação. O ganho do erro foi de 0,24983, com contribuição do erro anterior abaixo de 0,000019. A candidata integrada reproduziu `out.ogg` durante a pausa em Defy e Bang Bang, com correlação de pelo menos 0,999999997 nas janelas medidas. Na prova de pausa com `fiba6` ativo, o PCM antes da fila ficou em zero no miolo da pausa. A retomada reproduziu a mesma voz, repetindo 76 a 81 amostras junto à borda de envelope, cerca de 1,8 ms.

### Recordes, seleção e créditos

O ranking guarda cinco resultados por faixa e dificuldade. O ensaio de ordenação usou o `SongInfo` do Linux e confirmou a ordem e o empate estável. Na CDI de teste de gravação, o teclado Maple digitou `ACE-1`, com Shift, hífen e correção por Backspace. A VMU B1 guardou a tabela, que voltou após reiniciar o Flycast. Um cancelamento manteve o nome anterior. Com VMU cheia a imagem do cartão permaneceu idêntica byte a byte, e a tabela continuou na sessão tanto sem VMU quanto com VMU cheia. Os nomes aceitam até 20 caracteres ASCII. Importação dos recordes do Linux, envio online e VMU física seguem pendentes.

O seletor usa as malhas dos cassetes originais, a inclinação e o movimento de câmera. O teste Maple percorreu as três músicas e abriu a escolha de dificuldade. As três antigas texturas fixas ocupavam 384 KiB de VRAM. No probe de menu, houve 26 callbacks de apresentação em 32 VBlanks durante a transição do modelo, ante 32 no controle. Os créditos reproduzem os 80 elementos da referência e a dupla de trilhas de Defy. O teste passou pela abertura, pelo retorno via Esc e pelo retorno automático após a rolagem. Os quadros lado a lado registram diferenças de fonte e fase dos fundos. A transição de áudio dos créditos ainda pede o fade do Linux.

### Repertório original completo

As dez combinações jogáveis de faixa e dificuldade coincidiram nas 5.662 transições de regras com o mesmo relógio de 5 ms, incluindo omissões, erro, acordes, HOPO e soltura antecipada. As três dificuldades de cada faixa regular ficam disponíveis na seleção. O tutorial abre diretamente em Medium, como no Linux.

As quatro partidas integrais na mesma CDI do Flycast abriram a faixa, passaram por pausa e retorno, exibiram o resultado e iniciaram nova partida. Na última coluna, o Linux recebeu cada relatório Maple e cada horário efetivo da sessão Dreamcast correspondente. Todos os IDs de acertos, notas e pontos coincidiram nessas reaplicações.

| Faixa | Nível | Alvos no Flycast | Notas | Pontos | Atualizações idênticas no Linux |
| --- | --- | ---: | ---: | ---: | ---: |
| Defy The Machine | Amazing | 611 / 614 | 658 / 662 | 99.957 | 9.053 / 9.053 |
| Bang Bang, Mystery Man | Medium | 244 / 247 | 375 / 380 | 116.353 | 10.872 / 10.872 |
| This Week I've Been Mostly Playing Guitar | Amazing | 544 / 550 | 651 / 661 | 138.836 | 11.517 / 11.517 |
| Tutorial | Medium | 44 / 59 | 48 / 63 | 4.238 | 8.748 / 8.748 |

O programa de teste enviou toda a sequência planejada. A fila registrou zero estouros nos quatro jogos. Em This Week, o trace Maple observou 1.800 dos 1.804 eventos enviados. Faltaram uma pressão/soltura de Enter e uma de Shift direito. As outras três sessões observaram todos os eventos enviados. A comparação controlada da tabela usa os relatórios realmente observados.

As execuções Linux independentes também chegaram ao resultado e à reprise. Defy tem vídeo contínuo. Para recuperar as três outras em um ambiente com travas do OpenGL fora de tela, This Week foi gravada em EGL pbuffer com quadros pontuais. Bang Bang e Tutorial desativaram o desenho contínuo, preservando o processamento dos controles, a lógica do jogo e a saída PCM do SDL original. Os placares Linux foram 99.480, 127.509, 161.193 e 4.363 pontos, respectivamente. Os relatórios de entrada registram diferenças entre as sessões independentes. O teste da tabela usa os mesmos horários e estados das teclas.

A sonda comparou os quadros válidos do callback com o PCM original antes da fila. Na captura de vídeo, o backend SDL inseriu 45, 63, 449 e 990 blocos de silêncio durante jogo ativo, respectivamente. São cortes medidos no host de captura. Em novas partidas completas sem gravação contínua de vídeo, a mesma CDI registrou três blocos em jogo em This Week e três no tutorial. A faixa Dreamcast do MP4 contém a saída SDL original de Defy, com atraso audiovisual inicial de 144,17 ms. Os arquivos de prova preservam o PCM antes da fila para análise. Os ajustes dos stems originais em quatro janelas por faixa, com os PCM Linux e Dreamcast, também estão nas provas.

O [inventário](ESCOPO_LINUX.md) e a [matriz](PARIDADE.md) registram os recursos pendentes do jogo completo, incluindo editor, importadores, menus de configuração, prévia de músicas, localização e validação no console físico.

## Histórico até o checkpoint SH4ZAM

### SH4ZAM, leitura de CD, teclado e apresentação

A candidata parte de `62374a2e4f1136136ea9c8d051ce79b3500e9419` de `dreamcast-classic`. Usa o mesmo pacote Linux Frets on Fire 1.3.110 e os mesmos charts. A verificação da própria CDI confirmou os 26 arquivos nas quatro pastas de músicas.

| Arquivo da candidata de diagnóstico | SHA-256 |
| --- | --- |
| FretsOnFire_Dreamcast.elf | `6409775599d67a527db814f27315a9b6769d747ce77618244c7750962c92eb7e` |
| FretsOnFire_Dreamcast.cdi | `8d58e42aabfcc7dde779a8756d830215ede3f2df02847302466b4477f7c040ab` |
| Plano de teclado em Defy Amazing | `44045f428d1db20343ae31c782b9b5c9729838d8bccb155799a064374a3af14a` |

KallistiOS 2.2.1, GCC 15.1.0 SH-4, SH4ZAM 1.0.0 da toolchain, libvorbis e libogg do kos-ports. Para a compilação de diagnóstico, o comando do [README](README.md) recebeu `CXXFLAGS="-std=c++17 -Wall -Wextra -MMD -MP -DFRETSONFIRE_INPUT_TRACE"`. A instrumentação guarda relatórios e julgamentos em RAM, sem imprimir no caminho de cada palhetada. A automação lê esse diagnóstico e envia teclado por SDL e Maple.

#### Desempenho

A trigonometria de desenho usa a instrução FSCA pelo SH4ZAM. Os trastes têm vértices preparados para repouso e tecla pressionada. As malhas de notas reutilizam a projeção das posições compartilhadas sem alterar a ordem de seus vértices nem a cor por face. A geometria vai em lotes e por Store Queues, com envio de novo cabeçalho apenas quando muda o material ou a lista. A rotina de cache e os limites de lote seguem as técnicas usadas nos Test15 e Test17 de Super Mario World. [SH4ZAM](https://sh4zam.com/guides/getting-started/) · [Test15](https://github.com/olegne/dcioram-bros/blob/2ebe34099ec6fc6382af01b19bb59a899c901344/docs/DESEMPENHO_TEST15.md) · [Test17](https://github.com/olegne/dcioram-bros/blob/5f5e52d29727aa2e1e8df8d887f3f75879a933f2/docs/DESEMPENHO_TEST17.md)

| Ensaio isolado de 1.200 quadros em Flycast 2.7 | Antes | SH4ZAM, lotes e cache |
| --- | ---: | ---: |
| Submissão média do renderizador | 20.102 µs | 16.942 µs |
| Submissão p95 | 26.026 µs | 19.962 µs |
| Submissão máxima | 28.402 µs | 22.466 µs |
| Espera média medida em `pvr_wait_ready` | 680 µs | 1.661 µs |

Esse ensaio compara os algoritmos de submissão no mesmo chart e estado visual, com áudio e interface fora da simulação. O mesmo quadro comparativo teve diferença de até 2 em 255 por canal, com 1.620 de 307.200 pixels afetados. As alterações de aparência dos sustains e do resultado têm ensaios próprios. A cadência da partida completa e o desempenho no console têm medições próprias.

Um buffer de leitura de 16 KiB, alinhado a 32 bytes, permite que o ISO9660 do KallistiOS leia vários setores de uma vez por DMA. No ensaio isolado de 15 segundos com os dois stems de Defy, os comandos de CD durante a decodificação foram de 436 para 108. Os hashes do PCM decodificado foram idênticos. Os dois buffers usam 32 KiB enquanto a música está aberta. A preparação e o preenchimento inicial da AICA foram movidos para antes das oito batidas, mantendo a hora exata da entrada da faixa.

#### Comportamento reproduzido

- O teclado observa todos os relatórios `GETCOND` Maple completos, incluindo o Shift direito, e associa cada evento ao instante amostrado da AICA. O piloto de 36 segundos recebeu 622/622 transições e diagnosticou 170/170 alvos. As rotinas originais do Linux receberam esse traço Maple e o mesmo relógio. O resultado foi de 170 alvos, 173 gemas e 11.624 pontos nos dois motores, com pontuação idêntica nas 1.305 atualizações.
- No teste de pausa com o mesmo cursor de 7.821 ms, soltar as casas encerrou o sustain e mudou a pontuação de 550 para 633 nas duas implementações. A tecla F5 pressionada enquanto o menu estava aberto permaneceu fora do estado da guitarra.
- A forma de onda do sustain segue os passos adaptativos e os fins de nota por casa. As sete fixtures tiveram contagens de segmentos iguais ao código original e diferença de até 0,0088 pixel em uma projeção comum. As notas acertadas antecipadamente continuam visíveis até cruzarem a linha.
- A tela de resultado tem placar flamejado, entrada das estrelas em sequência, estatísticas e menu de ações à parte. O primeiro Enter abre as ações e o segundo confirma Replay. Diálogo de nome, recordes, contraste e parte da tipografia estão pendentes.
- Os nove efeitos de resultado são os Ogg estéreo originais de 44,1 kHz, com playback agendado após cinco segundos. O caminho usa RAM e canais AICA durante a reprodução. Um ensaio de pontuação zero confirmou a fala `jurgen1` de 5,5 segundos, incluindo áudio depois do limite anterior de 65.534 frames e coexistência com a música de menu e com a faixa da reprise. Os efeitos curtos ainda usam a conversão ADPCM de 22,05 kHz.

#### Partida integral e comparação das regras

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

#### Repetição com renderização em thread

A mesma CDI, o mesmo plano de teclado e os mesmos 6.144 quadros de áudio no SDL foram usados numa repetição completa com `rend.ThreadedRendering=yes` e sem fixação de CPUs. O backend registrou 14 faltas de amostras na inicialização e zero do início da música até a reprise. Os 8.590.336 quadros válidos do callback coincidiram byte a byte com a entrada da fila. A mediana da entrega do PCM ao callback SDL foi de 160,38 ms. A mudança de configuração inclui dois fatores do host, e a latência do Dreamcast físico é uma medição pendente.

O Flycast acertou 611 alvos e 658 gemas e fez 99.933 pontos e cinco estrelas nessa execução. As rotinas originais do Linux produziram os mesmos 611 IDs, gemas e pontos com os relatórios Maple e o relógio desta sessão. O placar coincidiu em 9.094 de 9.094 atualizações. O jogo registrou 9.093 quadros, 2.964 intervalos acima de 25 ms e 431 acima de 50 ms, com máximo de 109,258 ms. A nova gravação lado a lado usa esta sessão Dreamcast, com áudio original do SDL e atraso A/V inicial medido de 144,92 ms, e a sessão Linux ao vivo da tabela anterior. O arquivo de provas registra a configuração e os resultados das duas execuções Dreamcast.

### Histórico até o checkpoint anterior

#### Contagem, teclado e efeitos de palco

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

#### Histórico do checkpoint anterior

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
