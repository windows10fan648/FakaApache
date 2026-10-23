#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#ifndef _WIN32
#include <netdb.h>
#endif
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <utility>

#ifdef FAKAAPACHE_WITH_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::uintmax_t kMaxFileBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxConcurrentClients = 64;
constexpr int kClientTimeoutSeconds = 5;
constexpr std::size_t kMaxRequestBodyBytes = 10 * 1024 * 1024;

#ifdef FAKAAPACHE_WITH_OPENSSL
using TlsContextHandle = SSL_CTX*;
using TlsConnectionHandle = SSL*;
#else
using TlsContextHandle = void*;
using TlsConnectionHandle = void*;
#endif

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketResult = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
constexpr int kSocketError = SOCKET_ERROR;
constexpr int kSendFlags = 0;
#else
using SocketHandle = int;
using SocketResult = ssize_t;
constexpr SocketHandle kInvalidSocket = -1;
constexpr int kSocketError = -1;
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

std::mutex cout_mutex;
std::atomic<std::size_t> active_clients{0};
bool use_ansi_colors = false;

void initialize_console_colors() {
#ifdef _WIN32
    const HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD console_mode = 0;
    if (output_handle == INVALID_HANDLE_VALUE || !GetConsoleMode(output_handle, &console_mode)) {
        return;
    }

    if ((console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
        SetConsoleMode(output_handle, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        use_ansi_colors = true;
    }
#else
    use_ansi_colors = isatty(STDOUT_FILENO) != 0 && isatty(STDERR_FILENO) != 0;
#endif
}

int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string socket_error_message(int error) {
#ifdef _WIN32
    return "Winsock error " + std::to_string(error);
#else
    return std::strerror(error);
#endif
}

bool is_interrupted_error(int error) {
#ifdef _WIN32
    return error == WSAEINTR;
#else
    return error == EINTR;
#endif
}

bool is_timeout_error(int error) {
#ifdef _WIN32
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

void close_socket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool set_reuse_address(SocketHandle socket) {
    const int option = 1;
#ifdef _WIN32
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option),
                      static_cast<int>(sizeof(option))) != kSocketError;
#else
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) != kSocketError;
#endif
}

class NetworkRuntime {
public:
    NetworkRuntime() = default;

    bool initialize() {
#ifdef _WIN32
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            startup_error_ = result;
            return false;
        }
        initialized_ = true;
#endif
        return true;
    }

    [[nodiscard]] int startup_error() const {
        return startup_error_;
    }

    ~NetworkRuntime() {
#ifdef _WIN32
        if (initialized_) {
            WSACleanup();
        }
#endif
    }

    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;

private:
    bool initialized_ = false;
    int startup_error_ = 0;
};

class Socket {
public:
    explicit Socket(SocketHandle socket = kInvalidSocket) : socket_(socket) {}

    ~Socket() {
        close_socket(socket_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : socket_(other.release()) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] SocketHandle get() const {
        return socket_;
    }

    [[nodiscard]] SocketHandle release() {
        const SocketHandle released_socket = socket_;
        socket_ = kInvalidSocket;
        return released_socket;
    }

    void reset(SocketHandle socket = kInvalidSocket) {
        close_socket(socket_);
        socket_ = socket;
    }

private:
    SocketHandle socket_;
};

class ClientSlot {
public:
    ClientSlot() = default;

    ~ClientSlot() {
        active_clients.fetch_sub(1, std::memory_order_relaxed);
    }

    ClientSlot(const ClientSlot&) = delete;
    ClientSlot& operator=(const ClientSlot&) = delete;
};

void log_info(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    if (use_ansi_colors) {
        std::cout << "\033[32m[INFO]\033[0m " << message << std::endl;
    } else {
        std::cout << "[INFO] " << message << std::endl;
    }
}

void log_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    if (use_ansi_colors) {
        std::cerr << "\033[31m[ERROR]\033[0m " << message << std::endl;
    } else {
        std::cerr << "[ERROR] " << message << std::endl;
    }
}

