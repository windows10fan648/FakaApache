#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <netinet/in.h>
#include <unistd.h>

namespace fs = std::filesystem;

std::mutex cout_mutex;

// Colored logging helpers
void log_info(const std::string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "\033[32m[INFO]\033[0m " << msg << std::endl;
}

void log_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cerr << "\033[31m[ERROR]\033[0m " << msg << std::endl;
}

// Parse config file
std::unordered_map<std::string, std::string> parse_config(const std::string& filename) {
    std::unordered_map<std::string, std::string> config;
    std::ifstream file(filename);
    if (!file) {
        log_error("Failed to open config file: " + filename);
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        auto comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string key, value;
        if (std::getline(iss, key, '=') && std::getline(iss, value)) {
            auto trim = [](std::string& s) {
                size_t start = s.find_first_not_of(" \t");
                size_t end = s.find_last_not_of(" \t");
                if (start == std::string::npos || end == std::string::npos) {
                    s = "";
                    return;
                }
                s = s.substr(start, end - start + 1);
            };
            trim(key);
            trim(value);
            config[key] = value;
        }
    }
    return config;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::unordered_map<std::string, std::string> mime_types = {
    {".html", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".txt", "text/plain"}
};

std::string get_mime_type(const std::string& path) {
    auto ext = fs::path(path).extension().string();
    if (mime_types.count(ext)) return mime_types[ext];
    return "application/octet-stream";
}

bool parse_request(const std::string& request, std::string& method, std::string& path) {
    std::istringstream stream(request);
    stream >> method >> path;
    return !(method.empty() || path.empty());
}

void handle_client(int client_socket, const std::string& root_dir, const std::string& index_file) {
    char buffer[4096];
    ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer)-1, 0);
    if (bytes_received <= 0) {
        close(client_socket);
        return;
    }
    buffer[bytes_received] = '\0';
    std::string request(buffer);

    std::string method, path;
    if (!parse_request(request, method, path)) {
        log_error("Failed to parse request");
        close(client_socket);
        return;
    }

    log_info("Request: " + method + " " + path);

    if (method != "GET") {
        std::string response =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(client_socket, response.c_str(), response.size(), 0);
        close(client_socket);
        return;
    }

    std::string file_path = root_dir + path;
    if (file_path.back() == '/') file_path += index_file;

    if (!fs::exists(file_path) || fs::is_directory(file_path)) {
        std::string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(client_socket, response.c_str(), response.size(), 0);
        log_error("File not found: " + file_path);
        close(client_socket);
        return;
    }

    std::string content = read_file(file_path);
    std::string mime = get_mime_type(file_path);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: " << mime << "\r\n"
             << "Content-Length: " << content.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << content;

    send(client_socket, response.str().c_str(), response.str().size(), 0);

    close(client_socket);
    log_info("Served: " + file_path);
}

int main(int argc, char* argv[]) {
    std::string config_path = "siteconfig.fakaapache";

    // Simple CLI arg parsing for --config
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    auto config = parse_config(config_path);
    if (config.empty()) {
        log_error("Config parsing failed or config file missing. Exiting.");
        return 1;
    }

    std::string server_name = config.count("server_name") ? config["server_name"] : "localhost";
    int port = config.count("port") ? std::stoi(config["port"]) : 8080;
    std::string root_directory = config.count("root_directory") ? config["root_directory"] : "www";
    std::string index_file = config.count("index_file") ? config["index_file"] : "index.html";

    int server_fd;
    struct sockaddr_in address{};
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                   &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    log_info("FakaApache server listening on " + server_name + ":" + std::to_string(port));
    log_info("Serving files from: " + root_directory);
    log_info("Default index file: " + index_file);

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        std::thread t(handle_client, client_socket, root_directory, index_file);
        t.detach();
    }

    close(server_fd);
    return 0;
}
