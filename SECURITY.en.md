# SECURITY POLICY - ᴇɴ

This document describes the security boundaries and operational precautions for the HTTP Caching Proxy Server.

## Supported versions

Security fixes are intended for the latest code on the `main` branch. This project does not currently publish a formal release-support matrix. When reporting an issue, include the commit SHA or version you tested.

## Deployment model

**This server is a development and study project, not a hardened, production-ready forward proxy.**

The default listener is `127.0.0.1:8080`, which limits access to the local machine. Changing the bind address to `0.0.0.0` or another externally reachable interface can expose the proxy to every host that can reach that interface.

The proxy currently has:

- No client authentication.
- No user or IP access-control list.
- No per-client authorization, quotas, or rate limiting.
- No TLS termination for the client-to-proxy connection.

Anyone who can connect to an exposed listener can submit requests through it. Do not expose it directly to the public internet or an untrusted network.

## CONNECT restrictions

HTTPS proxying uses the HTTP `CONNECT` method to establish a TCP tunnel. By default, only destination port `443` is allowed.

Use `--connect-ports` to configure the permitted destination ports. The special value `any` removes the port restriction and is not recommended for an exposed service.

The port restriction is not a destination allowlist: it does not restrict destination hostnames or IP addresses.

## Destination access policy

`--allow-hosts` limits forwarding and CONNECT to listed hosts (`*.example.com` matches subdomains). `--block-private` refuses destinations whose resolved addresses are loopback, private, link-local (including `169.254.169.254` cloud metadata), CGNAT, multicast or otherwise non-public; the check runs after DNS resolution and the proxy only connects to approved addresses, which mitigates server-side request forgery (SSRF) against internal services. Both are disabled by default. They complement, and do not replace, egress firewall rules or an isolated network.

Clients beyond `--max-connections` receive `503 Service Unavailable`; at most 64 such answers are in progress at once, further connections are closed immediately.

After the tunnel is established, the proxy relays bytes and does not inspect or cache the encrypted application traffic.

## HTTP and cache considerations

- Ordinary HTTP forwarding is not encrypted between the client and this proxy. Use a trusted network or a protected transport layer when that connection crosses a network boundary.
- Cached responses are held in process memory. The cache is shared between clients of this proxy; response storage follows the implemented shared-cache rules, including restrictions for `private`, `no-store`, authenticated requests, `Set-Cookie`, and `Vary: *`.
- Do not treat the cache as a confidentiality boundary between mutually untrusted users. Do not enable public access without reviewing the data that may pass through the proxy.
- The `/__proxy/stats` endpoint has no authentication. It reports operational counters and cache statistics to any client able to reach the listener. It is served with `Cache-Control: no-store`.
- Access logs include the client endpoint, request method and URL, response status, response size, cache result, and elapsed time. URLs can contain sensitive information; protect log output accordingly.

## Parser and resource limits

The HTTP parser enforces limits on header size, header count, and message body size. The defaults are 64 KiB for the start line plus headers, 100 header fields, and 8 MiB for request bodies; response bodies are streamed with backpressure, so their size does not translate into proxy memory (only a copy kept for the cache, up to `--max-object`, per connection). The default connection limit is 16 simultaneous clients, including CONNECT tunnels. The configured product of `--max-connections` and `--max-body` cannot exceed 128 MiB. Fixed `Content-Length` values are checked without reserving the full declared body before bytes arrive.

The parser rejects ambiguous `Transfer-Encoding` plus `Content-Length` framing, malformed framing, unsupported transfer codings, and unsupported protocol upgrades. Only the `chunked` transfer coding is implemented. These checks reduce parsing ambiguity but are not a substitute for network isolation or a security review.

The cache has a configurable total capacity and maximum object size. The connection and body limits bound concurrent in-flight message-body storage separately from cache storage. This is not a strict process RSS limit: response serialization, socket buffers, allocator capacity, protocol state, and cache metadata also consume memory.

## Deployment checklist

- Keep the listener bound to `127.0.0.1` unless remote access is explicitly required.
- If remote access is required, place the proxy behind a trusted network boundary and enforce authentication, authorization, and network access controls externally.
- Enable `--block-private`, and `--allow-hosts` when the set of destinations is known, on any listener reachable by untrusted clients.
- Restrict outbound traffic with firewall rules as well; `--connect-ports` alone is not a host/IP allowlist.
- Keep the default CONNECT port restriction unless additional ports are required.
- Set `--cache-size` and `--max-object` according to available memory and expected traffic.
- Protect logs and avoid logging or sharing URLs that contain credentials or private tokens.
- Run with only the network permissions and operating-system privileges the process needs.
- Keep the compiler, Boost, CMake, and other build dependencies up to date.

## Reporting a vulnerability

Please do not disclose an unpatched vulnerability in a public issue.

Use GitHub's private vulnerability reporting for this repository if it is enabled. Otherwise, contact the repository maintainer privately through a channel listed on the maintainer's GitHub profile.

Include, where possible:

- A concise description of the impact and affected component.
- The commit SHA, compiler, operating system, and build options.
- Reproduction steps or a minimal proof of concept.
- Relevant logs with secrets, credentials, and personal data removed.

Do not include live credentials, private user data, or exploit payloads that affect systems you do not own or have permission to test.

## Scope and limitations

This policy documents the current implementation and its operational risks. It is not a guarantee that the software is free of vulnerabilities, and it does not constitute a security certification or production-readiness claim.

by caiothedev<br />
#security