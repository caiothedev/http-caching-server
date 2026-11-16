# CONTRIBUIÇÃO - ᴘᴛ

Obrigado por considerar contribuir com o **HTTP Caching Proxy Server**! Este guia explica como preparar o ambiente, os padrões do projeto e como enviar sua contribuição.

- [Como compilar e rodar](docs/README.pt-br.md)
- [Arquitetura](docs/ARCHITECTURE.pt-br.md)
- [English version](CONTRIBUTING.md)

## Estrutura do Projeto

```
.
├── .github/workflows/ci.yml   # CI: build + testes + smoke test (GCC/Clang, com e sem sanitizers)
├── CMakeLists.txt             # biblioteca proxy_core, executável http_proxy, testes proxy_tests
├── .clang-format              # estilo de código
├── docs/                      # como rodar e arquitetura (en + pt-br)
├── scripts/smoke_test.sh      # teste ponta a ponta com origem local
├── src/
│   ├── main.cpp               # CLI, pool de threads, sinais
│   ├── util/                  # strings, logger
│   ├── http/                  # mensagens, parser, URLs, datas (sem I/O)
│   ├── cache/                 # LRU, Cache-Control, política RFC 9111
│   └── proxy/                 # server, session, upstream, tunnel, deadline, config
└── tests/                     # testes unitários Catch2 (um arquivo por módulo)
```

As camadas só dependem das camadas abaixo: `proxy → cache → http → util`. Código em `http/` e `cache/` não faz I/O, o que permite testá-lo sem rede. Veja a [arquitetura](docs/ARCHITECTURE.pt-br.md) para mais detalhes.

## Primeiros passos

1. Faça um fork e clone o repositório.
2. Instale as dependências (veja [pré-requisitos](docs/README.pt-br.md#1-pré-requisitos)).
3. Compile com sanitizers e rode os testes:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DPROXY_ENABLE_SANITIZERS=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
scripts/smoke_test.sh build-asan/http_proxy
```

4. Crie uma branch a partir da `main`:

```bash
git checkout -b feat/minha-mudanca
```

Prefixos sugeridos: `feat/`, `fix/`, `docs/`, `test/`, `refactor/`, `ci/`.

## Regras Gerais

- **C++17**, sem extensões do compilador. Não adicione dependências além de Boost (headers) e Catch2 sem discutir antes numa issue.
- **Sem warnings.** O projeto compila com `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, e o CI trata warnings como erros.
- **Formatação:** siga o `.clang-format`. Para formatar só as linhas que você alterou:

```bash
git clang-format main
```

- **Nomes:** tipos em `PascalCase` (`HttpCache`), funções e variáveis em `snake_case` (`parse_cache_control`), membros privados com sufixo `_` (`buffer_`), constantes com prefixo `k` (`kViaToken`).
- **Namespaces:** todo código fica em `proxy::` e no sub-namespace do módulo (`proxy::http`, `proxy::cache`, `proxy::log`, `proxy::util`). Helpers internos de um `.cpp` vão num namespace anônimo.
- **Headers:** `#pragma once`; inclua o header próprio primeiro no `.cpp`, depois a biblioteca padrão, Boost e por último os headers do projeto (caminho relativo a `src/`, ex. `#include "http/parser.hpp"`).
- **Concorrência:**
  - estado de uma sessão só é acessado a partir do strand dela, sem locks;
  - estado compartilhado fica em `ProxyContext` e precisa ser thread-safe (`std::atomic` ou mutex interno, como em `HttpCache`);
  - objetos com operações assíncronas herdam `enable_shared_from_this` e capturam `shared_from_this()` nos handlers;
  - use `Deadline` para timeouts, em vez de criar novos timers.
- **Erros:** I/O usa `boost::system::error_code`, não exceções. Exceções só na inicialização (ex. falha no `bind`).
- **Logs:** use `LOG_DEBUG/INFO/WARN/ERROR` com o id da sessão (`"[" << id_ << "] ..."`). `INFO` é reservado para uma linha por requisição e eventos do servidor.
- **RFCs:** ao implementar comportamento de HTTP ou cache, cite a seção da RFC num comentário (ex. `// RFC 9111 §4.2.3`).

### 2. Testes

- Toda correção de bug deve vir com um teste que falharia sem a correção.
- Funcionalidades novas em `http/` ou `cache/` precisam de testes unitários no arquivo correspondente em `tests/`. Um arquivo novo de teste deve ser adicionado a `proxy_tests` no `CMakeLists.txt`.
- Use tags do Catch2 por módulo (`[parser]`, `[url]`, `[http-cache]`, ...).
- Mudanças no comportamento de rede (`proxy/`) devem ser verificadas com o smoke test; se for possível, estenda `scripts/smoke_test.sh` com o novo caso.
- Rode os testes com sanitizers antes de abrir o PR.

### 3. Commits

Use [Conventional Commits](https://www.conventionalcommits.org/pt-br/):

```
<tipo>(<escopo opcional>): <descrição no imperativo>
```

Exemplos:

```
feat(cache): suporte a stale-while-revalidate
fix(parser): rejeitar chunk size com overflow
docs: explicar o modelo de strands
test(url): casos de IPv6 com porta
```

Tipos: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `ci`, `build`, `chore`. Mantenha cada commit focado em uma mudança.

### 4. Pull Requests

1. Atualize sua branch com a `main` antes de abrir o PR.
2. Garanta que o CI passa: os quatro jobs (GCC/Clang × Release/Sanitizers) precisam estar verdes.
3. Na descrição, explique **o quê** mudou e **por quê**, como foi testado e, se for o caso, a seção da RFC relacionada. Referencie a issue (`Closes #N`).
4. PRs pequenos são revisados mais rápido. Separe refatorações de mudanças de comportamento.
5. Responda aos comentários da revisão com novos commits; o histórico pode ser compactado no merge.

Checklist:

- [ ] compila sem warnings com GCC e Clang
- [ ] `ctest` e `scripts/smoke_test.sh` passam, inclusive com `-DPROXY_ENABLE_SANITIZERS=ON`
- [ ] testes novos cobrem a mudança
- [ ] documentação atualizada (README, `docs/`, `--help`) quando há mudança visível ao usuário

### 5. Documentação

- A documentação é bilíngue: toda mudança em um arquivo `*.md` deve ser feita também na versão do outro idioma (`README.md` ↔ `README.pt-br.md`, `docs/ARCHITECTURE.en.md` ↔ `docs/ARCHITECTURE.pt-br.md`, etc.).
- Uma opção de linha de comando nova deve aparecer em `usage()` (`src/proxy/config.cpp`) e na tabela de opções de `docs/README*.md`.
- Mudanças de fluxo, concorrência ou política de cache devem ser refletidas em `docs/ARCHITECTURE*.md`, incluindo os diagramas Mermaid.
- Comentários no código são em inglês e explicam o **porquê**, não o que a linha faz.

### 6. Issues

- **Bugs:** descreva o comando usado para iniciar o proxy, a requisição (ex. o comando `curl`), o resultado esperado e o obtido, e as linhas relevantes do log com `--log-level debug`.
- **Sugestões:** explique o caso de uso e, se possível, a referência na RFC.
- **Segurança:** não abra issue pública para vulnerabilidades; entre em contato diretamente com o mantenedor.

by caiothedev<br />
#contribution