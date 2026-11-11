// NU Information Exchange System - Campus Client

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std;

using Clock = chrono::steady_clock;

// -----------------------------
// Global Variables
// -----------------------------
string server_ip;
int server_tcp_port;
int server_udp_port;
string campus_name;
uint16_t my_udp_port;

int tcp_socket_fd = -1;

mutex received_msgs_mutex;
vector<string> received_messages;

// -----------------------------
long current_epoch() {
    return chrono::duration_cast<chrono::seconds>(
        chrono::system_clock::now().time_since_epoch()).count();
}

// -----------------------------
bool send_all(int sock, const string &message) {
    const char *buffer = message.c_str();
    ssize_t total_sent = 0;
    ssize_t total_len = message.size();

    while (total_sent < total_len) {
        ssize_t sent = send(sock, buffer + total_sent, total_len - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

// -----------------------------
void tcp_receive_thread() {
    char buffer[4096];
    string partial_buffer;

    while (true) {
        ssize_t received = recv(tcp_socket_fd, buffer, sizeof(buffer) - 1, 0);

        if (received <= 0) {
            cerr << "[ERROR] Server disconnected or TCP recv error. Exiting...\n";
            exit(1);
        }

        buffer[received] = 0;
        partial_buffer.append(buffer, received);

        size_t newline_pos;
        while ((newline_pos = partial_buffer.find('\n')) != string::npos) {
            string line = partial_buffer.substr(0, newline_pos);
            partial_buffer.erase(0, newline_pos + 1);

            if (line.rfind("MSG|", 0) == 0) {
                lock_guard<mutex> lock(received_msgs_mutex);
                received_messages.push_back(line);
                cout << "\n[INCOMING] " << line << "\nmenu> ";
                cout.flush();
            } else if (line.rfind("AUTH_OK", 0) == 0) {
                cout << "[INFO] Authentication successful.\n";
            } else if (line.rfind("AUTH_FAIL", 0) == 0) {
                cerr << "[ERROR] Authentication Failed. Closing client.\n";
                exit(1);
            } else {
                cout << "[SERVER] " << line << "\n";
            }
        }
    }
}

// -----------------------------
void heartbeat_thread() {
    int udp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_udp_port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    while (true) {
        string heartbeat_msg = "HEART|Campus:" + campus_name +
                               "|TS:" + to_string(current_epoch());

        sendto(udp_socket_fd, heartbeat_msg.c_str(), heartbeat_msg.size(), 0,
               (sockaddr*)&server_addr, sizeof(server_addr));

        this_thread::sleep_for(chrono::seconds(10));
    }
}

// -----------------------------
void udp_broadcast_listener() {
    int udp_socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(my_udp_port);

    bind(udp_socket_fd, (sockaddr*)&local_addr, sizeof(local_addr));

    char buffer[2048];

    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);

        ssize_t received = recvfrom(
            udp_socket_fd, buffer, sizeof(buffer) - 1, 0,
            (sockaddr*)&from, &from_len);

        if (received <= 0) continue;

        buffer[received] = 0;
        string msg(buffer);

        if (msg.rfind("BCAST|", 0) == 0)
            cout << "\n[ANNOUNCEMENT] " << msg << "\nmenu> ";
        else
            cout << "\n[UDP] " << msg << "\nmenu> ";

        cout.flush();
    }
}

// -----------------------------
// MAIN
// -----------------------------
int main(int argc, char *argv[]) {
    if (argc < 6) {
        cerr << "Usage: " << argv[0]
             << " <server_ip> <tcp_port> <udp_port> <CampusName> <my_udp_port>\n";
        return 1;
    }

    server_ip = argv[1];
    server_tcp_port = stoi(argv[2]);
    server_udp_port = stoi(argv[3]);
    campus_name = argv[4];
    my_udp_port = stoi(argv[5]);

    // ------------------
    // Connect TCP
    // ------------------
    tcp_socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_tcp_port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    if (connect(tcp_socket_fd, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("TCP connect failed");
        return 1;
    }

    cout << "[INFO] Connected to server.\n";

    // ------------------
    // Authentication (ASK USER)
    // ------------------
    string password;
    cout << "Enter password for campus '" << campus_name << "': ";
    getline(cin, password);

    if (password.empty()) {
        cerr << "[ERROR] Password cannot be empty.\n";
        return 1;
    }

    string auth_msg =
        "AUTH|Campus:" + campus_name +
        "|Pass:" + password +
        "|UDPPORT:" + to_string(my_udp_port) + "\n";

    if (!send_all(tcp_socket_fd, auth_msg)) {
        cerr << "[ERROR] Failed to send authentication.\n";
        return 1;
    }

    // ------------------
    // Launch Threads
    // ------------------
    thread(tcp_receive_thread).detach();
    thread(heartbeat_thread).detach();
    thread(udp_broadcast_listener).detach();

    // ------------------
    // Menu
    // ------------------
    cout << "---- Campus Client: " << campus_name << " ----\n";

    string input;

    while (true) {
        cout << "menu> ";
        getline(cin, input);

        if (input == "1" || input == "send") {
            string target, dept, body;
            cout << "Target Campus: ";
            getline(cin, target);
            cout << "Department: ";
            getline(cin, dept);
            cout << "Message: ";
            getline(cin, body);

            string msg = "MSG|FROM:" + campus_name +
                         "|TO:" + target +
                         "|DEPT:" + dept +
                         "|BODY:" + body + "\n";

            send_all(tcp_socket_fd, msg);
            cout << "[SENT]\n";
        }
        else if (input == "2" || input == "view") {
            lock_guard<mutex> lock(received_msgs_mutex);
            for (auto &m : received_messages) cout << m << "\n";
        }
        else if (input == "4" || input == "quit") {
            cout << "Closing client...\n";
            close(tcp_socket_fd);
            exit(0);
        }
        else {
            cout << "Commands:\n"
                    "1/send  - Send message\n"
                    "2/view  - View received\n"
                    "4/quit  - Exit\n";
        }
    }

    return 0;
}
