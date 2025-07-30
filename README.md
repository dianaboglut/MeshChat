# Off-the-grid Emergency Network
> A decentralized ESP32 mesh network built using ESP-NOW for direct, low-power communication without Wi-Fi or internet!

# 📖 About
ESP-Mesh is a project that creates a self-organizing mesh network of ESP32 devices using the ESP-NOW protocol. The system enables devices to communicate directly without relying on routers, centralized infrastructure, or internet access.

This approach is perfect for isolated areas, distributed IoT systems, and emergency scenarios (earthquakes, forest fires, disasters) where traditional networks are unavailable or unreliable.

# ✨ Features
- Decentralized Mesh Network: Nodes dynamically join/leave and auto-reconfigure the network
- Handshake Protocol: Devices perform a handshake to integrate into the mesh
- MAC Address & Adjacency Tracking: Maintains a full list of nodes and routing paths
- Message Passing: Send messages with a structured payload (PAYLOAD, FROM, TO)
- Serial Commands:
  - LST GRID – Show adjacency matrix
  - LST HS – Show handshake history
  - ADD, DEBUG, HELP
- Isolation Detection: Nodes “scream their name” when alone to find others
- Debug Mode: Easier testing with extra log information

# 💡 Potential Applications
- Emergency communications in disaster zones
- Hiking or festival networks
- Distributed IoT sensor coordination
- Industrial environments with no centralized infrastructure

# 🛠️ Technologies
- Hardware: ESP32
- Protocol: ESP-NOW (direct device-to-device communication)
- Custom handshake & routing logic for dynamic mesh building
