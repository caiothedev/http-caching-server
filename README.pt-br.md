# SERVIDOR PROXY DE CACHING HTTP - ᴘᴛ

Um servidor proxy HTTP com sistema de cache desenvolvido em C++ como projeto de estudos, com foco em programação de redes, comunicação via sockets TCP, concorrência e gerenciamento de requisições HTTP.

## Objetivos

- Desenvolver um servidor proxy de cache HTTP em C++ capaz de processar requisições dos tipos GET, POST e CONNECT.
- Utilizar programação com sockets TCP baseada na biblioteca Boost para o envio e recebimento de pacotes, e tratar requisições simultâneas provenientes de diferentes origens por meio de multithreading.
- Compreender os fundamentos dos protocolos HTTP/1.1 e TCP/IP.

## Stack

- **C++17**
- **Boost.Asio**: I/O assíncrono sobre sockets TCP, com um `io_context` executado por um pool de threads e um *strand* por conexão
- **CMake** (≥ 3.16)
- **Catch2 v3**: testes unitários, baixado automaticamente pelo CMake

## Funcionalidades

- **GET / POST / PUT / DELETE / ...**: requisições em formato absoluto (`GET http://host/path HTTP/1.1`) são encaminhadas para a origem no formato de origem (*origin form*)
- **CONNECT**: túnel TCP bidirecional (HTTPS) com fechamento parcial (*half-close*) e tempo limite de inatividade (*idle timeout*); as portas permitidas são configuráveis ​​(padrão: 443)
- **Analisador (*parser*) HTTP/1.1 incremental implementado manualmente**: suporta `Content-Length`, codificação por blocos (*chunked encoding*), corpos lidos até o fechamento da conexão, respostas intermediárias (1xx), *pipelining*, validação rigorosa de CRLF/cabeçalhos e limites de tamanho
- O suporte a codificações de transferência (*transfer coding*) é intencionalmente limitado apenas ao tipo `chunked`; combinações de `Transfer-Encoding` não suportadas ou ambíguas são rejeitadas
- O cabeçalho `Expect` suporta apenas o valor `100-continue`; outras expectativas resultam em um erro `417 Expectation Failed`
- Atualizações de protocolo HTTP (`101 Switching Protocols` / `Upgrade`) não são suportadas e são explicitamente rejeitadas
- Respostas do tipo `205 Reset Content` são validadas como não contendo corpo e são estruturadas com `Content-Length: 0`
- **Cache em memória (RFC 9111)**:
  - LRU limitado por bytes, com tamanho máximo por objeto
  - `Cache-Control` (`max-age`, `s-maxage`, `no-store`, `no-cache`, `private`, `public`, `must-revalidate`, `max-stale`, `min-fresh`, `only-if-cached`), `Expires`, `Pragma: no-cache`
  - Cálculo de idade (`Age`) e frescor heurístico via `Last-Modified`
  - Revalidação com `If-None-Match` / `If-Modified-Since` e atualização via `304 Not Modified`
  - Suporte a `Vary`; invalidação após POST/PUT/DELETE bem-sucedidos
  - Header `X-Cache: HIT | MISS | REVALIDATED` nas respostas
- Keep-alive com o cliente, `Expect: 100-continue`, remoção de headers *hop-by-hop*, `Via`, detecção de loop
- Timeouts de upstream, de cliente ocioso e de túnel
- Encerramento gracioso via SIGINT/SIGTERM: interromper o aceite de novos clientes, concluir respostas HTTP em andamento e fechar túneis CONNECT ativos
- Endpoint de estatísticas: `GET /__proxy/stats` (JSON)

## Build

Dependências (Ubuntu/Debian):

```bash
sudo apt install build-essential cmake libboost-dev
```

Compilar e rodar os testes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/smoke_test.sh build/http_proxy   # teste ponta a ponta
```

Build com AddressSanitizer/UBSan (recomendado durante o desenvolvimento):

```bash
cmake -S . -B build-asan -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
```

## Uso

```bash
./build/http_proxy --port 8080 --threads 4 --cache-size 128
```

```bash
# primeira requisição: X-Cache: MISS; segunda: X-Cache: HIT
curl -x http://127.0.0.1:8080 -i http://example.com/

# HTTPS passa por um túnel CONNECT (não é cacheado)
curl -x http://127.0.0.1:8080 https://example.com/

# estatísticas
curl http://127.0.0.1:8080/__proxy/stats
```

Execute `./build/http_proxy --help` para ver todas as opções. Por padrão o proxy escuta só em `127.0.0.1`. Use `--listen 0.0.0.0` com cuidado: um proxy aberto pode ser usado por qualquer pessoa da rede.

## Arquitetura

```
src/
├── main.cpp              # CLI, io_context + pool de threads, sinais
├── util/                 # strings, logger thread-safe
├── http/                 # mensagens, parser incremental, URLs, datas HTTP
├── cache/                # LruCache<K,V>, Cache-Control, política RFC 9111 (HttpCache)
└── proxy/
    ├── server            # acceptor; cada conexão ganha seu próprio strand
    ├── session           # ciclo de vida de uma conexão de cliente
    ├── upstream          # uma troca request/response com a origem
    ├── tunnel            # relay de bytes do CONNECT
    └── deadline.hpp      # watchdog de timeout baseado em steady_timer
tests/                    # testes Catch2 do parser, URL, datas, LRU e política de cache
```

Fluxo de uma requisição: `Session` lê e faz o parse → consulta o `HttpCache` → *hit*: responde direto; *stale*: envia uma requisição condicional; *miss*: `UpstreamExchange` conecta na origem → a resposta é armazenada se a política permitir → é reenquadrada com `Content-Length` e enviada ao cliente.

O `HttpCache` é compartilhado entre threads e protegido por mutex. Todo o resto do estado pertence a uma única sessão, cujos handlers rodam serializados no strand dela.

## Documentação

- [Como compilar, rodar e testar](docs/README.pt-br.md): pré-requisitos, opções do CMake e da CLI, exemplos de uso, códigos de erro e solução de problemas
- [Arquitetura](docs/ARCHITECTURE.pt-br.md): camadas, modelo de concorrência, ciclo de vida de uma requisição, parser, política de cache e timeouts
- [Como contribuir](CONTRIBUTING.pt-br.md): padrões de código, testes, commits e pull requests

O [CI](.github/workflows/ci.yml) compila com GCC e Clang (Release e Debug com ASan/UBSan), roda os testes unitários e o smoke test em todo push na `main` e em todo pull request.

by caiothedev<br />
#http #c++ #proxy