# DOCUMENTAÇÃO - ᴘᴛ

Guia de como compilar, executar e testar o proxy.

- [Arquitetura](ARCHITECTURE.pt-br.md)
- [Como contribuir](../CONTRIBUTING.pt-br.md)
- [English version](README.md)

## Sumário

1. [Pré-requisitos](#1-pré-requisitos)
2. [Compilação](#2-compilação)
3. [Executando](#3-executando)
4. [Opções de linha de comando](#4-opções-de-linha-de-comando)
5. [Usando o proxy](#5-usando-o-proxy)
6. [Testes](#6-testes)
7. [Integração contínua](#7-integração-contínua)
8. [Solução de problemas](#8-solução-de-problemas)

## 1. Pré-requisitos

| Ferramenta | Versão mínima | Observação |
| --- | --- | --- |
| Compilador C++17 | GCC 9 / Clang 10 | testado com GCC 13 e Clang 18 |
| CMake | 3.16 | |
| Boost | 1.70 | apenas headers (Boost.Asio); nada para linkar |
| Git + acesso à internet | — | o CMake baixa o Catch2 v3 ao configurar com testes |
| Ninja | opcional | builds mais rápidos (`-G Ninja`) |

Instalação das dependências:

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake ninja-build libboost-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build boost-devel

# Arch Linux
sudo pacman -S base-devel cmake ninja boost

# macOS (Homebrew)
brew install cmake ninja boost
```

## 2. Compilação

Build de release com testes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Isso gera dois executáveis em `build/`:

- `http_proxy`: o servidor;
- `proxy_tests`: os testes unitários.

### Opções do CMake

| Opção | Padrão | Efeito |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Debug` | `Debug`, `Release`, `RelWithDebInfo` |
| `PROXY_BUILD_TESTS` | `ON` | compila `proxy_tests` (baixa o Catch2) |
| `PROXY_ENABLE_SANITIZERS` | `OFF` | compila com AddressSanitizer + UndefinedBehaviorSanitizer |
| `CMAKE_COMPILE_WARNING_AS_ERROR` | `OFF` | transforma warnings em erros (usado no CI) |

Build sem testes (não precisa de internet):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPROXY_BUILD_TESTS=OFF
cmake --build build -j
```

Build com sanitizers, recomendado durante o desenvolvimento:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
```

> Com Clang no Linux, os sanitizers precisam do `compiler-rt` (`sudo apt install libclang-rt-dev` no Ubuntu).

## 3. Executando

```bash
./build/http_proxy
```

Por padrão o proxy escuta em `127.0.0.1:8080`, usa uma thread por CPU e um cache de 64 MiB. Saída esperada:

```
2026-01-01 12:00:00.000 [INFO ] [t1234] proxy listening on 127.0.0.1:8080 with 8 threads, cache 64 MiB
```

Cada requisição gera uma linha de log no formato:

```
[id-da-sessão] cliente "MÉTODO URL" status bytes X-Cache duração
[2] 127.0.0.1:52006 "GET http://example.com/" 200 1256B HIT 0ms
```

`Ctrl+C` (SIGINT) ou SIGTERM encerram o servidor, que imprime as estatísticas finais do cache.

## 4. Opções de linha de comando

| Opção | Padrão | Descrição |
| --- | --- | --- |
| `-l, --listen ADDR` | `127.0.0.1` | endereço de escuta (IPv4 ou IPv6) |
| `-p, --port N` | `8080` | porta de escuta |
| `-t, --threads N` | nº de CPUs | threads do pool que executam o `io_context` |
| `--cache-size MB` | `64` | tamanho total do cache em MiB |
| `--max-object MB` | `8` | maior resposta que pode ser guardada, em MiB |
| `--no-cache` | — | desativa o cache (proxy de encaminhamento puro) |
| `--timeout SEC` | `30` | prazo para os headers da resposta da origem e para cada espera por dados da origem |
| `--pool-size N` | `32` | conexões keep-alive ociosas mantidas com as origens (`0` desativa o reuso) |
| `--pool-idle-timeout SEC` | `15` | por quanto tempo uma conexão ociosa com a origem é mantida |
| `--idle-timeout SEC` | `60` | timeout de um cliente keep-alive ocioso |
| `--tunnel-timeout SEC` | `300` | timeout de inatividade de um túnel CONNECT |
| `--connect-ports LIST` | `443` | portas permitidas para CONNECT, separadas por vírgula, ou `any` |
| `--allow-hosts LIST` | qualquer | hosts de destino permitidos (`*.example.com` cobre subdomínios) |
| `--block-private` | — | recusa destinos de loopback/privados/link-local |
| `--log-level LEVEL` | `info` | `debug`, `info`, `warn` ou `error` |
| `-h, --help` | — | mostra a ajuda |

As opções aceitam tanto `--port 8080` quanto `--port=8080`.

Exemplo:

```bash
./build/http_proxy --port 3128 --threads 4 --cache-size 256 --max-object 16 --log-level debug
```

> **Segurança:** `--listen 0.0.0.0` expõe o proxy para toda a rede. Não há autenticação: qualquer pessoa que alcance a porta pode usá-lo como proxy aberto. Use apenas em redes confiáveis. Restrinja os destinos com `--allow-hosts` e `--block-private`.

## 5. Usando o proxy

### Com curl

```bash
# Primeira requisição: X-Cache: MISS
curl -x http://127.0.0.1:8080 -i http://example.com/

# Segunda requisição: X-Cache: HIT, com header Age
curl -x http://127.0.0.1:8080 -i http://example.com/

# Forçar revalidação com a origem
curl -x http://127.0.0.1:8080 -i -H 'Cache-Control: no-cache' http://example.com/

# Responder só a partir do cache (504 se não estiver lá)
curl -x http://127.0.0.1:8080 -i -H 'Cache-Control: only-if-cached' http://example.com/outra

# HTTPS passa por um túnel CONNECT (não é cacheado)
curl -x http://127.0.0.1:8080 https://example.com/
```

### Com variáveis de ambiente

```bash
export http_proxy=http://127.0.0.1:8080
export https_proxy=http://127.0.0.1:8080
curl -i http://example.com/
```

### No navegador

Configure um proxy HTTP manual apontando para `127.0.0.1` porta `8080`, usando o mesmo proxy para HTTPS. No Firefox: *Configurações → Rede → Configurar conexão*.

### Header `X-Cache`

| Valor | Significado |
| --- | --- |
| `HIT` | resposta servida do cache, sem contato com a origem |
| `MISS` | resposta buscada na origem (e guardada se a política permitir) |
| `REVALIDATED` | entrada antiga confirmada pela origem com `304 Not Modified` |

Respostas que não são de um `GET` não recebem `X-Cache`.

### Endpoint de estatísticas

```bash
curl http://127.0.0.1:8080/__proxy/stats
```

```json
{
  "connections": 12,
  "active_connections": 1,
  "requests": 20,
  "tunnels": 2,
  "upstream_errors": 0,
  "client_aborts": 0,
  "blocked_requests": 0,
  "rejected_connections": 0,
  "upstream_pool": {
    "idle": 1,
    "reused": 7
  },
  "cache": {
    "enabled": true,
    "entries": 5,
    "bytes": 48213,
    "capacity": 67108864,
    "hits": 9,
    "misses": 6,
    "revalidations": 1,
    "stores": 5,
    "evictions": 0,
    "invalidations": 0
  }
}
```

### Códigos de erro gerados pelo proxy

| Status | Quando |
| --- | --- |
| `400` | requisição malformada, alvo que não é `http://` absoluto, ou CONNECT sem `host:porta` |
| `403` | CONNECT para uma porta fora de `--connect-ports`, ou destino recusado por `--allow-hosts` / `--block-private` |
| `503` | o limite de `--max-connections` foi atingido |
| `413` | corpo maior que o limite |
| `431` | headers maiores que 64 KiB ou mais de 100 headers |
| `502` | origem não resolve, recusa a conexão ou envia resposta inválida |
| `504` | os headers da origem não chegaram dentro de `--timeout`, ou `only-if-cached` sem entrada no cache |
| `505` | versão HTTP não suportada |
| `508` | loop detectado (o proxy recebeu uma requisição que já passou por ele) |

## 6. Testes

### Testes unitários

```bash
ctest --test-dir build --output-on-failure
```

Ou diretamente pelo executável do Catch2, que permite filtrar por nome ou tag:

```bash
./build/proxy_tests                    # todos
./build/proxy_tests "[parser]"         # só uma tag
./build/proxy_tests --list-tests       # lista os casos
```

| Arquivo | Cobre |
| --- | --- |
| `tests/test_parser.cpp` | parser HTTP/1.1: framing, chunked, pipelining, limites, erros |
| `tests/test_url.cpp` | parsing e normalização de URLs, resolução de referências |
| `tests/test_date.cpp` | datas HTTP (IMF-fixdate, RFC 850, asctime) |
| `tests/test_lru_cache.cpp` | LRU limitado por custo, evicção |
| `tests/test_cache_control.cpp` | parsing de diretivas `Cache-Control` |
| `tests/test_http_cache.cpp` | política RFC 9111: armazenamento, frescor, revalidação, `Vary` |
| `tests/test_config.cpp` | parsing e validação da linha de comando |
| `tests/test_access.cpp` | faixas de endereços não públicos e correspondência da lista de hosts |
| `tests/test_upstream_pool.cpp` | reuso de conexões ociosas, detecção de conexões fechadas/sujas, expiração e limite de tamanho |
| `tests/test_proxy.cpp` | o proxy rodando de ponta a ponta: cache, revalidação, pipelining, `100-continue`, cliente que desiste, `503`, política de acesso, CONNECT, encerramento gracioso, streaming (grande, chunked, delimitado pelo fechamento, backpressure, cache durante o streaming), pool de conexões e nova tentativa |

### Smoke test (ponta a ponta)

`scripts/smoke_test.sh` sobe uma origem local (`python3 -m http.server`) e o proxy, e verifica encaminhamento, `MISS → HIT`, o endpoint de estatísticas, a política de portas do CONNECT e o erro `502`:

```bash
scripts/smoke_test.sh build/http_proxy
```

Requer `bash`, `curl` e `python3`. As portas podem ser trocadas com `PROXY_PORT` e `ORIGIN_PORT`.

## 7. Integração contínua

O workflow [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) roda em todo push na `main`, em todo pull request e manualmente (*workflow_dispatch*). Ele executa, em Ubuntu 24.04, uma matriz de quatro jobs:

| Compilador | Build | Sanitizers |
| --- | --- | --- |
| GCC | Release | — |
| Clang | Release | — |
| GCC | Debug | ASan + UBSan |
| Clang | Debug | ASan + UBSan |

Cada job compila com warnings tratados como erros, roda os testes unitários e o smoke test. Para reproduzir um job localmente:

```bash
CXX=clang++ cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build
ctest --test-dir build --output-on-failure
scripts/smoke_test.sh build/http_proxy
```

## 8. Solução de problemas

**`Could NOT find Boost`**: instale o pacote de desenvolvimento do Boost (`libboost-dev`, `boost-devel`, `boost`). Se estiver num caminho fora do padrão, passe `-DBOOST_ROOT=/caminho/do/boost`.

**Falha ao baixar o Catch2**: a configuração com testes precisa de acesso ao GitHub. Sem internet, use `-DPROXY_BUILD_TESTS=OFF`.

**`bind: Address already in use`**: outra aplicação usa a porta. Escolha outra com `--port`.

**O curl não passa pelo proxy ao acessar `localhost`/`127.0.0.1`**: a variável `NO_PROXY`/`no_proxy` tem precedência sobre `-x`. Use `curl --noproxy '' -x ...`.

**`400 only http:// URLs can be proxied`**: o cliente mandou uma URL `https://` sem CONNECT. Configure o cliente para usar o proxy também para HTTPS.

**`403 CONNECT to port N is not allowed`**: inclua a porta em `--connect-ports` (por exemplo `--connect-ports 443,8443`).

**Linker não encontra `libclang_rt.asan`**: instale o `compiler-rt` do Clang (`libclang-rt-dev` no Ubuntu) ou use GCC.

by caiothedev<br />
#docs