std::string trim_copy(std::string value) {
    const std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string printable_for_log(std::string_view value) {
    constexpr std::size_t kMaxLogLength = 200;
    std::string result;
    result.reserve(std::min(value.size(), kMaxLogLength));

    for (std::size_t i = 0; i < value.size() && i < kMaxLogLength; ++i) {
        const unsigned char character = static_cast<unsigned char>(value[i]);
        result.push_back(std::isprint(character) ? static_cast<char>(character) : '?');
    }

    if (value.size() > kMaxLogLength) {
        result += "...";
    }
    return result;
}

std::unordered_map<std::string, std::string> parse_config(const std::string& filename) {
    std::unordered_map<std::string, std::string> config;
    std::ifstream file(filename);
    if (!file) {
        log_error("Failed to open config file: " + filename);
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        const std::size_t comment_position = line.find('#');
        if (comment_position != std::string::npos) {
            line.resize(comment_position);
        }

        line = trim_copy(std::move(line));
        if (line.empty()) {
            continue;
        }

        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            log_error("Ignoring malformed configuration line");
            continue;
        }

        std::string key = trim_copy(line.substr(0, separator));
        std::string value = trim_copy(line.substr(separator + 1));
        if (key.empty() || value.empty()) {
            log_error("Ignoring configuration entry with an empty key or value");
            continue;
        }

        config[std::move(key)] = std::move(value);
    }

    return config;
}

bool parse_port(const std::string& value, std::uint16_t& port) {
    unsigned int parsed_port = 0;
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, parsed_port);

    if (error != std::errc{} || parsed_end != end || parsed_port == 0 || parsed_port > 65535) {
        return false;
    }

    port = static_cast<std::uint16_t>(parsed_port);
    return true;
}

bool parse_positive_limit(const std::string& value, std::size_t& result) {
    unsigned long long parsed = 0;
    const auto [parsed_end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || parsed_end != value.data() + value.size() || parsed == 0 ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    result = static_cast<std::size_t>(parsed);
    return true;
}

bool send_all(SocketHandle socket, std::string_view data, TlsConnectionHandle tls = nullptr) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t remaining = data.size() - sent;
        const int chunk_size = static_cast<int>(std::min(
            remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        int result = 0;
#ifdef FAKAAPACHE_WITH_OPENSSL
        if (tls != nullptr) {
            result = SSL_write(tls, data.data() + sent, chunk_size);
        } else
#endif
        {
            result = send(socket, data.data() + sent, chunk_size, kSendFlags);
        }
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }

        const int error = last_socket_error();
        if (result == kSocketError && is_interrupted_error(error)) {
            continue;
        }
        return false;
    }
    return true;
}

void send_response(SocketHandle socket,
                   std::string_view status,
                   std::string_view body = "",
                   std::string_view content_type = "text/plain; charset=utf-8",
                   std::string_view extra_headers = "",
                   bool include_body = true,
                   TlsConnectionHandle tls = nullptr) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << extra_headers
             << "\r\n";
    if (include_body) {
        response << body;
    }
    send_all(socket, response.str(), tls);
}

enum class HeaderReadResult {
    Complete,
    Malformed,
    TooLarge,
    TimedOut,
};

HeaderReadResult read_request_headers(SocketHandle socket,
                                      std::string& request,
                                      TlsConnectionHandle tls = nullptr) {
    char buffer[2048];
    while (request.size() < kMaxHeaderBytes) {
        const std::size_t capacity = std::min(sizeof(buffer), kMaxHeaderBytes - request.size());
        SocketResult bytes_received = 0;
#ifdef FAKAAPACHE_WITH_OPENSSL
        if (tls != nullptr) {
            bytes_received = SSL_read(tls, buffer, static_cast<int>(capacity));
        } else
#endif
        {
            bytes_received = recv(socket, buffer, static_cast<int>(capacity), 0);
        }
        if (bytes_received > 0) {
            request.append(buffer, static_cast<std::size_t>(bytes_received));
            if (request.find("\r\n\r\n") != std::string::npos) {
                return HeaderReadResult::Complete;
            }
            continue;
        }

        if (bytes_received == 0) {
            return HeaderReadResult::Malformed;
        }

        const int error = last_socket_error();
        if (is_interrupted_error(error)) {
            continue;
        }
        if (is_timeout_error(error)) {
            return HeaderReadResult::TimedOut;
        }
        return HeaderReadResult::Malformed;
    }

    return HeaderReadResult::TooLarge;
}

