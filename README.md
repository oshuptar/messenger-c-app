# Messenger C App

This project is a solution and an extension to the laboratory task described in `task.pdf`.

It implements a multi-client chat server using **epoll** and **non-blocking I/O** in C, designed to handle multiple simultaneous users efficiently.  
Clients can connect, exchange messages, and disconnect gracefully — with full buffer management and support for slow clients.

---

## Features
- Asynchronous, non-blocking I/O with `epoll`
- Circular buffer for efficient message handling
- Clean client connection and disconnection handling
- Broadcast (“flood”) messaging to all connected users
- Safe handling of partial writes and slow clients
- Simple text-based protocol (works with any TCP client)

---

## Testing

You can test the server using **netcat (nc)**.