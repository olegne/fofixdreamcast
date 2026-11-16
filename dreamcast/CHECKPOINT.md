# Checkpoint de guitarra Maple

## Compilação verificada

O checkpoint parte de `20113e16757ba82d0bd4b24fc8c1e0e447aff8ad`. Os arquivos em `src/` deste commit de desenvolvimento correspondem ao ELF testado abaixo. A compilação usou KallistiOS 2.2.1, GCC SH-4 15.1.0, libvorbis e libogg de kos-ports, as opções padrão do [Makefile](Makefile) e mkdcdisc. Os recursos em ROM disk vêm de [data/](../data) e de `tools/prepare_assets.py`.

| Arquivo de teste interno | SHA-256 |
| --- | --- |
| ELF do checkpoint | `38e939cba69de7b1834652e7812f8423a1081eaa0cfef6b89d316689e82f6739` |
| CDI com as três músicas oficiais do Frets on Fire 1.3.110 | `4113c6ee970e7289f5dff796e83ed2c8c79f6b92a7e79a594b9e76367a59da0d` |
| CDI da fixture sintética de teclado com o mesmo ELF | `8db901979ecb072c01fa5d132f4c57c9d9430f0ade7889f509ab3f26225e473a` |

| Defy The Machine, mídia usada em ambas as execuções | SHA-256 |
| --- | --- |
| `notes.mid` | `866487093c06144adb62e3d0a9b3b0b3330aac2464b05474df6054a020bf4395` |
| `song.ogg` | `ace9ac477e98de687b2062a899ec7e60e34b2e136b4ff1e866a952852b02b502` |
| `guitar.ogg` | `a9e18c6c8f157ef1e4e55292f3755055c23d7ed9fc757f5b066adb0679642afe` |
| `keyboard.json`, cronograma do replay | `be31ed9b4205757780a09126ce3953b4b91298107eed080c79c7396ebf7d14c1` |

Para reconstruir a CDI de teste, extrair `defy`, `bangbang` e `twibmpg` do pacote identificado na [referência de PC](REFERENCIA_PC.md). Compilar com `make -C dreamcast cdi SONGS_DIR=/caminho/para/songs`. Quaisquer diferenças de toolchain, recursos ou empacotamento exigem anotar os novos hashes.

## Testes executados

- **Regras internas.** Exportação do parser de `chart.cpp` em um executável de host e comparação ao chart carregado no PC. Defy Expert tem 614 alvos e 662 gemas. Casas coincidem, inícios e finais têm diferenças inferiores a 1 ms por arredondamento e 384 gemas marcadas HOPO coincidem com as classes de toque correspondentes do PC. Isso confirma a leitura desse chart. As decisões durante a partida divergem.
- **Integração Maple.** Flycast 2.7 com teclado na porta A e eventos SDL para pressionar e soltar teclas. Fixture de seis segundos com cinco casas simultâneas, Enter mantido, Shift direito, F10 alternativo, Page Up e Esc durante a partida. Ctrl esquerdo pausou em 1.782 ms, o cursor permaneceu em 78.604 amostras e a retomada iniciou a partir desse cursor depois de 3.008 ms. Resultado de 5 acertos, 0 erros, 0 palhetadas extras e 1.051 pontos. O replay começou na amostra zero.
- **CDI com música completa.** Flycast 2.7, modo NTSC 640 × 480, renderizador sem thread extra, nenhum ajuste de velocidade. A entrada SDL passou pelo teclado Maple emulado. Seleção de Defy e Expert pelos menus, reprodução até 167.213 ms, pausa por Ctrl, retomada por Enter, tela de resultado e nova partida. Em PC, teclado SDL, mixer Fretwork, tema e parâmetros da [referência](REFERENCIA_PC.md). As execuções usaram a mesma lista de 2.256 eventos de teclas em tempo musical. Ela inclui duas notas omitidas, uma palhetada deliberadamente errada e um sustain de cerca de 980 ms solto cedo. Na gravação final, o replay Maple deixou o relógio de jogo avançar entre duas transições da mesma tecla. Os logs preservam a hora prevista e a hora do envio de cada evento.
- **Dreamcast físico.** Validação pendente. Quando houver console, confirmar boot, reconhecimento do teclado, acorde de cinco casas, as duas palhetadas, Ctrl, retomada a partir do mesmo trecho e replay. Repetir com faixa completa e áudio estéreo vindo do console.

