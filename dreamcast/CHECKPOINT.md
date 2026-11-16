# Checkpoint do Frets on Fire clássico

## Proveniência

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

## O que mudou

- Menu, logo, fontes, cassetes, palco, câmera, pista, casas, notas e HUD baseados nos recursos originais de 1.3.110. A imagem é renderizada no PowerVR em 640 × 480.
- Biblioteca com as músicas do pacote. A dificuldade usa os nomes Amazing, Medium, Easy e Supaeasy quando disponível. Tutorial pelo menu próprio.
- Entrada Maple com F1 a F5, Enter e Shift direito. Os relatórios de teclado são enfileirados durante a partida. A rotina de teclado do KOS captura mudanças nas teclas comuns, e a rotina de VBlank amostra também o Shift.
- A/V em 100 ms, oito batidas de contagem inicial, Esc para pausar e retomar. Enter em Return to Song resume no port. O original usa Esc para fechar a pausa e dispara a retomada no fim da transição.
- Ganhos iniciais de acompanhamento e guitarra em 1,0. Erros de guitarra silenciam o instrumento. Um novo efeito de palhetada errada substitui o anterior em canais AICA dedicados. Final de música baseado na trilha de acompanhamento.
- Novas regras de acordes, tapping, HOPO, multiplicador, sustain, janela de palhetada sem casa, contagem de gemas e estrelas derivadas da implementação original.

## Validação interna

Os quatro charts oficiais totalizam 4.606 gemas em todos os níveis. Dificuldade, casa e marcação de HOPO coincidiram por evento com as classes de leitura do pacote. As diferenças de começo e fim foram menores que 0,001 ms para esses charts.

Em um relógio comum de 5 ms, o teste de regras C++ e o original de PC processaram 828 transições decisórias iguais de acertos, pontuação, sequência e ganho. O teste usa os handlers do jogo original e isola áudio, vídeo e transporte de entrada. Uma fixture adicional verificou acordes na janela de acerto, janela de 30 ms, pulso mantido, omissão, palhetada tardia e corte de sustain.

## Integração com música completa

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

## Pendências

- Medir relatórios Maple e eventos da primeira divergência restante com o mesmo ponto musical e determinar quanto vem da entrada. Pulsos que o teclado e o barramento nunca amostram seguem fora da fila. O Esc ainda é lido no laço principal.
- Alinhar a contagem visual ao atraso A/V desde o início. No original, Esc também pode pausar a contagem. O port usa Esc para sair dessa etapa.
- Completar a animação das luzes, a forma de onda dos sustains, as transições das fitas e as telas de recordes, bem como as opções da [matriz de paridade](PARIDADE.md).
- Capturar o PCM antes do backend SDL do Flycast e no Dreamcast físico. Verificar boot, acorde de cinco teclas, alternância de palheta, Esc, sustain interrompido e Replay em uma música completa.
