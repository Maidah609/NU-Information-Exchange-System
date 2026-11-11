// central_server.cpp
// Compile:
//   g++ central_server.cpp -pthread -o central_server
//
// Run:
//   ./central_server <tcp_port> <udp_port>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using Clock = chrono::steady_clock;
using TimePoint = chrono::time_point<Clock>;

// ---------------------------
// Structures & Globals
// ---------------------------
struct ClientRecord {
    int tcpSocket = -1;
    string campus;
    string ip;
    uint16_t udpPort = 0;       // where to send broadcasts for this client
    TimePoint lastHeartbeat;    // last heartbeat timestamp
};

mutex clientsMutex;
map<string, ClientRecord> activeClients;   // keyed by campus name
mutex server_console_mutex; // mutex to protect server console I/O

// Hardcoded demo credentials (server-side authoritative)
map<string, string> campusPasswords = {
    {"Lahore",    "NU-LHR-123"},
    {"Karachi",   "NU-KHI-123"},
    {"Islamabad", "NU-ISB-123"},
    {"Peshawar",  "NU-PSH-123"},
    {"Multan",    "NU-MTN-123"},
    {"CFD",       "NU-CFD-123"}
};

// ---------------------------
// Helpers
// ---------------------------
long current_epoch_seconds() {
    return chrono::duration_cast<chrono::seconds>(
        chrono::system_clock::now().time_since_epoch()).count();
}

// Extract value from messages containing key:value segments.
// Example: get_field_value("AUTH|Campus:Lahore|Pass:abc|UDPPORT:6001", "Campus") -> "Lahore"
string get_field_value(const string &msg, const string &key) {
    size_t pos = msg.find(key + ":");
    if (pos == string::npos) return "";
    pos += key.size() + 1;
    size_t end = msg.find('|', pos);
    if (end == string::npos) end = msg.find('\n', pos);
    if (end == string::npos) end = msg.size();
    return msg.substr(pos, end - pos);
}

