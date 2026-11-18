# Orientações para agentes

## Retomada e continuidade

Antes de editar, leia este arquivo e o [checkpoint do port](dreamcast/CHECKPOINT.md), incluindo a atualização mais recente e as fontes citadas. Confira no remoto o ramo de desenvolvimento e seu HEAD. Quando houver checkout acessível, confira também ramo, HEAD, status e diff locais; preserve alterações existentes e registre o que não pôde ser inspecionado.

O desenvolvimento atual do port clássico está em `dreamcast-classic`. Confirme essa escolha a cada retomada nas fontes atuais: `master` é o ramo padrão do fork e `dreamcast` conserva a etapa FoFiX. Use o HEAD atual como base, sem retornar a um checkpoint antigo apenas por ser conhecido.

Antes de interrupção, troca de sessão ou encerramento, atualize o checkpoint com:

- Data, objetivo e estado da tarefa: concluída, parada, bloqueada ou ainda em andamento, com escopo explícito.
- Ramo, commit e arquivos de base; árvore e hashes dos artefatos quando disponíveis.
- Alteração efetivamente realizada, arquivos afetados e commit resultante quando já conhecido.
- Verificação ou teste com evidência localizável, comando ou procedimento, resultado e ambiente; identifique separadamente host, Linux de referência, Flycast e Dreamcast físico.
- Pendências e próxima ação concreta, suficiente para retomar sem reconstruir a conversa.
- Em um bloqueio, causa demonstrada, erro ou log observado, tentativa realizada e alternativa ou condição necessária para prosseguir. Separe hipóteses de causas confirmadas.
- Se a tarefa parar, declare a parada e seu motivo, o último ponto confirmado e o que falta. Uma lista de intenção não equivale a trabalho executado.

Só descreva um processo como “rodando” quando tiver observado PID, job ou execução equivalente; anote identificador, ambiente e momento da observação. Sem essa evidência, use “execução não verificada”. Ao retomar, confira novamente: um registro antigo não confirma execução atual.

Diferencie implementação, leitura documental, teste de host, emulação, relato de uso e ensaio físico acompanhado. Um relato do usuário sobre o console deve ser identificado como relato, com data e revisão/hash conhecidos ou explicitamente ausentes. Ele não encerra sozinho a validação física detalhada nem a paridade.

Mantenha os checkpoints históricos e seus limites. Resultados de uma CDI, configuração ou ambiente não se estendem automaticamente a outra revisão, outro perfil do Flycast ou ao console.

## Referência do port e alterações remotas

A referência atual é o pacote Linux oficial Frets on Fire clássico 1.3.110 identificado em [REFERENCIA_PC.md](dreamcast/REFERENCIA_PC.md). Leia também a [matriz de paridade](dreamcast/PARIDADE.md), o [inventário do Linux](dreamcast/ESCOPO_LINUX.md) e o [README do port](dreamcast/README.md). Não transplante critérios da antiga etapa FoFiX sem conferir a referência escolhida.

Preserve as implementações existentes de prioridade do teclado Maple, LCD da VMU na porta B com teclado e rumble nos aumentos de multiplicador. Antes de mudar comportamento, consulte código e evidência atuais; não trate recursos já documentados como ausentes e não invente implementação a partir de pedidos antigos.

Para commits via API, crie a árvore sobre a árvore do HEAD observado, crie o commit com esse HEAD como pai e atualize a referência sem força, com `expected_sha` ou proteção equivalente. Se o ramo avançar, releia os novos arquivos/HEAD e refaça a alteração sobre a base atual; não sobrescreva trabalho concorrente.

Esta regra de continuidade não autoriza por si só executar builds, gerar CDI, publicar artefatos ou ampliar o escopo de uma tarefa. Registre exatamente o que foi autorizado e realizado.
