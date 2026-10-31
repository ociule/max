/* Copyright (c) 2022 M.A.X. Port Team
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "jevbridge.hpp"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdlib>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static constexpr uint16_t JevBridge_DefaultPort = 47811;

#if defined(_WIN32)
static constexpr intptr_t JevBridge_InvalidSocket = static_cast<intptr_t>(INVALID_SOCKET);

static void JevBridge_CloseSocket(intptr_t socket) { closesocket(static_cast<SOCKET>(socket)); }

static bool JevBridge_WouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
static constexpr intptr_t JevBridge_InvalidSocket = -1;

static void JevBridge_CloseSocket(intptr_t socket) { close(static_cast<int>(socket)); }

static bool JevBridge_WouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

JevBridge::~JevBridge() { Close(); }

bool JevBridge::Connect() {
#if defined(_WIN32)
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
#endif

    uint16_t port = JevBridge_DefaultPort;
    const char* port_text = std::getenv("MAX_JEV_BRIDGE_PORT");

    if (port_text) {
        port = static_cast<uint16_t>(std::atoi(port_text));
    }

    const auto socket_handle = static_cast<intptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));

    if (socket_handle == JevBridge_InvalidSocket) {
        return false;
    }

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    // a localhost connect either succeeds or is refused right away
    if (connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        JevBridge_CloseSocket(socket_handle);

        if (!m_reported_failure) {
            SDL_Log("Jev: the bridge is not running on port %u. Start tools/jev_bridge.py.", port);
            m_reported_failure = true;
        }

        return false;
    }

#if defined(_WIN32)
    u_long non_blocking = 1;

    ioctlsocket(static_cast<SOCKET>(socket_handle), FIONBIO, &non_blocking);
#else
    fcntl(static_cast<int>(socket_handle), F_SETFL, fcntl(static_cast<int>(socket_handle), F_GETFL) | O_NONBLOCK);
#endif

    m_socket = socket_handle;
    m_buffer.clear();

    SDL_Log("Jev: connected to the bridge on port %u.", port);

    return true;
}

bool JevBridge::Send(const std::string& line) {
    if (m_socket == JevBridge_InvalidSocket && !Connect()) {
        return false;
    }

    const std::string message = line + "\n";
    size_t sent = 0;

    while (sent < message.size()) {
        const auto count = send(m_socket, message.data() + sent, static_cast<int>(message.size() - sent), 0);

        if (count > 0) {
            sent += count;

        } else if (count < 0 && JevBridge_WouldBlock()) {
            SDL_Delay(1);

        } else {
            Close();

            return false;
        }
    }

    return true;
}

bool JevBridge::Poll(std::string& line) {
    if (m_socket == JevBridge_InvalidSocket) {
        return false;
    }

    char data[4096];

    for (;;) {
        const auto count = recv(m_socket, data, sizeof(data), 0);

        if (count > 0) {
            m_buffer.append(data, count);

        } else if (count < 0 && JevBridge_WouldBlock()) {
            break;

        } else {
            Close();

            break;
        }
    }

    const size_t end = m_buffer.find('\n');

    if (end == std::string::npos) {
        return false;
    }

    line = m_buffer.substr(0, end);
    m_buffer.erase(0, end + 1);

    return true;
}

void JevBridge::Close() {
    if (m_socket != JevBridge_InvalidSocket) {
        JevBridge_CloseSocket(m_socket);
        m_socket = JevBridge_InvalidSocket;
    }
}