// Safe parse of uint16_t from string; returns false on error.
bool parse_uint16(const string &s, uint16_t &out) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit(c)) return false;
    try {
        long v = stol(s);
        if (v < 0 || v > 65535) return false;
        out = static_cast<uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

// send all bytes reliably (returns false if send fails)
bool send_full(int sock, const string &data) {
    const char *buf = data.c_str();
    ssize_t total = 0;
    ssize_t len = (ssize_t)data.size();
    while (total < len) {
        ssize_t n = send(sock, buf + total, (size_t)(len - total), 0);
        if (n < 0) {
            // transient EINTR handling
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += n;
    }
    return true;
}

// Close socket and ignore errors
void safe_close(int fd) {
    if (fd >= 0) close(fd);
}

// ---------------------------
// TCP Client Thread
// - authenticate on first line
// - then forward MSG lines to destination campus via TCP
// - maintains activeClients map
// ---------------------------
void handle_tcp_client(int clientSock, string clientIP) {
    char buffer[4096];
    // Read first line (authentication)
    ssize_t n = recv(clientSock, buffer, sizeof(buffer)-1, 0);
    if (n <= 0) {
        safe_close(clientSock);
        cerr << "[WARN] Connection closed before auth from " << clientIP << "\n";
        return;
    }
    buffer[n] = 0;
    string line(buffer);

    // Trim leading/trailing whitespace/newlines
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

    if (line.rfind("AUTH|", 0) != 0) {
        send_full(clientSock, "AUTH_FAIL|ERR:BadFormat\n");
        safe_close(clientSock);
        cerr << "[WARN] Invalid auth format from " << clientIP << "\n";
        return;
    }

    // parse auth fields
    string campus = get_field_value(line, "Campus");
    string pass   = get_field_value(line, "Pass");
    string udpStr = get_field_value(line, "UDPPORT");

    // Validate required fields
    if (campus.empty() || pass.empty() || udpStr.empty()) {
        send_full(clientSock, "AUTH_FAIL|ERR:MissingFields\n");
        safe_close(clientSock);
        cerr << "[WARN] Missing auth fields from " << clientIP << "\n";
        return;
    }

    // Validate UDP port numeric
    uint16_t udpPort = 0;
    if (!parse_uint16(udpStr, udpPort)) {
        send_full(clientSock, "AUTH_FAIL|ERR:BadUDPPort\n");
        safe_close(clientSock);
        cerr << "[WARN] Invalid UDP port from " << clientIP << " : " << udpStr << "\n";
        return;
    }

    // Validate campus exists in server credentials
    auto itCred = campusPasswords.find(campus);
    if (itCred == campusPasswords.end()) {
        send_full(clientSock, "AUTH_FAIL|ERR:UnknownCampus\n");
        safe_close(clientSock);
        cerr << "[WARN] Unknown campus '" << campus << "' from " << clientIP << "\n";
        return;
    }

    // Password check
    if (itCred->second != pass) {
        send_full(clientSock, "AUTH_FAIL|ERR:WrongPassword\n");
        safe_close(clientSock);
        cerr << "[WARN] Wrong password for campus '" << campus << "' from " << clientIP << "\n";
        return;
    }

    // Prevent duplicate login for same campus
    {
        lock_guard<mutex> lk(clientsMutex);
        auto it = activeClients.find(campus);
        if (it != activeClients.end() && it->second.tcpSocket >= 0) {
            send_full(clientSock, "AUTH_FAIL|ERR:AlreadyOnline\n");
            safe_close(clientSock);
            cerr << "[WARN] Duplicate login attempt for campus '" << campus << "'\n";
            return;
        }
    }

    // Register client
    {
        lock_guard<mutex> lk(clientsMutex);
        ClientRecord rec;
        rec.tcpSocket = clientSock;
        rec.campus = campus;
        rec.ip = clientIP;
        rec.udpPort = udpPort;
        rec.lastHeartbeat = Clock::now();
        activeClients[campus] = rec;
    }

    cout << "[INFO] Authenticated campus: " << campus
         << " (TCP sock: " << clientSock << ", UDP port: " << udpPort
         << ", ip: " << clientIP << ")\n";

    // Acknowledge to client
    if (!send_full(clientSock, string("AUTH_OK|Campus:" + campus + "\n"))) {
        cerr << "[WARN] Failed to send AUTH_OK to " << campus << "\n";
        // keep connection but log warning
    }

    // Now enter receive loop for this client
    string recvbuf;
    while (true) {
        ssize_t r = recv(clientSock, buffer, sizeof(buffer)-1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            cerr << "[ERROR] recv error for campus " << campus << " : " << strerror(errno) << "\n";
            break;
        }
        if (r == 0) {
            // graceful disconnect
            cout << "[INFO] Campus " << campus << " disconnected (sock " << clientSock << ")\n";
            break;
        }

        buffer[r] = 0;
        recvbuf.append(buffer, r);

        // process full lines
        size_t pos;
        while ((pos = recvbuf.find('\n')) != string::npos) {
            string msgline = recvbuf.substr(0, pos);
            recvbuf.erase(0, pos + 1);

            // ignore empty lines
            if (msgline.empty()) continue;

            if (msgline.rfind("MSG|", 0) == 0) {
                // Validate presence of TO:
                string toCampus = get_field_value(msgline, "TO");
                if (toCampus.empty()) {
                    send_full(clientSock, "ERR|BAD_MSG|Reason:MissingTO\n");
                    continue;
                }
                // Optional: prevent spammy self-sends (allow if you want)
                // if (toCampus == campus) { send_full(...); continue; }

                lock_guard<mutex> lk(clientsMutex);
                auto it = activeClients.find(toCampus);
                if (it == activeClients.end() || it->second.tcpSocket < 0) {
                    send_full(clientSock, string("ERR|DEST_OFFLINE|TO:" + toCampus + "\n"));
                } else {
                    int destSock = it->second.tcpSocket;
                    if (!send_full(destSock, msgline + "\n")) {
                        cerr << "[WARN] Failed to forward to " << toCampus << "\n";
                        // Mark destination offline and remove
                        safe_close(destSock);
                        activeClients.erase(it);
                        send_full(clientSock, string("ERR|DEST_OFFLINE|TO:" + toCampus + "\n"));
                    } else {
                        // forwarded successfully - optionally log
                        cout << "[INFO] Forwarded message from " << campus << " to " << toCampus << "\n";
                    }
                }
            } else {
                // Unknown command from client: log and optionally reply
                cerr << "[INFO] Unknown TCP command from " << campus << " : " << msgline << "\n";
            }
        }
    } // end recv loop

    // cleanup client record on disconnect
    {
        lock_guard<mutex> lk(clientsMutex);
        auto it = activeClients.find(campus);
        if (it != activeClients.end() && it->second.tcpSocket == clientSock) {
            activeClients.erase(it);
            cout << "[INFO] Removed client record for " << campus << "\n";
        }
    }
    safe_close(clientSock);
}

// ---------------------------
// UDP Heartbeat Listener
// - listens for HEART|Campus:... messages and updates lastHeartbeat
// ---------------------------
void udp_listener_fn(int udp_sock) {
    char buf[2048];
    sockaddr_in cliaddr{};
    socklen_t len = sizeof(cliaddr);

    while (true) {
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf)-1, 0, (sockaddr*)&cliaddr, &len);
        if (n < 0) {
            if (errno == EINTR) continue;
            cerr << "[ERROR] recvfrom udp: " << strerror(errno) << "\n";
            continue;
        }
        if (n == 0) continue;

        buf[n] = 0;
        string msg(buf);

        if (msg.rfind("HEART|", 0) == 0) {
            string campus = get_field_value(msg, "Campus");
            if (campus.empty()) {
                cerr << "[HEART] Missing Campus field in heartbeat\n";
                continue;
            }

            string sender_ip = inet_ntoa(cliaddr.sin_addr);

            lock_guard<mutex> lk(clientsMutex);
            auto it = activeClients.find(campus);

            if (it != activeClients.end()) {
                it->second.lastHeartbeat = Clock::now();
                // update IP if we didn't have it or it differs (useful for NAT cases)
                if (it->second.ip.empty()) it->second.ip = sender_ip;
                cout << "[HEART] " << campus << " @ " << sender_ip << " updated\n";
            } else {
                // Heartbeat from unauthenticated client: log it and optionally add minimal record
                cout << "[HEART] Heartbeat from unknown/not-connected campus '" << campus << "' @ " << sender_ip << "\n";
                // Optionally create a placeholder entry (tcpSocket = -1)
                ClientRecord cr;
                cr.tcpSocket = -1;
                cr.campus = campus;
                cr.ip = sender_ip;
                cr.udpPort = 0;
                cr.lastHeartbeat = Clock::now();
                activeClients[campus] = cr;
            }
        } else {
            cout << "[UDP] Unknown UDP msg: " << msg << "\n";
        }
    }
}