bool parse_request_line(const std::string& request,
                        std::string& method,
                        std::string& target,
                        std::string& version) {
    const std::size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos) {
        return false;
    }

    std::istringstream stream(request.substr(0, line_end));
    std::string extra;
    if (!(stream >> method >> target >> version) || (stream >> extra)) {
        return false;
    }

    return version == "HTTP/1.0" || version == "HTTP/1.1";
}

std::optional<std::string> get_header_value(const std::string& request, std::string_view header_name) {
    const std::size_t first_line_end = request.find("\r\n");
    if (first_line_end == std::string::npos) {
        return std::nullopt;
    }

    std::size_t line_start = first_line_end + 2;
    while (line_start < request.size()) {
        const std::size_t line_end = request.find("\r\n", line_start);
        if (line_end == std::string::npos || line_end == line_start) {
            break;
        }

        const std::size_t separator = request.find(':', line_start);
        if (separator != std::string::npos && separator < line_end) {
            std::string name = request.substr(line_start, separator - line_start);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });

            if (name == header_name) {
                return trim_copy(request.substr(separator + 1, line_end - separator - 1));
            }
        }
        line_start = line_end + 2;
    }
    return std::nullopt;
}

enum class RequestBodyReadResult {
    Complete,
    Malformed,
    TooLarge,
    TimedOut,
};

RequestBodyReadResult read_request_body(SocketHandle socket,
                                        std::string& request,
                                        TlsConnectionHandle tls = nullptr) {
    const std::optional<std::string> content_length = get_header_value(request, "content-length");
    if (!content_length.has_value()) {
        return RequestBodyReadResult::Malformed;
    }

    std::size_t body_length = 0;
    const char* const begin = content_length->data();
    const char* const end = begin + content_length->size();
    const auto [parsed_end, error] = std::from_chars(begin, end, body_length);
    if (error != std::errc{} || parsed_end != end) {
        return RequestBodyReadResult::Malformed;
    }
    if (body_length > kMaxRequestBodyBytes) {
        return RequestBodyReadResult::TooLarge;
    }

    const std::size_t headers_end = request.find("\r\n\r\n");
    if (headers_end == std::string::npos) {
        return RequestBodyReadResult::Malformed;
    }
    const std::size_t body_start = headers_end + 4;
    while (request.size() < body_start + body_length) {
        char buffer[8192];
        const std::size_t remaining = body_start + body_length - request.size();
        const int capacity = static_cast<int>(std::min(remaining, sizeof(buffer)));
        SocketResult bytes_received = 0;
#ifdef FAKAAPACHE_WITH_OPENSSL
        if (tls != nullptr) {
            bytes_received = SSL_read(tls, buffer, capacity);
        } else
#endif
        {
            bytes_received = recv(socket, buffer, capacity, 0);
        }
        if (bytes_received > 0) {
            request.append(buffer, static_cast<std::size_t>(bytes_received));
            continue;
        }
        if (bytes_received == 0) {
            return RequestBodyReadResult::Malformed;
        }

        const int socket_error = last_socket_error();
        if (is_interrupted_error(socket_error)) {
            continue;
        }
        if (is_timeout_error(socket_error)) {
            return RequestBodyReadResult::TimedOut;
        }
        return RequestBodyReadResult::Malformed;
    }

    request.resize(body_start + body_length);
    return RequestBodyReadResult::Complete;
}

bool url_decode(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());

    const auto hex_value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '%') {
            decoded.push_back(encoded[i]);
            continue;
        }

        if (i + 2 >= encoded.size()) {
            return false;
        }
        const int high = hex_value(encoded[i + 1]);
        const int low = hex_value(encoded[i + 2]);
        if (high < 0 || low < 0) {
            return false;
        }

        const char character = static_cast<char>((high << 4) | low);
        if (character == '\0') {
            return false;
        }
        decoded.push_back(character);
        i += 2;
    }

    return true;
}

bool is_within_root(const fs::path& path, const fs::path& root) {
    auto path_it = path.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) {
            return false;
        }
    }
    return true;
}

enum class PathResolutionResult {
    Ok,
    BadRequest,
    Forbidden,
    NotFound,
};