| Resultado de Defy Expert na gravação contínua | PC | Dreamcast no Flycast |
| --- | ---: | ---: |
| Alvos com acerto | 606 / 614 | 595 / 614 |
| Pontuação | 142.598 | 103.677 |
| Espera após acionar Resume | 3.054 ms | 3.026 ms |

A correção de espera reutiliza o PCM ainda enfileirado na AICA, com uma área de 32 KiB no SH-4. O buffer circular é reposicionado para começar na amostra registrada na pausa. A leitura prossegue do ponto corrente do Vorbis e preenche somente o espaço recuperado do bloco parcialmente consumido. A versão diagnóstica que fazia seek e decodificava de novo o buffer completo levou 4.770 ms para retomar Defy.

## Divergências demonstradas

- **Regras e entrada.** Na primeira gravação do ELF atual, o Flycast acertou 566 alvos. Em uma CDI instrumentada, as transições do primeiro alvo divergente, 5.409,833 ms, foram entregues ao SDL quando o jogo ainda marcava 5.447 ms. O estado de Enter voltou a solto antes da leitura do port. O alvo passou a pontuar quando o replay deixou o relógio do jogo avançar antes de soltar a tecla. A rodada completa com esse ajuste chegou a 595 acertos. No alvo de 8.974,380 ms, uma palhetada de 29 ms se perdeu mesmo com transições separadas. Em uma sonda com duração mínima de 60 ms, a janela efetiva de 102 ms permitiu que o port registrasse o acerto. As janelas, a recepção Maple e as regras de pontuação continuam em investigação. A duplicação do tutorial foi reproduzida no PC e está na [auditoria](REFERENCIA_PC.md). Defy não repetiu o mesmo objeto de nota na rodada de diagnóstico.
- **Imagem.** Nas capturas 640 × 480 ao redor de 11 s, a posição básica da pista coincide. O PC desenha um feixe de luz violeta no palco. O Dreamcast ainda tem chama, materiais dos trastes, indicação de erro, relógio e placar diferentes. A seleção é carrossel no PC e lista no port. O fundo da pausa do PC esconde a pista e o menu dispõe de seis ações.
- **Áudio.** Em região estável a 10 s, o ajuste por stem obtém ganho de acompanhamento/guitarra de 0,797/1,000 no PC e 0,805/1,000 no Flycast. Em 35,72 s, depois de soltar o sustain, a guitarra cai a 0,195 e 0,201 respectivamente. São medidas dos PCM originais, com busca de deslocamento por correlação, sem compensação de ganho. A normalização local da correlação entre a forma de onda e a combinação dos stems originais supera 0,999 nesses intervalos estáveis. Houve silêncio adicional e saltos de alinhamento em momentos fora da pausa na gravação Flycast, incluindo um trecho de cerca de 163 ms. A captura sem vídeo anterior também mostrou descontinuidades de alinhamento. Falta isolar o emulador, a captura e o caminho de streaming do port. O som de miss, o Killswitch e outras respostas ainda precisam de entradas equivalentes e verificação específica.
- **Carga.** O port registrou 7.019 quadros da partida ativa de Defy. É uma média aproximada de 42 quadros de jogo por segundo de música. Faltam os tempos por quadro, os piores intervalos e o pico de memória. A gravação com OpenGL por software e o teste sem vídeo são condições distintas e nenhum deles mede desempenho no console.

Próxima ação concreta. Instrumentar a recepção dos relatórios de teclado Maple ao redor do alvo de 8.974,380 ms, junto com a leitura de estado do jogo. Em paralelo, traçar posição e ocupação da fila de áudio antes da saída SDL do Flycast em uma rodada sem encoder. Esses registros permitem localizar a origem das perdas de tecla e dos cortes de som antes de alterar as regras.
