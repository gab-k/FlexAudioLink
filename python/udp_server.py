import socket

# --- Configuration ---
# Use '0.0.0.0' to listen on all available network interfaces (Wi-Fi, Ethernet, etc.)
# This makes the server accessible from any device on your local network.
HOST_IP = '0.0.0.0'
HOST_PORT = 5005  # The port your ESP32 will send data to. You can change this.
BUFFER_SIZE = 1024 # Max amount of data to receive in one go (in bytes).

# --- Helper function to get your PC's local IP ---
def get_local_ip():
    """
    Tries to find the local IP address of the machine.
    This is the address you will need to configure on your ESP32 client.
    """
    s = None
    try:
        # Create a temporary socket to a public DNS server to find the local IP
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Doesn't actually send data, just connects to find the interface IP
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        # Fallback if the above method fails (e.g., no internet connection)
        ip = socket.gethostbyname(socket.gethostname())
    finally:
        if s:
            s.close()
    return ip

# --- Main Server Logic ---
def run_server():
    """
    Initializes and runs the UDP server.
    """
    # 1. Get the server's IP address and print it for the user
    server_ip = get_local_ip()
    print(f"--- UDP Server Starting ---")
    print(f"Your PC's IP Address is: {server_ip}")
    print(f"Configure your ESP32 client to send data to this IP on port {HOST_PORT}.")
    print("-----------------------------")

    # 2. Create a UDP socket
    #    AF_INET means we are using IPv4 addresses.
    #    SOCK_DGRAM means we are using UDP.
    try:
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
        # 3. Bind the socket to our IP and Port
        #    This tells the OS that this script wants to receive all UDP packets
        #    sent to this specific port.
        server_socket.bind((HOST_IP, HOST_PORT))
        
        print(f"Server is listening on port {HOST_PORT}...")
        print("Waiting to receive data from the headset...")
        print("Press Ctrl+C to stop the server.")

    except OSError as e:
        print(f"Error: Could not bind to port {HOST_PORT}. Is another program using it?")
        print(e)
        return

    # 4. Main loop to continuously listen for data
    while True:
        try:
            # 5. Receive data from a client
            #    This line will "block" (wait) until a packet is received.
            #    'data' will contain the bytes received (e.g., microphone audio).
            #    'client_address' will be a tuple: (client_ip, client_port).
            data, client_address = server_socket.recvfrom(BUFFER_SIZE)

            # The server has now automatically learned the client's address!
            # Instead of creating a new message, just echo the received data back.
            if data:
                server_socket.sendto(data, client_address)
                print(f"[+] Received {len(data)} bytes from {client_address}, echoed back.")
            
            # # Try to decode the data as a UTF-8 string for printing.
            # # If your ESP32 sends raw bytes, this might show strange characters.
            # try:
            #     message = data.decode('utf-8')
            #     print(f"    Message: '{message}'")
            # except UnicodeDecodeError:
            #     print(f"    Received {len(data)} bytes of binary data.")

            # # 6. Process data and send a response back
            # #    This simulates sending the music stream back to the headset.
            # response_message = b"Audio chunk #1 received. Sending music data back."
            
            # server_socket.sendto(response_message, client_address)
            # print(f"    -> Sent response back to {client_address}")

        except KeyboardInterrupt:
            # Handle Ctrl+C to gracefully shut down the server
            print("\nServer is shutting down.")
            break
        except Exception as e:
            print(f"\nAn error occurred: {e}")
            break

    # 7. Close the socket when the loop is broken
    server_socket.close()
    print("Server socket closed.")


# --- Run the server ---
if __name__ == "__main__":
    run_server()