PathResolutionResult resolve_requested_file(const fs::path& canonical_root,
                                            std::string_view request_target,
                                            const std::string& index_file,
                                            fs::path& resolved_file) {
    const std::size_t parameter_position = request_target.find_first_of("?#");
    request_target = request_target.substr(0, parameter_position);
    if (request_target.empty() || request_target.front() != '/') {
        return PathResolutionResult::BadRequest;
    }

    std::string decoded_target;
    if (!url_decode(request_target, decoded_target) || decoded_target.find('\\') != std::string::npos) {
        return PathResolutionResult::BadRequest;
    }

    fs::path relative_path = decoded_target.substr(1);
    if (relative_path.empty() || decoded_target.back() == '/') {
        relative_path /= index_file;
    }
    if (relative_path.is_absolute()) {
        return PathResolutionResult::Forbidden;
    }

    std::error_code error;
    const fs::path candidate = fs::weakly_canonical(canonical_root / relative_path, error);
    if (error) {
        return PathResolutionResult::NotFound;
    }
    if (!is_within_root(candidate, canonical_root)) {
        return PathResolutionResult::Forbidden;
    }

    if (!fs::exists(candidate, error) || error || !fs::is_regular_file(candidate, error) || error) {
        return PathResolutionResult::NotFound;
    }

    resolved_file = candidate;
    return PathResolutionResult::Ok;
}

bool read_file(const fs::path& path, std::string& content) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > kMaxFileBytes || size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

