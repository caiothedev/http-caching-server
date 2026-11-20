# POLÍTICA DE SEGURANÇA - ᴘᴛ

Este documento descreve os limites de segurança e as precauções operacionais do HTTP Caching Proxy Server.

## Versões com suporte

As correções de segurança são direcionadas ao código mais recente da branch `main`. Atualmente, o projeto não publica uma matriz formal de suporte a versões. Ao reportar um problema, informe o SHA do commit ou a versão testada.

## Modelo de implantação

**Este servidor é um projeto de estudo e desenvolvimento, não um proxy de encaminhamento reforçado e pronto para produção.**

O endereço padrão de escuta é `127.0.0.1:8080`, limitando o acesso à própria máquina. Alterar o endereço de bind para `0.0.0.0` ou outra interface acessível externamente pode expor o proxy a todos os hosts que consigam alcançar essa interface.

Atualmente, o proxy não possui:

- Autenticação de clientes.
- Lista de controle de acesso por usuário ou endereço IP.
- Autorização por cliente, cotas ou limitação de taxa.
- Terminação TLS na conexão entre cliente e proxy.
- Lista de hosts ou endereços IP de destino permitidos para o encaminhamento HTTP comum.

Qualquer pessoa capaz de se conectar a um listener exposto pode enviar requisições através do proxy. Não o exponha diretamente à internet pública ou a uma rede não confiável.

## Restrições de CONNECT

O acesso HTTPS pelo proxy utiliza o método HTTP `CONNECT` para estabelecer um túnel TCP. Por padrão, somente a porta de destino `443` é permitida.

Use `--connect-ports` para configurar as portas de destino permitidas. O valor especial `any` remove a restrição de portas e não é recomendado para um serviço exposto.

A restrição de portas não é uma lista de destinos permitidos: ela não restringe nomes de host nem endereços IP de destino. Implantações que exigem controle de tráfego de saída devem aplicá-lo fora desta aplicação, por exemplo, com um firewall ou uma rede isolada.

Depois que o túnel é estabelecido, o proxy retransmite os bytes e não inspeciona nem armazena em cache o tráfego criptografado da aplicação.

## Considerações sobre HTTP e cache

- O encaminhamento HTTP comum não é criptografado entre o cliente e este proxy. Utilize uma rede confiável ou uma camada de transporte protegida quando essa conexão atravessar uma fronteira de rede.
- As respostas em cache são mantidas na memória do processo. O cache é compartilhado entre os clientes deste proxy; o armazenamento segue as regras implementadas para caches compartilhados, incluindo restrições para `private`, `no-store`, requisições autenticadas, `Set-Cookie` e `Vary: *`.
- Não trate o cache como uma fronteira de confidencialidade entre usuários mutuamente não confiáveis. Não habilite acesso público sem revisar os dados que podem passar pelo proxy.
- O endpoint `/__proxy/stats` não possui autenticação. Ele fornece contadores operacionais e estatísticas do cache a qualquer cliente capaz de alcançar o listener. A resposta utiliza `Cache-Control: no-store`.
- Os logs de acesso incluem o endpoint do cliente, método e URL da requisição, status da resposta, tamanho da resposta, resultado do cache e tempo decorrido. URLs podem conter informações sensíveis; proteja os logs adequadamente.

## Parser e limites de recursos

O parser HTTP impõe limites ao tamanho dos cabeçalhos, à quantidade de campos de cabeçalho e ao tamanho do corpo da mensagem. Os padrões são 64 KiB para a linha inicial mais os cabeçalhos, 100 campos de cabeçalho e 64 MiB para os corpos. O limite efetivo do corpo é aumentado quando necessário para comportar o tamanho máximo configurado para um objeto em cache.

O parser rejeita enquadramentos ambíguos com `Transfer-Encoding` e `Content-Length` simultâneos, enquadramentos malformados, codificações de transferência não suportadas e atualizações de protocolo não suportadas. Somente a codificação de transferência `chunked` é implementada. Essas verificações reduzem ambiguidades de interpretação, mas não substituem o isolamento de rede nem uma revisão de segurança.

O cache possui capacidade total e tamanho máximo por objeto configuráveis. Esses valores limitam a memória usada pelo armazenamento em cache, mas não representam um limite completo de memória do processo: conexões ativas e corpos de requisições/respostas em processamento também consomem memória.

## Lista de verificação de implantação

- Mantenha o listener em `127.0.0.1`, a menos que o acesso remoto seja explicitamente necessário.
- Se o acesso remoto for necessário, coloque o proxy atrás de uma fronteira de rede confiável e aplique autenticação, autorização e controles de acesso à rede externamente.
- Restrinja o tráfego de saída com regras de firewall; não confie em `--connect-ports` como uma lista de hosts ou IPs permitidos.
- Mantenha a restrição padrão da porta CONNECT, a menos que outras portas sejam necessárias.
- Configure `--cache-size` e `--max-object` de acordo com a memória disponível e o tráfego esperado.
- Proteja os logs e evite registrar ou compartilhar URLs que contenham credenciais ou tokens privados.
- Execute o processo somente com as permissões de sistema operacional e de rede necessárias.
- Mantenha o compilador, Boost, CMake e demais dependências de build atualizados.

## Reportar uma vulnerabilidade

Não divulgue uma vulnerabilidade ainda não corrigida em uma issue pública.

Utilize o recurso privado de reporte de vulnerabilidades do GitHub para este repositório, caso esteja habilitado. Caso contrário, entre em contato com o mantenedor do repositório em particular por um canal listado no perfil do mantenedor no GitHub.

Inclua, sempre que possível:

- Uma descrição concisa do impacto e do componente afetado.
- O SHA do commit, compilador, sistema operacional e opções de build.
- Etapas de reprodução ou uma prova de conceito mínima.
- Logs relevantes com segredos, credenciais e dados pessoais removidos.

Não inclua credenciais válidas, dados privados de usuários ou payloads de exploração que afetem sistemas que você não possui ou não tem autorização para testar.

## Escopo e limitações

Esta política documenta a implementação atual e seus riscos operacionais. Ela não garante que o software esteja livre de vulnerabilidades e não constitui certificação de segurança nem declaração de prontidão para produção.

by caiothedev<br />
#security