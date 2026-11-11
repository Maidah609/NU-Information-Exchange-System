# NU Information Exchange System

### Computer Networks Lab Project

A multi-campus communication system developed using **C++ Socket Programming** and **Cisco Packet Tracer**. The project demonstrates how different university campuses can communicate through a centralized server using TCP and UDP protocols.

It combines practical client-server programming with network topology design and routing simulation.

## Project Overview

The NU Information Exchange System allows multiple university campuses to exchange messages through a central server. Each campus operates as a client and connects to the server for authentication and communication.

The project consists of two main components:

### 1. Client-Server Communication (C++)
- Centralized server supporting multiple campus clients
- TCP-based authentication and message exchange
- UDP heartbeat monitoring every 10 seconds
- UDP announcements from the central server
- Multithreading for handling concurrent connections
- Admin console for monitoring connected campuses

### 2. Network Design (Cisco Packet Tracer)
- Multi-campus network topology
- Router and switch configuration
- RIPv2 dynamic routing
- IP addressing and subnetting
- Communication between different campus networks
- Connectivity testing

## Technologies Used

- **Programming Language:** C++
- **Operating System:** Ubuntu/Linux
- **Networking:** TCP/IP, UDP, Socket Programming
- **Concurrency:** C++ Threads and Mutex
- **Network Simulation:** Cisco Packet Tracer
- **Routing Protocol:** RIPv2
- **Compiler:** G++

## Project Structure

```text
NU-Information-Exchange-System/
├── README.md
├── central_server.cpp
├── campus_client.cpp
├── Campus-Network.pkt
└── Lab-Project-Report.pdf
```

## How to Run

### Step 1: Compile the Programs

Open the terminal in the project directory and execute:

```bash
g++ central_server.cpp -pthread -o central_server
g++ campus_client.cpp -pthread -o campus_client
```

### Step 2: Start the Central Server

```bash
./central_server 5000 5001
```

### Step 3: Connect Campus Clients

Open separate terminals for each campus.

**Lahore:**
```bash
./campus_client 127.0.0.1 5000 5001 Lahore 6001
```

**Karachi:**
```bash
./campus_client 127.0.0.1 5000 5001 Karachi 6002
```

**Islamabad:**
```bash
./campus_client 127.0.0.1 5000 5001 Islamabad 6003
```

Enter the corresponding campus demo password when prompted. The demonstration credentials are defined in `central_server.cpp`.

### Step 4: Test Communication

Once authenticated, campus clients can send messages to other connected campuses.

The server administrator can use the following commands:

- `list` — Display connected campuses
- `bcast <message>` — Send an announcement to connected campuses
- `help` — Display available commands
- `quit` — Shut down the server

### Step 5: Explore the Network Simulation

Open `Campus-Network.pkt` in Cisco Packet Tracer to explore the network topology, RIPv2 configurations, and connectivity between campuses.

## Learning Outcomes

This project helped us gain practical experience in:

- TCP and UDP socket programming
- Client-server architecture
- Concurrent programming using multithreading
- Network routing and configuration
- Designing and testing multi-campus networks

## Team Members

- Maidah Nasir
- Anza Naseer

## Project Type

Computer Networks Lab — University Project

*This project was developed for educational purposes. Authentication uses hardcoded demonstration credentials and unencrypted TCP connections; it is not intended for production deployment.*
