# CACHE HTTP - ᴘᴛ

Este documento descreve o cache HTTP compartilhado em memória implementado pelo proxy. O foco é o comportamento real do projeto, não todas as funcionalidades definidas pela RFC 9111.

- [Documentação do projeto](README.md)
- [Arquitetura](ARCHITECTURE.en.md)
- [English](CACHING.en.md)

## 1. Visão geral

O cache é implementado por `HttpCache` e por um `LruCache` limitado por bytes. Ele é compartilhado por todas as sessões de clientes em um único processo do servidor e protegido por um mutex.

- Chave do cache: URL HTTP absoluta normalizada, incluindo caminho e query.
- Dados armazenados: status, motivo, cabeçalhos e corpo decodificado da resposta.
- Armazenamento: somente na memória do processo; as entradas são perdidas quando o processo termina.
- Remoção: LRU (menos recentemente usado), limitada pelo tamanho total estimado das entradas.
- Escopo: cache compartilhado; não há separação por identidade do cliente.

O tráfego HTTPS transportado por `CONNECT` é um túnel TCP opaco e não é armazenado em cache.

## 2. Elegibilidade das requisições

Somente requisições `GET` comuns são consultadas no cache. Requisições que contêm cabeçalhos condicionais do cliente (como `If-None-Match`, `If-Modified-Since`, `If-Match`, `If-Unmodified-Since`, `If-Range` ou `Range`) ignoram a consulta ao cache e são encaminhadas à origem.

Uma requisição com `Cache-Control: no-store` não é elegível para armazenamento. Uma requisição com `Cache-Control: no-cache` ou, quando `Cache-Control` estiver ausente, `Pragma: no-cache` exige validação de uma entrada existente.

## 3. Regras de armazenamento das respostas

Uma resposta só é armazenada quando todas as condições aplicáveis são atendidas:

- O método da requisição é `GET`.
- O status da resposta não é informativo (`1xx`), conteúdo parcial (`206`) ou `304`.
- Nem a requisição nem a resposta possuem `Cache-Control: no-store`.
- A resposta não possui `Cache-Control: private`, pois este é um cache compartilhado.
- Se a requisição contém `Authorization`, a resposta deve permitir explicitamente o armazenamento compartilhado com `public`, `s-maxage` ou `must-revalidate`.
- Uma resposta que contém `Set-Cookie` só é armazenada quando também possui `Cache-Control: public`.
- Respostas com `Vary: *` nunca são armazenadas.
- A resposta deve possuir informação explícita de frescor (`max-age`, `s-maxage`, `Expires` ou `public`) ou um status elegível para cache heurístico acompanhado de um validador `ETag` ou `Last-Modified`.
- A resposta deve caber no tamanho máximo configurado para um objeto, incluindo a estimativa de metadados do cache.

Os status elegíveis por padrão para cache heurístico são `200`, `203`, `204`, `300`, `301`, `308`, `404`, `405`, `410`, `414` e `501`. Sem informação explícita de frescor, também é necessário um validador.

## 4. Frescor e idade

O tempo de frescor é selecionado nesta ordem:

1. `s-maxage`
2. `max-age`
3. `Expires - Date` (ou o horário de recebimento da resposta quando `Date` estiver ausente)
4. Um tempo heurístico igual a 10% de `Date - Last-Modified`, somente para os status listados acima e limitado pelo máximo heurístico configurado.

Uma data `Expires` inválida resulta em tempo de frescor igual a zero. O limite heurístico padrão é de 24 horas.

A idade atual é calculada usando a idade inicial corrigida e o tempo decorrido desde o recebimento da resposta. A idade inicial corrigida considera os campos `Date` e `Age` da resposta e o atraso da resposta.

Uma entrada é considerada fresca enquanto sua idade atual for menor que seu tempo de frescor, respeitando também as diretivas da requisição. Uma entrada marcada com `no-cache` precisa ser validada antes de ser reutilizada. `must-revalidate`, `proxy-revalidate` e `s-maxage` impedem o fornecimento de uma resposta obsoleta no tratamento implementado de `max-stale`.

## 5. Diretivas da requisição

A implementação reconhece estas diretivas de requisição:

| Diretiva | Comportamento implementado |
| --- | --- |
| `no-cache` | Exige validação antes de reutilizar uma entrada existente. |
| `no-store` | Impede o armazenamento da resposta desta requisição. |
| `max-age=N` | Rejeita uma entrada cuja idade atual seja maior ou igual a N segundos. |
| `min-fresh=N` | Exige que restem pelo menos N segundos de frescor. |
| `max-stale` | Permite uma entrada obsoleta quando a resposta não exige revalidação. |
| `max-stale=N` | Permite obsolescência de até N segundos sob a mesma restrição. |
| `only-if-cached` | Não contata a origem em caso de cache miss; o proxy retorna `504`. |

Para compatibilidade com HTTP/1.0, `Pragma: no-cache` é tratado como `no-cache` somente quando a requisição não possui o campo `Cache-Control`.

## 6. Diretivas e metadados da resposta

O parser reconhece `no-cache`, `no-store`, `no-transform`, `max-age`, `s-maxage`, `must-revalidate`, `proxy-revalidate`, `public` e `private`. Diretivas desconhecidas são ignoradas. As formas qualificadas `no-cache="..."` e `private="..."` são tratadas como suas formas não qualificadas.

O cache também utiliza:

- `Date`, `Age` e `Expires` para calcular idade e frescor.
- `ETag` e `Last-Modified` como validadores de revalidação.
- `Vary` para associar uma entrada aos valores dos cabeçalhos da requisição usados para selecionar a representação.

Para `Vary`, o cache armazena o valor combinado de cada cabeçalho de requisição indicado e o verifica durante a consulta. Uma divergência resulta em cache miss.

## 7. Revalidação

Quando uma entrada está obsoleta ou exige validação, o proxy envia uma requisição condicional à origem usando os valores armazenados de `ETag` e/ou `Last-Modified`:

- `If-None-Match` para `ETag`.
- `If-Modified-Since` para `Last-Modified`.

Se a origem retornar `304 Not Modified`, o proxy combina os cabeçalhos aplicáveis da resposta com a resposta armazenada, recalcula os metadados do cache e entrega o corpo armazenado. O cliente recebe a representação original com um cabeçalho `Age` atualizado e `X-Cache: REVALIDATED`.

Se a origem retornar uma nova resposta, ela será avaliada para armazenamento e substituirá ou invalidará a entrada obsoleta conforme apropriado.

## 8. Invalidação

Após uma requisição insegura bem-sucedida (um método diferente de `GET` ou `HEAD`, com status de resposta menor que `400`), o proxy invalida:

- A entrada de cache correspondente ao destino da requisição.
- URIs de mesma origem referenciadas nos campos `Location` e `Content-Location` da resposta, quando esses valores puderem ser resolvidos.

O cache não persiste entradas entre reinicializações.

## 9. Capacidade e remoção de entradas

A capacidade total padrão do cache é de 64 MiB e o tamanho máximo padrão por objeto é de 8 MiB. Ambos podem ser configurados pela linha de comando.

O LRU acompanha bytes estimados, incluindo o corpo, o motivo da resposta, os nomes e valores dos cabeçalhos e uma sobrecarga fixa por entrada. Quando a inserção de uma entrada ultrapassaria a capacidade total, as entradas menos recentemente usadas são removidas até que a nova entrada caiba. Uma entrada maior que o tamanho máximo por objeto ou que a capacidade total não é armazenada.

O tamanho máximo por objeto é limitado à capacidade total durante a configuração.

## 10. Indicadores visíveis ao cliente

| Cabeçalho | Valor | Significado |
| --- | --- | --- |
| `X-Cache` | `HIT` | Uma resposta armazenada e fresca foi entregue sem contato com a origem. |
| `X-Cache` | `MISS` | A requisição foi buscada na origem; o armazenamento é tentado quando o cache está habilitado. |
| `X-Cache` | `REVALIDATED` | Uma entrada obsoleta foi validada pela origem com `304 Not Modified`. |
| `Age` | segundos | Idade atual calculada de uma resposta entregue pelo cache. |

Respostas a métodos diferentes de `GET` não recebem o cabeçalho `X-Cache`. Um `MISS` não garante que a resposta tenha sido armazenada.

## 11. Limitações

- O cache é mantido em memória e é local a um processo; não há persistência em disco nem cache distribuído.
- Somente respostas a requisições `GET` são armazenadas.
- Túneis `CONNECT` não são armazenados nem inspecionados.
- Requisições condicionais/de intervalo do cliente ignoram a consulta ao cache.
- Esta é uma implementação deliberadamente limitada do comportamento de cache compartilhado, não uma declaração de cobertura completa da RFC 9111.

by caiothedev<br />
#caching