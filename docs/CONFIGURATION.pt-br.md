# CONFIGURAÇÃO - ᴘᴛ

Este documento lista as opções de linha de comando, valores padrão, valores aceitos e comportamento em execução do HTTP Caching Proxy Server.

- [Documentação do projeto](README.md)
- [Arquitetura](ARCHITECTURE.en.md)
- [English](CONFIGURATION.en.md)

## 1. Início rápido

Execute com a configuração padrão:

```bash
./build/http_proxy
```

Exemplo com configurações explícitas:

```bash
./build/http_proxy --listen 127.0.0.1 --port 3128 --threads 4 \
  --cache-size 256 --max-object 16 --timeout 30 \
  --idle-timeout 60 --tunnel-timeout 300 --connect-ports 443 \
  --log-level info
```

Opções que recebem um valor aceitam as duas formas:

```bash
--port 8080
--port=8080
```

## 2. Opções de linha de comando

| Opção | Padrão | Valores / intervalo aceitos | Descrição |
| --- | --- | --- | --- |
| `-l, --listen ADDR` | `127.0.0.1` | Endereço IPv4 ou IPv6 | Endereço local no qual o servidor escuta. |
| `-p, --port N` | `8080` | 1–65535 | Porta TCP de escuta. |
| `-t, --threads N` | Quantidade de CPUs | 1–1024 | Número de threads que executam o loop de eventos do Asio. |
| `--cache-size MB` | `64` | 1–1.048.576 MiB | Capacidade total do cache. |
| `--max-object MB` | `8` | 1–1.048.576 MiB | Tamanho estimado máximo de uma entrada do cache. |
| `--no-cache` | desabilitado | flag | Desabilita consulta e armazenamento em cache; as requisições são encaminhadas. |
| `--timeout SEC` | `30` | 1–3600 segundos | Timeout de conexão/leitura com a origem. |
| `--idle-timeout SEC` | `60` | 1–3600 segundos | Timeout de inatividade da conexão keep-alive do cliente. |
| `--tunnel-timeout SEC` | `300` | 1–86400 segundos | Timeout de inatividade do túnel CONNECT. |
| `--connect-ports LIST` | `443` | Portas de 1–65535 separadas por vírgula, ou `any` | Portas de destino permitidas para CONNECT. |
| `--log-level LEVEL` | `info` | `debug`, `info`, `warn`, `error` | Severidade mínima dos logs. |
| `-h, --help` | — | flag | Exibe as informações de uso e encerra o programa. |

## 3. Padrões e valores derivados

Quando `--threads` é omitido, o servidor utiliza a quantidade de threads de hardware informada pelo sistema, com o mínimo de uma thread de trabalho.

Os padrões do cache são:

- Capacidade total: 64 MiB.
- Tamanho máximo por entrada: 8 MiB.
- Tempo máximo de frescor heurístico: 24 horas (padrão interno; atualmente não é exposto como opção de linha de comando).

Os padrões do parser HTTP são:

- Tamanho máximo da linha inicial mais a seção de cabeçalhos: 64 KiB.
- Quantidade máxima de campos de cabeçalho: 100.
- Tamanho máximo do corpo da mensagem: 64 MiB.

O limite efetivo do corpo é aumentado quando o tamanho máximo configurado para uma entrada do cache é maior que o limite padrão do parser. O tamanho máximo por entrada também é limitado à capacidade total do cache quando for maior que ela.

## 4. Política de portas CONNECT

Por padrão, requisições CONNECT podem alcançar somente a porta de destino `443`.

Por exemplo, para permitir as portas 443 e 8443:

```bash
./build/http_proxy --connect-ports 443,8443
```

Para permitir qualquer porta de destino:

```bash
./build/http_proxy --connect-ports any
```

Uma lista de portas vazia é interpretada internamente como permissão para qualquer porta. A configuração `any` não é recomendada para um proxy exposto. Esta opção restringe somente as portas; ela não fornece uma lista de hosts ou endereços IP de destino permitidos.

## 5. Configuração do cache

Use `--cache-size` para definir a capacidade total do cache em memória e `--max-object` para definir o tamanho máximo de uma entrada individual.

```bash
./build/http_proxy --cache-size 512 --max-object 32
```

Os valores são especificados em MiB. O parser aceita cada valor entre 1 e 1.048.576 MiB. Se `--max-object` exceder `--cache-size`, ele será reduzido à capacidade total do cache.

Desabilite completamente o cache com:

```bash
./build/http_proxy --no-cache
```

Consulte [CACHING.md](CACHING.en.md) para conhecer as regras de armazenamento, cálculo de frescor, revalidação e remoção de entradas.

## 6. Logs

O nível padrão é `info`.

- `debug`: diagnósticos detalhados de conexão, encaminhamento e cache.
- `info`: informações de inicialização e linhas de acesso das requisições.
- `warn`: problemas recuperáveis e condições rejeitadas.
- `error`: erros fatais.

As linhas de log de acesso incluem o endpoint do cliente, método e URL da requisição, status da resposta, tamanho da resposta, resultado do cache e tempo decorrido. Trate os logs como potencialmente sensíveis, pois URLs podem conter dados privados.

## 7. Validação e erros

Valores ausentes ou inválidos, opções desconhecidas e valores numéricos fora do intervalo fazem o programa exibir uma mensagem de erro e o texto de uso, encerrando com o código de saída `2`.

O endereço de escuta é interpretado quando o servidor é iniciado. Um endereço inválido ou uma falha na configuração do socket é reportado como erro de inicialização.

Utilize:

```bash
./build/http_proxy --help
```

para exibir as opções disponíveis.

## 8. Observações de segurança

O listener padrão é `127.0.0.1`. Vincular o servidor a `0.0.0.0` expõe o proxy em todas as interfaces IPv4. O servidor não possui autenticação de clientes nem lista de controle de acesso integrada.

Não exponha o proxy a uma rede não confiável sem controles externos de rede. Consulte [SECURITY.md](../SECURITY.pt-br.md) antes de alterar o endereço de escuta ou a política de CONNECT.

by caiothedev<br />
#configuration