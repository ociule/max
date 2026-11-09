#ifndef POLICYBRIDGE_HPP
#define POLICYBRIDGE_HPP

#include <cstdint>
#include <string>

/** Local JSON-lines transport for an external policy. Send and Poll never wait
 * for socket capacity. The controller owns the reply deadline. */
class PolicyBridge {
    intptr_t m_socket{-1};
    std::string m_input;
    std::string m_output;
    size_t m_sent{0};
    bool Connect();

public:
    ~PolicyBridge();
    PolicyBridge() = default;
    PolicyBridge(const PolicyBridge&) = delete;
    PolicyBridge& operator=(const PolicyBridge&) = delete;
    bool Send(const std::string& line);
    bool Poll(std::string& line);
    bool IsConnected() const { return m_socket != -1; }
    void Close();
};

#endif