// ---------------------------
// Admin Console
// - commands: list, bcast <message>, help, quit
// - bcast sends UDP datagram to known client udp endpoints
// ---------------------------
void admin_console_fn(int udp_sock) {
    cout << "\n---- ADMIN CONSOLE ----\n";
    cout << "Commands: list | bcast <message> | help | quit\n";

    string line;
    while (true) {
        cout << "admin> ";
        if (!getline(cin, line)) {
            // EOF on stdin -> exit console
            cout << "Admin console input closed, continuing server.\n";
            break;
        }

        if (line.empty()) continue;
        if (line == "help") {
            cout << "Commands:\n";
            cout << "  list             - show connected campuses and last heartbeat\n";
            cout << "  bcast <message>  - broadcast UDP announcement to all known campuses\n";
            cout << "  quit             - exit server\n";
        } else if (line == "list") {
            lock_guard<mutex> lk(clientsMutex);
            if (activeClients.empty()) {
                cout << "[INFO] No known/active campuses.\n";
                continue;
            }
            cout << "Connected campuses:\n";
            for (auto &p : activeClients) {
                auto &ci = p.second;
                long secs = chrono::duration_cast<chrono::seconds>(Clock::now() - ci.lastHeartbeat).count();
                cout << " - " << ci.campus
                     << " | TCP: " << ci.tcpSocket
                     << " | ip: " << (ci.ip.empty() ? "unknown" : ci.ip)
                     << " | udp_port: " << ci.udpPort
                     << " | last hb(s) ago: " << secs << "\n";
            }
        } else if (line.rfind("bcast ", 0) == 0) {
            string msg = line.substr(6);
            if (msg.empty()) {
                cout << "[ERR] Provide a non-empty message to bcast.\n";
                continue;
            }

            string bcast = "BCAST|TS:" + to_string(current_epoch_seconds()) + "|BODY:" + msg;

            lock_guard<mutex> lk(clientsMutex);
            for (auto &p : activeClients) {
                auto &ci = p.second;
                if (ci.ip.empty() || ci.udpPort == 0) {
                    cout << "[WARN] Skipping " << ci.campus << " (no UDP endpoint known)\n";
                    continue;
                }

                sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(ci.udpPort);
                if (inet_pton(AF_INET, ci.ip.c_str(), &dest.sin_addr) != 1) {
                    cout << "[WARN] Invalid IP for " << ci.campus << ": " << ci.ip << "\n";
                    continue;
                }

                ssize_t s = sendto(udp_sock, bcast.c_str(), bcast.size(), 0, (sockaddr*)&dest, sizeof(dest));
                if (s <= 0) {
                    cout << "[BCAST] Failed to send to " << ci.campus << " (" << ci.ip << ":" << ci.udpPort << ")\n";
                } else {
                    cout << "[BCAST] Sent to " << ci.campus << " " << ci.ip << ":" << ci.udpPort << "\n";
                }
            }
        } else if (line == "quit") {
            cout << "Shutting down server (admin requested quit)...\n";
            // immediate exit (no graceful thread join)
            exit(0);
        } else {
            cout << "Unknown command. Type help.\n";
        }
    }
}

