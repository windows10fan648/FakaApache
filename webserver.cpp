#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <system_error>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::uintmax_t kMaxFileBytes = 10 * 1024 * 1024;
constexpr std::size_t kMaxConcurrentClients = 64;
constexpr int kClientTimeoutSeconds = 5;

std::mutex cout_mutex;
std::atomic<std::size_t> active_clients{0};

class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}

    ~Socket() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.release()) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }

    [[nodiscard]] int release() {
        const int released_fd = fd_;
        fd_ = -1;
        return released_fd;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
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
    std::cout << "\033[32m[INFO]\033[0m " << message << std::endl;
}

void log_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cerr << "\033[31m[ERROR]\033[0m " << message << std::endl;
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

bool send_all(int socket_fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = send(socket_fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void send_response(int socket_fd,
                   std::string_view status,
                   std::string_view body = "",
                   std::string_view content_type = "text/plain; charset=utf-8",
                   std::string_view extra_headers = "") {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << extra_headers
             << "\r\n"
             << body;
    send_all(socket_fd, response.str());
}

enum class HeaderReadResult {
    Complete,
    Malformed,
    TooLarge,
    TimedOut,
};

HeaderReadResult read_request_headers(int socket_fd, std::string& request) {
    char buffer[2048];
    while (request.size() < kMaxHeaderBytes) {
        const std::size_t capacity = std::min(sizeof(buffer), kMaxHeaderBytes - request.size());
        const ssize_t bytes_received = recv(socket_fd, buffer, capacity, 0);
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
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

void configure_client_timeout(int client_socket) {
    timeval timeout{};
    timeout.tv_sec = kClientTimeoutSeconds;
    timeout.tv_usec = 0;
    if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        log_error("Failed to set client receive timeout: " + std::string(std::strerror(errno)));
    }
}

void handle_client(int client_socket, const fs::path& canonical_root, const std::string& index_file) {
    Socket client(client_socket);
    ClientSlot slot;
    configure_client_timeout(client.get());

    std::string request;
    switch (read_request_headers(client.get(), request)) {
        case HeaderReadResult::Complete:
            break;
        case HeaderReadResult::TooLarge:
            send_response(client.get(), "431 Request Header Fields Too Large");
            return;
        case HeaderReadResult::TimedOut:
            send_response(client.get(), "408 Request Timeout");
            return;
        case HeaderReadResult::Malformed:
            send_response(client.get(), "400 Bad Request");
            return;
    }

    std::string method;
    std::string target;
    std::string version;
    if (!parse_request_line(request, method, target, version)) {
        send_response(client.get(), "400 Bad Request");
        return;
    }

    log_info("Request: " + printable_for_log(method) + " " + printable_for_log(target));

    if (method != "GET") {
        send_response(client.get(), "405 Method Not Allowed", "", "text/plain; charset=utf-8", "Allow: GET\r\n");
        return;
    }

    fs::path file_path;
    switch (resolve_requested_file(canonical_root, target, index_file, file_path)) {
        case PathResolutionResult::Ok:
            break;
        case PathResolutionResult::BadRequest:
            send_response(client.get(), "400 Bad Request");
            return;
        case PathResolutionResult::Forbidden:
            send_response(client.get(), "403 Forbidden");
            log_error("Rejected unsafe path: " + printable_for_log(target));
            return;
        case PathResolutionResult::NotFound:
            send_response(client.get(), "404 Not Found");
            return;
    }

    std::string content;
    if (!read_file(file_path, content)) {
        send_response(client.get(), "500 Internal Server Error");
        log_error("Failed to read file: " + file_path.string());
        return;
    }

    send_response(client.get(), "200 OK", content, get_mime_type(file_path));
    log_info("Served: " + file_path.string());
}

void print_usage(const char* executable_name) {
    std::cerr << "Usage: " << executable_name << " [--config <path>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

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

    std::uint16_t port = 0;
    if (!parse_port(get_config_value("port", "8080"), port)) {
        log_error("The configured port must be an integer from 1 through 65535.");
        return 1;
    }

    std::error_code error;
    const fs::path canonical_root = fs::canonical(root_directory, error);
    if (error || !fs::is_directory(canonical_root, error) || error) {
        log_error("The configured root directory is not an accessible directory: " + root_directory);
        return 1;
    }

    Socket server(socket(AF_INET, SOCK_STREAM, 0));
    if (server.get() < 0) {
        log_error("Socket creation failed: " + std::string(std::strerror(errno)));
        return 1;
    }

    const int reuse_address = 1;
    if (setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        log_error("Failed to set SO_REUSEADDR: " + std::string(std::strerror(errno)));
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        log_error("bind_address must be a valid IPv4 address: " + bind_address);
        return 1;
    }

    if (bind(server.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        log_error("Bind failed: " + std::string(std::strerror(errno)));
        return 1;
    }

    if (listen(server.get(), SOMAXCONN) < 0) {
        log_error("Listen failed: " + std::string(std::strerror(errno)));
        return 1;
    }

    log_info("FakaApache listening for " + server_name + " at " + bind_address + ":" + std::to_string(port));
    log_info("Serving files from: " + canonical_root.string());
    log_info("Default index file: " + index_file);
    log_info("Concurrent-client limit: " + std::to_string(kMaxConcurrentClients));

    while (true) {
        const int client_socket = accept(server.get(), nullptr, nullptr);
        if (client_socket < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error("Accept failed: " + std::string(std::strerror(errno)));
            continue;
        }

        if (!acquire_client_slot()) {
            send_response(client_socket, "503 Service Unavailable");
            close(client_socket);
            continue;
        }

        std::thread(handle_client, client_socket, std::cref(canonical_root), std::cref(index_file)).detach();
    }
}
