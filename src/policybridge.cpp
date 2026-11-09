#include "policybridge.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
static void CloseSocket(intptr_t s) { closesocket(static_cast<SOCKET>(s)); }
static bool WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
static void CloseSocket(intptr_t s) { close(static_cast<int>(s)); }
static bool WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

namespace {
constexpr size_t MaxMessage = 16 * 1024 * 1024;
constexpr size_t BytesPerPoll = 64 * 1024;
}

PolicyBridge::~PolicyBridge() { Close(); }

bool PolicyBridge::Connect() {
#if defined(_WIN32)
    static const bool initialized = []() {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!initialized) return false;
#endif
    long port = 47812;
    if (const char* value = std::getenv("MAX_POLICY_BRIDGE_PORT")) {
        char* end;
        port = std::strtol(value, &end, 10);
        if (!*value || *end || port < 1 || port > 65535) return false;
    }
    const auto handle = static_cast<intptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (handle == -1) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    // Only loopback is supported; connect completes or is refused locally.
    if (connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CloseSocket(handle);
        return false;
    }
#if defined(_WIN32)
    u_long enabled = 1;
    if (ioctlsocket(static_cast<SOCKET>(handle), FIONBIO, &enabled) != 0) {
#else
    if (fcntl(static_cast<int>(handle), F_SETFL, O_NONBLOCK) == -1) {
#endif
        CloseSocket(handle);
        return false;
    }
#if defined(SO_NOSIGPIPE)
    int enabled = 1;
    setsockopt(static_cast<int>(handle), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    m_socket = handle;
    return true;
}

bool PolicyBridge::Send(const std::string& line) {
    if (line.size() >= MaxMessage || !m_output.empty()) return false;
    if (!IsConnected() && !Connect()) return false;
    m_output = line + '\n';
    m_sent = 0;
    return true;
}

bool PolicyBridge::Poll(std::string& line) {
    if (!IsConnected()) return false;
    if (!m_output.empty()) {
        const auto remaining = std::min(BytesPerPoll, m_output.size() - m_sent);
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags = MSG_NOSIGNAL;
#endif
        const auto count = send(m_socket, m_output.data() + m_sent, static_cast<int>(remaining), flags);
        if (count > 0) {
            m_sent += count;
            if (m_sent == m_output.size()) m_output.clear();
        } else if (count == 0 || !WouldBlock()) {
            Close();
            return false;
        }
    }
    char buffer[4096];
    size_t received = 0;
    while (received < BytesPerPoll && m_input.find('\n') == std::string::npos) {
        const auto count = recv(m_socket, buffer, sizeof(buffer), 0);
        if (count > 0) {
            m_input.append(buffer, count);
            received += count;
            if (m_input.size() >= MaxMessage) {
                Close();
                return false;
            }
        } else if (count < 0 && WouldBlock()) {
            break;
        } else {
            Close();
            return false;
        }
    }
    const auto end = m_input.find('\n');
    if (end == std::string::npos) return false;
    line = m_input.substr(0, end);
    m_input.erase(0, end + 1);
    return true;
}

void PolicyBridge::Close() {
    if (IsConnected()) CloseSocket(m_socket);
    m_socket = -1;
    m_input.clear();
    m_output.clear();
    m_sent = 0;
}