std::string get_mime_type(const fs::path& path) {
    std::string extension = path.extension().string();
    for (char& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    if (extension == ".html" || extension == ".htm") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js") {
        return "application/javascript; charset=utf-8";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".txt") {
        return "text/plain; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

bool acquire_client_slot() {
    std::size_t current = active_clients.load(std::memory_order_relaxed);
    while (current < kMaxConcurrentClients) {
        if (active_clients.compare_exchange_weak(
                current, current + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void configure_client_timeout(SocketHandle socket) {
#ifdef _WIN32
    const DWORD timeout_milliseconds = static_cast<DWORD>(kClientTimeoutSeconds * 1000);
    const int result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                                  reinterpret_cast<const char*>(&timeout_milliseconds),
                                  static_cast<int>(sizeof(timeout_milliseconds)));
#else
    timeval timeout{};
    timeout.tv_sec = kClientTimeoutSeconds;
    timeout.tv_usec = 0;
    const int result = setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    if (result == kSocketError) {
        log_error("Failed to set client receive timeout: " + socket_error_message(last_socket_error()));
    }
}

struct ForwardTarget {
    std::string address;
    std::uint16_t port = 0;
};

struct RateLimitConfig {
    std::size_t requests_per_minute = 120;
    std::size_t burst = 20;
};

struct RateBucket {
    double tokens = -1.0;
    std::chrono::steady_clock::time_point last_refill{};
};

std::mutex rate_limit_mutex;
std::unordered_map<std::string, RateBucket> rate_buckets;

bool allow_request(const std::string& client_address, const RateLimitConfig& config) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    RateBucket& bucket = rate_buckets[client_address];
    if (bucket.tokens < 0.0) {
        bucket.tokens = static_cast<double>(config.burst);
        bucket.last_refill = now;
    }

    const std::chrono::duration<double> elapsed = now - bucket.last_refill;
    const double refill = elapsed.count() * static_cast<double>(config.requests_per_minute) / 60.0;
    bucket.tokens = std::min(static_cast<double>(config.burst), bucket.tokens + refill);
    bucket.last_refill = now;
    if (bucket.tokens < 1.0) {
        return false;
    }
    bucket.tokens -= 1.0;
    return true;
}

std::string peer_address(SocketHandle socket) {
    sockaddr_storage address{};
#ifdef _WIN32
    int address_length = static_cast<int>(sizeof(address));
#else
    socklen_t address_length = sizeof(address);
#endif
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        return "unknown";
    }

    char host[NI_MAXHOST]{};
    if (getnameinfo(reinterpret_cast<const sockaddr*>(&address), address_length,
                    host, static_cast<socklen_t>(sizeof(host)), nullptr, 0, NI_NUMERICHOST) != 0) {
        return "unknown";
    }
    return host;
}

bool connect_to_forward_target(const ForwardTarget& target, Socket& upstream) {
    const std::string port = std::to_string(target.port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const int lookup_result = getaddrinfo(target.address.c_str(), port.c_str(), &hints, &addresses);
    if (lookup_result != 0) {
        return false;
    }

    for (const addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const SocketHandle candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == kInvalidSocket) {
            continue;
        }

        if (connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            upstream.reset(candidate);
            configure_client_timeout(upstream.get());
            freeaddrinfo(addresses);
            return true;
        }
        close_socket(candidate);
    }

    freeaddrinfo(addresses);
    return false;
}

bool forward_request(SocketHandle client_socket,
                      const std::string& request,
                      const ForwardTarget& target,
                      TlsConnectionHandle client_tls = nullptr) {
    Socket upstream;
    if (!connect_to_forward_target(target, upstream)) {
        send_response(client_socket, "502 Bad Gateway", "", "text/plain; charset=utf-8", "", true, client_tls);
        log_error("Unable to connect to forwarding target " + target.address + ":" +
                  std::to_string(target.port));
        return false;
    }

    const std::size_t headers_end = request.find("\r\n\r\n");
    std::string upstream_request = request;
    upstream_request.insert(headers_end, "Connection: close\r\n");
    if (!send_all(upstream.get(), upstream_request)) {
        send_response(client_socket, "502 Bad Gateway", "", "text/plain; charset=utf-8", "", true, client_tls);
        return false;
    }

    char buffer[8192];
    while (true) {
        const SocketResult bytes_received = recv(upstream.get(), buffer, sizeof(buffer), 0);
        if (bytes_received > 0) {
            if (!send_all(client_socket, std::string_view(buffer, static_cast<std::size_t>(bytes_received)), client_tls)) {
                return false;
            }
            continue;
        }
        if (bytes_received == 0) {
            return true;
        }

        const int error = last_socket_error();
        if (is_interrupted_error(error)) {
            continue;
        }
        log_error("Forwarding connection closed with socket error: " + socket_error_message(error));
        return false;
    }
}

void handle_client(SocketHandle client_socket,
                   const fs::path& canonical_root,
                   const std::string& index_file,
                   const ForwardTarget* forwarding_target,
                   TlsContextHandle tls_context,
                   const RateLimitConfig& rate_limit_config) {
    Socket client(client_socket);
    ClientSlot slot;
    configure_client_timeout(client.get());

#ifdef FAKAAPACHE_WITH_OPENSSL
    std::unique_ptr<SSL, decltype(&SSL_free)> tls_holder(nullptr, SSL_free);
    if (tls_context != nullptr) {
        tls_holder.reset(SSL_new(tls_context));
        if (!tls_holder || SSL_set_fd(tls_holder.get(), static_cast<int>(client.get())) != 1 ||
            SSL_accept(tls_holder.get()) != 1) {
            log_error("TLS handshake failed");
            return;
        }
    }
    const TlsConnectionHandle tls = tls_holder.get();
#else
    const TlsConnectionHandle tls = nullptr;
#endif

    if (!allow_request(peer_address(client.get()), rate_limit_config)) {
        send_response(client.get(), "429 Too Many Requests", "Rate limit exceeded\n",
                      "text/plain; charset=utf-8", "Retry-After: 1\r\n", true, tls);
        return;
    }

    std::string request;
    switch (read_request_headers(client.get(), request, tls)) {
        case HeaderReadResult::Complete:
            break;
        case HeaderReadResult::TooLarge:
            send_response(client.get(), "431 Request Header Fields Too Large", "", "text/plain; charset=utf-8", "", true, tls);
            return;
        case HeaderReadResult::TimedOut:
            send_response(client.get(), "408 Request Timeout", "", "text/plain; charset=utf-8", "", true, tls);
            return;
        case HeaderReadResult::Malformed:
            send_response(client.get(), "400 Bad Request", "", "text/plain; charset=utf-8", "", true, tls);
            return;
    }

    std::string method;
    std::string target;
    std::string version;
    if (!parse_request_line(request, method, target, version)) {
        send_response(client.get(), "400 Bad Request", "", "text/plain; charset=utf-8", "", true, tls);
        return;
    }

    log_info("Request: " + printable_for_log(method) + " " + printable_for_log(target));

    if (method != "GET" && method != "HEAD" && method != "POST") {
        send_response(client.get(), "405 Method Not Allowed", "", "text/plain; charset=utf-8", "Allow: GET, HEAD, POST\r\n", true, tls);
        return;
    }

    if (method == "POST" && forwarding_target == nullptr) {
        send_response(client.get(), "405 Method Not Allowed", "", "text/plain; charset=utf-8", "Allow: GET, HEAD\r\n", true, tls);
        return;
    }

    if (method == "POST") {
        const std::optional<std::string> transfer_encoding = get_header_value(request, "transfer-encoding");
        if (transfer_encoding.has_value() && *transfer_encoding != "identity") {
            send_response(client.get(), "400 Bad Request", "", "text/plain; charset=utf-8", "", true, tls);
            return;
        }

        switch (read_request_body(client.get(), request, tls)) {
            case RequestBodyReadResult::Complete:
                break;
            case RequestBodyReadResult::TooLarge:
                send_response(client.get(), "413 Content Too Large", "", "text/plain; charset=utf-8", "", true, tls);
                return;
            case RequestBodyReadResult::TimedOut:
                send_response(client.get(), "408 Request Timeout", "", "text/plain; charset=utf-8", "", true, tls);
                return;
            case RequestBodyReadResult::Malformed:
                send_response(client.get(), "400 Bad Request", "", "text/plain; charset=utf-8", "", true, tls);
                return;
        }
    }

    if (forwarding_target != nullptr) {
        forward_request(client.get(), request, *forwarding_target, tls);
        return;
    }

    fs::path file_path;
    switch (resolve_requested_file(canonical_root, target, index_file, file_path)) {
        case PathResolutionResult::Ok:
            break;
        case PathResolutionResult::BadRequest:
            send_response(client.get(), "400 Bad Request", "", "text/plain; charset=utf-8", "", true, tls);
            return;
        case PathResolutionResult::Forbidden:
            send_response(client.get(), "403 Forbidden", "", "text/plain; charset=utf-8", "", true, tls);
            log_error("Rejected unsafe path: " + printable_for_log(target));
            return;
        case PathResolutionResult::NotFound:
            send_response(client.get(), "404 Not Found", "", "text/plain; charset=utf-8", "", true, tls);
            return;
    }

    std::string content;
    if (!read_file(file_path, content)) {
        send_response(client.get(), "500 Internal Server Error", "", "text/plain; charset=utf-8", "", true, tls);
        log_error("Failed to read file: " + file_path.string());
        return;
    }

    send_response(client.get(), "200 OK", content, get_mime_type(file_path), "", method == "GET", tls);
    log_info("Served: " + file_path.string());
}

void print_usage(const char* executable_name) {
    std::cerr << "Usage: " << executable_name << " [--config <path>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    initialize_console_colors();

    NetworkRuntime network_runtime;
    if (!network_runtime.initialize()) {
        log_error("Network initialization failed: " + socket_error_message(network_runtime.startup_error()));
        return 1;
    }

    std::string config_path = "siteconfig.fakaapache";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--config") {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            config_path = argv[++i];
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            log_error("Unknown argument: " + argument);
            print_usage(argv[0]);
            return 1;
        }
    }

    const auto config = parse_config(config_path);
    if (config.empty()) {
        log_error("Configuration parsing failed or the configuration file is empty.");
        return 1;
    }

    const auto get_config_value = [&config](const std::string& key, const std::string& fallback) {
        const auto iterator = config.find(key);
        return iterator == config.end() ? fallback : iterator->second;
    };

    const std::string server_name = get_config_value("server_name", "localhost");
    const std::string bind_address = get_config_value("bind_address", "127.0.0.1");
    const std::string root_directory = get_config_value("root_directory", "www");
    const std::string index_file = get_config_value("index_file", "index.html");
    const std::string forward_address = get_config_value("forward_address", "");
    const std::string forward_port_value = get_config_value("forward_port", "");
    const std::string tls_certificate = get_config_value("tls_certificate", "");
    const std::string tls_private_key = get_config_value("tls_private_key", "");
    RateLimitConfig rate_limit_config;
    if (!parse_positive_limit(get_config_value("rate_limit_requests_per_minute", "120"),
                              rate_limit_config.requests_per_minute) ||
        !parse_positive_limit(get_config_value("rate_limit_burst", "20"), rate_limit_config.burst)) {
        log_error("Rate-limit settings must be positive integers.");
        return 1;
    }

    const bool forwarding_configured = !forward_address.empty() || !forward_port_value.empty();
    ForwardTarget forwarding_target;
    if (forwarding_configured) {
        if (forward_address.empty() || forward_port_value.empty() ||
            !parse_port(forward_port_value, forwarding_target.port)) {
            log_error("Forwarding requires forward_address and a forward_port from 1 through 65535.");
            return 1;
        }
        forwarding_target.address = forward_address;
    }

    std::uint16_t port = 0;
    if (!parse_port(get_config_value("port", "8080"), port)) {
        log_error("The configured port must be an integer from 1 through 65535.");
        return 1;
    }

    TlsContextHandle tls_context = nullptr;
    if (!tls_certificate.empty() || !tls_private_key.empty()) {
#ifdef FAKAAPACHE_WITH_OPENSSL
        if (tls_certificate.empty() || tls_private_key.empty()) {
            log_error("TLS requires both tls_certificate and tls_private_key.");
            return 1;
        }
        OPENSSL_init_ssl(0, nullptr);
        tls_context = SSL_CTX_new(TLS_server_method());
        if (tls_context == nullptr ||
            SSL_CTX_set_min_proto_version(tls_context, TLS1_2_VERSION) != 1 ||
            SSL_CTX_use_certificate_file(tls_context, tls_certificate.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(tls_context, tls_private_key.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(tls_context) != 1) {
            log_error("Failed to load the configured TLS certificate or private key.");
            if (tls_context != nullptr) {
                SSL_CTX_free(tls_context);
            }
            return 1;
        }
#else
        log_error("This build does not include OpenSSL TLS support.");
        return 1;
#endif
    }

    fs::path canonical_root;
    if (!forwarding_configured) {
        std::error_code error;
        canonical_root = fs::canonical(root_directory, error);
        if (error || !fs::is_directory(canonical_root, error) || error) {
            log_error("The configured root directory is not an accessible directory: " + root_directory);
            return 1;
        }
    }

    Socket server(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (server.get() == kInvalidSocket) {
        log_error("Socket creation failed: " + socket_error_message(last_socket_error()));
        return 1;
    }

    if (!set_reuse_address(server.get())) {
        log_error("Failed to set SO_REUSEADDR: " + socket_error_message(last_socket_error()));
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        log_error("bind_address must be a valid IPv4 address: " + bind_address);
        return 1;
    }

    if (bind(server.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == kSocketError) {
        log_error("Bind failed: " + socket_error_message(last_socket_error()));
        return 1;
    }

    if (listen(server.get(), SOMAXCONN) == kSocketError) {
        log_error("Listen failed: " + socket_error_message(last_socket_error()));
        return 1;
    }

    log_info("FakaApache listening for " + server_name + " at " + bind_address + ":" + std::to_string(port));
    if (forwarding_configured) {
        log_info("Forwarding HTTP requests to: " + forwarding_target.address + ":" +
                 std::to_string(forwarding_target.port));
    } else {
        log_info("Serving files from: " + canonical_root.string());
        log_info("Default index file: " + index_file);
    }
    if (tls_context != nullptr) {
        log_info("TLS is enabled");
    }
    log_info("Concurrent-client limit: " + std::to_string(kMaxConcurrentClients));
    log_info("Rate limit: " + std::to_string(rate_limit_config.requests_per_minute) +
             " requests/minute, burst " + std::to_string(rate_limit_config.burst));

    while (true) {
        const SocketHandle client_socket = accept(server.get(), nullptr, nullptr);
        if (client_socket == kInvalidSocket) {
            const int accept_error = last_socket_error();
            if (is_interrupted_error(accept_error)) {
                continue;
            }
            log_error("Accept failed: " + socket_error_message(accept_error));
            continue;
        }

        if (!acquire_client_slot()) {
            if (tls_context == nullptr) {
                send_response(client_socket, "503 Service Unavailable");
            }
            close_socket(client_socket);
            continue;
        }

        const ForwardTarget* target = forwarding_configured ? &forwarding_target : nullptr;
        std::thread(handle_client, client_socket, std::cref(canonical_root), std::cref(index_file), target,
                    tls_context, std::cref(rate_limit_config)).detach();
    }
}
