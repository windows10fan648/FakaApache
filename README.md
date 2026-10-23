# FakaApache

**FakaApache** is a small, educational HTTP/1.x static-file server with optional reverse-proxy forwarding, written in C++17. It is intended for learning how TCP sockets, HTTP request parsing, response headers, static-file delivery, forwarding, and basic connection limits fit together. It is **not** a replacement for Apache, Nginx, Caddy, or another production web server.

## Downloads and releases

Prebuilt Windows packages are published on the [GitHub Releases page](https://github.com/windows10fan648/FakaApache/releases). The recommended download is the portable Windows ZIP, which contains the executable, its default configuration, the `www` directory, and this README. Verify every downloaded archive against its accompanying SHA-256 checksum before use. [RELEASES.md](RELEASES.md) records the available assets and Windows verification command.

## Build and run

A C++17 compiler is required. On POSIX systems, build the server with the following command.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread -DFAKAAPACHE_WITH_OPENSSL webserver.cpp -o fakaapache -lssl -lcrypto -lz
```

Run the server from the repository root so that the default configuration can find the `www` directory.

```bash
./fakaapache --config siteconfig.fakaapache
```

The checked-in configuration binds only to `127.0.0.1:8080`. After starting the server, open <http://127.0.0.1:8080/> in a browser or run `curl -i http://127.0.0.1:8080/`.

## Windows and MSVC

MSVC is the recommended and supported Windows toolchain. Install Visual Studio 2022 (or newer) with the Desktop development with C++ workload and Git, then configure and build from a Developer PowerShell. CMake automatically downloads a pinned vcpkg checkout into a versioned directory under `build`, bootstraps it, and installs the OpenSSL dependency through `vcpkg.json`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The executable is written to `build/Release/fakaapache.exe`. Run it from the repository root so it can find `siteconfig.fakaapache` and `www`:

```powershell
.\build\Release\fakaapache.exe --config siteconfig.fakaapache
```

The CMake configuration uses C++17, enables MSVC's `/W4` and `/permissive-` diagnostics, links Winsock (`ws2_32`), and installs OpenSSL for TLS support. The first configure requires network access and Git. To use an existing vcpkg installation instead, pass `-DFAKAAPACHE_BOOTSTRAP_VCPKG=OFF -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`.

## Scheduled for removal: MinGW-w64

MinGW-w64 support is retained temporarily for compatibility and is scheduled for removal in a future release. New Windows builds must use MSVC and CMake. If you still need MinGW-w64 during the transition, install a 64-bit toolchain using its POSIX threading model, then run the following command from the repository root in a MinGW shell.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread -DFAKAAPACHE_WITH_OPENSSL webserver.cpp -o fakaapache.exe -static -static-libgcc -static-libstdc++ -lws2_32 -lssl -lcrypto -lz
```

Alternatively, use `mingw32-make` with the supplied `Makefile`; it detects Windows and produces `fakaapache.exe` with the required Winsock library. From another POSIX machine, `make mingw` still cross-compiles a Windows executable with `x86_64-w64-mingw32-g++-posix`, but that path is also scheduled for removal.

A MinGW-ready [Code::Blocks project](FakaApache.cbp) is included as well. Open it in Code::Blocks, select a MinGW-w64 compiler toolchain, then build either the **Debug** or **Release** target. This project configuration follows the same Code::Blocks structure used by the related Mini-Terminal project while adding the `ws2_32` Winsock dependency required by FakaApache.

The Windows build initializes Winsock 2.2 before listening, uses `closesocket()` for socket cleanup, links against `ws2_32`, and retains all request-path, header-size, timeout, response-write, and concurrency protections of the POSIX build. The MinGW build scheduled for removal statically links the MinGW runtime and pthread support, while the MSVC build uses the normal Visual C++ runtime deployment model. The executable continues to read `siteconfig.fakaapache` from its working directory; run it from the repository root or provide an explicit `--config` path.

## Configuration

The configuration uses INI-style sections and `key = value` entries. Lines beginning with `#` are comments. Global settings belong in `[server]`; host-specific settings use sections such as `[vhost example.com]`.

```ini
[server]
server_name = localhost
port = 8080
root_directory = www
```

Virtual-host sections are parsed and reserved for host-based routing; the current static root remains the `[server]` root until routing is enabled.

| Setting | Default | Purpose |
| --- | --- | --- |
| `server_name` | `localhost` | A label shown in startup logs. It does not determine the listening interface. |
| `bind_address` | `127.0.0.1` | The IPv4 address on which to listen. Keep the default for local development; set `0.0.0.0` only when network access is intentional. |
| `port` | `8080` | The TCP port. It must be an integer from `1` through `65535`. |
| `root_directory` | `www` | The directory containing files that may be served. |
| `index_file` | `index.html` | The file served for a request ending in `/`. |
| `forward_address` | unset | Optional upstream hostname or IPv4 address for HTTP forwarding. Must be used with `forward_port`. |
| `forward_port` | unset | Optional upstream TCP port. When forwarding is enabled, GET requests are proxied instead of served from `root_directory`. |
| `tls_certificate` | unset | PEM certificate path. HTTPS is enabled when this and `tls_private_key` are both set. |
| `tls_private_key` | unset | PEM private-key path corresponding to `tls_certificate`. |
| `rate_limit_requests_per_minute` | `120` | Token-bucket refill rate per client IP. |
| `rate_limit_burst` | `20` | Maximum immediate requests per client IP. |
| `auth_username` | unset | Optional HTTP Basic Authentication username. Must be used with `auth_password`. |
| `auth_password` | unset | Optional plaintext Basic Authentication password. Protect the configuration file. |

To forward this server to an application listening on port `3000`, set both forwarding values:

```ini
forward_address = 127.0.0.1
forward_port = 3000
```

FakaApache opens a new upstream connection for each request and relays the upstream HTTP response. Forwarding supports `GET` and `POST`; POST bodies require a valid `Content-Length` header and are limited to 10 MiB. It is best used with a local or trusted upstream service. A failed upstream connection returns `502 Bad Gateway`.

Static HTTP/1.1 clients can reuse a connection for up to 100 requests. Static `GET` and `HEAD` requests support single byte ranges such as `Range: bytes=0-1023` and return `206 Partial Content`; multiple ranges are not supported. Forwarded connections remain close-delimited.

### TLS and rate limiting

Set both TLS paths to enable HTTPS on the configured listening port:

```ini
tls_certificate = certs/server.crt
tls_private_key = certs/server.key
```

The certificate and private key must be PEM files, and the private key must match the certificate. Rate limiting uses a token bucket per client IP. The default allows 120 requests per minute with a burst of 20; customize `rate_limit_requests_per_minute` and `rate_limit_burst` as needed.

### Authentication and compression

Set both credentials to require HTTP Basic Authentication for all routes:

```ini
auth_username = admin
auth_password = change-me
```

Credentials are stored as plaintext configuration, so protect the file and use TLS whenever authentication is enabled. Static responses are gzip-compressed when the client sends `Accept-Encoding: gzip`; forwarded upstream responses are relayed without recompression.

## Security and operational boundaries

The server URL-decodes and canonicalizes requested paths before serving them, rejects requests that escape the configured document root, and guards against symlink escapes. It protects sends from `SIGPIPE` and limits active client handlers to 64.

These safeguards make the example safer for local experimentation, but they do not provide virtual hosting, structured logging, chunked request-body proxying, or a production-grade concurrency architecture. Do not expose it directly to the public internet. Do not configure forwarding to an untrusted or user-controlled upstream.

## Tests

From ordinary Command Prompt, start FakaApache separately and run `tests\integration_test.cmd http://127.0.0.1:8080` (the URL defaults to that value). This curl-based test checks GET, HEAD, ranges, gzip negotiation, keep-alive headers, missing files, traversal and malformed URLs, unsupported methods, invalid ranges, and static POST rejection without requiring PowerShell. To test Basic Authentication too, set `FAKAAPACHE_AUTH_USER` and `FAKAAPACHE_AUTH_PASSWORD` before running it. Rate limiting, forwarding, and TLS require a separately configured server instance; use the PowerShell suite or a dedicated test configuration for those modes.

On a POSIX host, run `make test` after building to execute the integration checks. The suite verifies successful static-file delivery, HEAD responses, range delivery, unsupported-method handling, missing-file handling, percent-encoded traversal rejection, malformed URL rejection, oversized-header rejection, and invalid-port validation. On Windows PowerShell, run `.\tests\integration_test.ps1` after building; it additionally checks gzip negotiation, Basic Authentication, and rate limiting. Configure a Windows build with CMake and build the `fakaapache` target to verify MSVC support. The MinGW cross-build scheduled for removal can still be checked separately with `make mingw`; executing the resulting Windows binary requires a Windows environment or compatible runtime.