// ---------------------------
// Main
// ---------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        cerr << "Usage: " << argv[0] << " <tcp_port> <udp_port>\n";
        cerr << "Example: " << argv[0] << " 5000 5001\n";
        return 1;
    }

    // parse ports defensively
    int tcp_port = 0, udp_port = 0;
    try {
        tcp_port = stoi(string(argv[1]));
        udp_port = stoi(string(argv[2]));
    } catch (...) {
        cerr << "[ERROR] Invalid port numbers.\n";
        return 1;
    }
    if (tcp_port <= 0 || udp_port <= 0) {
        cerr << "[ERROR] Ports must be positive integers.\n";
        return 1;
    }

    // ---- create TCP listening socket ----
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }
    int opt = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        safe_close(listenfd);
        return 1;
    }

    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(tcp_port);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind");
        safe_close(listenfd);
        return 1;
    }
    if (listen(listenfd, 10) < 0) {
        perror("listen");
        safe_close(listenfd);
        return 1;
    }
    cout << "[START] TCP listening on port " << tcp_port << "\n";

    // ---- create UDP socket (for heartbeats + admin broadcast) ----
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("udp socket"); safe_close(listenfd); return 1; }
    sockaddr_in udpaddr{};
    udpaddr.sin_family = AF_INET;
    udpaddr.sin_addr.s_addr = INADDR_ANY;
    udpaddr.sin_port = htons(udp_port);
    if (bind(udp_sock, (sockaddr*)&udpaddr, sizeof(udpaddr)) < 0) {
        perror("udp bind");
        safe_close(listenfd);
        safe_close(udp_sock);
        return 1;
    }
    cout << "[START] UDP listening on port " << udp_port << " (for heartbeats)\n";

    // ---- launch background threads ----
    thread udp_thread(udp_listener_fn, udp_sock);
    udp_thread.detach();

    thread admin_thread(admin_console_fn, udp_sock);
    admin_thread.detach();

    // ---- accept loop ----
    while (true) {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int connfd = accept(listenfd, (sockaddr*)&cli, &clilen);
        if (connfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        string client_ip = inet_ntoa(cli.sin_addr);
        cout << "[CONNECT] New TCP connection from " << client_ip << " (sock " << connfd << ")\n";
        thread t(handle_tcp_client, connfd, client_ip);
        t.detach();
    }

    // never reached
    safe_close(listenfd);
    safe_close(udp_sock);
    return 0;
}

