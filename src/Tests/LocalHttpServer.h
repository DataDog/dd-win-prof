// Unless explicitly stated otherwise all files in this repository are licensed under
// the Apache 2 License. This product includes software developed at Datadog
// (https://www.datadoghq.com/). Copyright 2025 Datadog, Inc.

#pragma once

#include <winsock2.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const std::string& value) : name(name) {
    const auto size = GetEnvironmentVariableA(name, nullptr, 0);
    previous.resize(size);
    if (size) {
      previous.resize(GetEnvironmentVariableA(name, previous.data(), size));
    }
    SetEnvironmentVariableA(name, value.c_str());
  }
  ~ScopedEnvironment() {
    SetEnvironmentVariableA(name, previous.empty() ? nullptr : previous.c_str());
  }

 private:
  const char* name;
  std::string previous;
};

// Loopback-only HTTP capture. Status 0 disconnects; -1 exceeds the export timeout.
class LocalHttpServer {
 public:
  struct Request {
    std::string headers;
    std::string body;
  };

  explicit LocalHttpServer(std::vector<int> statuses = {202})
      : statuses(std::move(statuses)) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    if (listener == INVALID_SOCKET ||
        bind(listener, reinterpret_cast<sockaddr*>(&address), size) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0 ||
        listen(listener, SOMAXCONN) != 0) {
      closesocket(listener);
      WSACleanup();
      throw std::runtime_error("Loopback listener failed");
    }
    url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    worker = std::thread([this] { Serve(); });
  }

  ~LocalHttpServer() {
    stopping = true;
    shutdown(listener, SD_BOTH);
    closesocket(listener);
    worker.join();
    WSACleanup();
  }

  const std::string& Url() const { return url; }
  std::vector<Request> Requests() const {
    std::lock_guard lock(mutex);
    return requests;
  }
  std::string Error() const {
    std::lock_guard lock(mutex);
    return error;
  }

 private:
  static void Receive(SOCKET client, std::string& buffer, size_t size) {
    while (buffer.size() < size) {
      if (buffer.size() > 4 * 1024 * 1024) {
        throw std::runtime_error("HTTP request too large");
      }
      char bytes[8192];
      const auto count = recv(client, bytes, sizeof(bytes), 0);
      if (count <= 0) throw std::runtime_error("Incomplete HTTP request");
      buffer.append(bytes, count);
    }
  }

  static std::string Line(SOCKET client, std::string& buffer, size_t& offset) {
    auto end = buffer.find("\r\n", offset);
    while (end == std::string::npos) {
      Receive(client, buffer, buffer.size() + 1);
      end = buffer.find("\r\n", offset);
    }
    auto line = buffer.substr(offset, end - offset);
    offset = end + 2;
    return line;
  }

  static Request Read(SOCKET client) {
    std::string buffer;
    size_t offset = 0;
    Request request;
    std::string line;
    do {
      line = Line(client, buffer, offset);
      request.headers += line + "\r\n";
    } while (!line.empty());
    auto headers = request.headers;
    std::transform(
        headers.begin(), headers.end(), headers.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        }
    );
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
      for (;;) {
        const auto size = std::stoull(Line(client, buffer, offset), nullptr, 16);
        if (size == 0) {
          while (!Line(client, buffer, offset).empty()) {
          }
          break;
        }
        Receive(client, buffer, offset + size + 2);
        request.body.append(buffer, offset, size);
        offset += size;
        if (!Line(client, buffer, offset).empty()) {
          throw std::runtime_error("Invalid HTTP chunk terminator");
        }
      }
    } else {
      const auto length = headers.find("content-length:");
      if (length == std::string::npos) throw std::runtime_error("Missing HTTP length");
      const auto size = std::stoull(headers.substr(length + 15));
      Receive(client, buffer, offset + size);
      request.body = buffer.substr(offset, size);
    }
    return request;
  }

  void Serve() {
    while (!stopping) {
      fd_set ready;
      FD_ZERO(&ready);
      FD_SET(listener, &ready);
      timeval timeout{0, 100000};
      if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
      const auto client = accept(listener, nullptr, nullptr);
      if (client == INVALID_SOCKET) break;
      const DWORD receiveTimeout = 2000;
      setsockopt(
          client,
          SOL_SOCKET,
          SO_RCVTIMEO,
          reinterpret_cast<const char*>(&receiveTimeout),
          sizeof(receiveTimeout)
      );
      try {
        auto request = Read(client);
        int status;
        {
          std::lock_guard lock(mutex);
          status = statuses[std::min(requests.size(), statuses.size() - 1)];
          requests.push_back(std::move(request));
        }
        if (status == -1) {
          const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(11);
          while (!stopping && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        } else if (status > 0) {
          const auto response =
              "HTTP/1.1 " + std::to_string(status) +
              " Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
          if (send(client, response.data(), static_cast<int>(response.size()), 0) !=
              static_cast<int>(response.size())) {
            throw std::runtime_error("HTTP response failed");
          }
        }
      } catch (const std::exception& exception) {
        std::lock_guard lock(mutex);
        error = exception.what();
      }
      closesocket(client);
    }
  }

  SOCKET listener = INVALID_SOCKET;
  std::vector<int> statuses;
  std::string url;
  std::atomic<bool> stopping = false;
  std::thread worker;
  mutable std::mutex mutex;
  std::vector<Request> requests;
  std::string error;
};
