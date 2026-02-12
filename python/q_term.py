"""
Quad-Port Serial Console & Auto-Discovery Tool
----------------------------------------------
A multi-threaded GUI dashboard for monitoring and interacting with up to four 
USB-Serial devices simultaneously. Designed for testing and embedded 
development where device enumeration (COM port number) changes frequently.

Architecture:
- GUI: Tkinter-based 2x2 grid layout.
- Concurrency: 
    - Main thread handles UI event loop.
    - Background "Watchdog" threads (1Hz) scan for specific USB Serial Numbers.
    - Dedicated RX threads for each connected port to prevent UI blocking.
- I/O Handling: 
    - Implements a 50ms read buffer to coalesce fragmented serial packets into 
      readable lines (fixes "split line" display issues).
    - Auto-reconnect logic handles hot-plugging events gracefully.

Configuration:
Requires 'q_term_config.json' in the working directory. Maps "Friendly Name" to 
"USB Serial Number". See JSON format below:
    [
        {"name": "Sensor A", "usb_serial": "001066062303", "baud": 115200},
        ...
    ]

Dependencies: pyserial, tkinter
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import datetime
import time
import json
import os

class AutoSerialConsole(tk.LabelFrame):
    def __init__(self, parent, config, *args, **kwargs):
        """
        config: dict containing "name", "usb_serial", "baud"
        """
        self.target_name = config.get("name", "Unknown")
        self.target_serial = config.get("usb_serial", "").strip()
        self.target_baud = config.get("baud", 115200)
        
        super().__init__(parent, text=f" {self.target_name} (Searching...) ", padx=5, pady=5, *args, **kwargs)
        
        self.serial_conn = None
        self.is_connected = False
        self.stop_thread = threading.Event()
        
        # History
        self.command_history = []
        self.history_index = 0

        # --- Top Info ---
        top_frame = tk.Frame(self)
        top_frame.pack(fill=tk.X, pady=2)
        
        # Status Label
        self.lbl_status = tk.Label(top_frame, text=f"Waiting for device...", fg="orange", font=("Arial", 9, "bold"))
        self.lbl_status.pack(side=tk.LEFT)
        
        # Target Serial Label
        lbl_target = tk.Label(top_frame, text=f"Target: {self.target_serial[:20]}", fg="#888")
        lbl_target.pack(side=tk.RIGHT)

        # --- Console Text ---
        self.console_text = scrolledtext.ScrolledText(self, state='disabled', height=10, width=40, bg="#1e1e1e", fg="#00ff00", font=("Consolas", 9))
        self.console_text.pack(fill=tk.BOTH, expand=True, pady=5)
        
        self.console_text.tag_config('tx', foreground='#00ffff')
        self.console_text.tag_config('rx', foreground='#00ff00')
        self.console_text.tag_config('sys', foreground='#aaaaaa', font=("Consolas", 9, "italic"))
        self.console_text.tag_config('err', foreground='#ff5555', font=("Consolas", 9, "bold"))

        # --- Input Area ---
        input_frame = tk.Frame(self)
        input_frame.pack(fill=tk.X)
        
        self.entry_input = tk.Entry(input_frame, font=("Consolas", 9))
        self.entry_input.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 5))
        
        self.entry_input.bind("<Return>", lambda event: self.send_data())
        self.entry_input.bind("<Up>", self.navigate_history_up)
        self.entry_input.bind("<Down>", self.navigate_history_down)
        
        # Send Button
        self.btn_send = tk.Button(input_frame, text="Send", command=self.send_data, state="disabled", width=6)
        self.btn_send.pack(side=tk.RIGHT)

        # Individual Clear Button
        self.btn_clear = tk.Button(input_frame, text="Clear", command=self.clear_console, width=5)
        self.btn_clear.pack(side=tk.RIGHT, padx=5)

        # Start the Auto-Connect Watchdog
        self.after(1000, self.auto_connect_watchdog)

    def log(self, message, tag='sys'):
        def _append():
            self.console_text.config(state='normal')
            timestamp = datetime.datetime.now().strftime("%H:%M:%S")
            self.console_text.insert(tk.END, f"[{timestamp}] {message}\n", tag)
            self.console_text.see(tk.END)
            self.console_text.config(state='disabled')
        self.after(0, _append)

    def clear_console(self):
        """Clears the text area for this specific console."""
        self.console_text.config(state='normal')
        self.console_text.delete(1.0, tk.END)
        self.console_text.config(state='disabled')

    def auto_connect_watchdog(self):
        if not self.is_connected:
            found_port = self.find_port_by_serial(self.target_serial)
            if found_port:
                self.connect(found_port)
            else:
                self.lbl_status.config(text="Searching...", fg="orange")
                self.config(text=f" {self.target_name} (Searching...) ", fg="black")
        
        self.after(1000, self.auto_connect_watchdog)

    def find_port_by_serial(self, target_serial):
        ports = serial.tools.list_ports.comports()
        for port in ports:
            if port.serial_number and port.serial_number.strip().upper() == target_serial.strip().upper():
                return port.device
        return None

    def connect(self, port_name):
        try:
            self.serial_conn = serial.Serial(port_name, baudrate=self.target_baud, timeout=0.05)
            self.is_connected = True
            
            self.stop_thread.clear()
            self.btn_send.config(state="normal")
            self.lbl_status.config(text=f"Connected: {port_name}", fg="green")
            self.config(text=f" {self.target_name} (ONLINE) ", fg="green")
            
            self.log(f"Auto-connected to {port_name}", 'sys')

            threading.Thread(target=self.read_loop, daemon=True).start()
            
        except serial.SerialException as e:
            self.log(f"Connection Failed: {e}", 'err')

    def disconnect(self):
        self.stop_thread.set()
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        
        self.is_connected = False
        self.btn_send.config(state="disabled")
        self.lbl_status.config(text="Disconnected", fg="red")
        self.log("Device Disconnected.", 'err')

    def read_loop(self):
        rx_buffer = ""
        last_rx_time = time.time()
        BUFFER_TIMEOUT = 0.05 

        while not self.stop_thread.is_set() and self.serial_conn and self.serial_conn.is_open:
            try:
                if self.serial_conn.in_waiting > 0:
                    char_data = self.serial_conn.read(self.serial_conn.in_waiting).decode('utf-8', errors='replace')
                    rx_buffer += char_data
                    last_rx_time = time.time()

                    if '\n' in rx_buffer:
                        lines = rx_buffer.split('\n')
                        for line in lines[:-1]:
                            clean = line.strip('\r')
                            if clean: self.log(f"RX: {clean}", 'rx')
                        rx_buffer = lines[-1]
                else:
                    if rx_buffer and (time.time() - last_rx_time > BUFFER_TIMEOUT):
                        self.log(f"RX: {rx_buffer.strip()}", 'rx')
                        rx_buffer = ""
                    time.sleep(0.01)

            except serial.SerialException:
                self.stop_thread.set()
                self.after(0, self.disconnect)
                break

    def send_data(self):
        if self.is_connected and self.serial_conn:
            data = self.entry_input.get()
            if data:
                try:
                    self.serial_conn.write((data + "\n").encode('utf-8'))
                    self.log(f"TX: {data}", 'tx')
                    
                    if not self.command_history or self.command_history[-1] != data:
                        self.command_history.append(data)
                    self.history_index = len(self.command_history)
                    
                    self.entry_input.delete(0, tk.END)
                except Exception as e:
                    self.log(f"Send Error: {e}", 'err')

    def navigate_history_up(self, event):
        if self.command_history and self.history_index > 0:
            self.history_index -= 1
            self.update_input_from_history()

    def navigate_history_down(self, event):
        if self.command_history and self.history_index < len(self.command_history):
            self.history_index += 1
            if self.history_index == len(self.command_history):
                self.entry_input.delete(0, tk.END)
            else:
                self.update_input_from_history()

    def update_input_from_history(self):
        self.entry_input.delete(0, tk.END)
        self.entry_input.insert(0, self.command_history[self.history_index])


class QuadSerialApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Quad Port Auto-Connect Console")
        self.geometry("1100x700")
        
        # We need a container for the 4 consoles (top) and one for global controls (bottom)
        self.main_container = tk.Frame(self)
        self.main_container.pack(fill=tk.BOTH, expand=True)
        
        self.bottom_controls = tk.Frame(self, pady=10)
        self.bottom_controls.pack(fill=tk.X, side=tk.BOTTOM)

        # Grid Configuration (2x2) for main_container
        self.main_container.columnconfigure(0, weight=1)
        self.main_container.columnconfigure(1, weight=1)
        self.main_container.rowconfigure(0, weight=1)
        self.main_container.rowconfigure(1, weight=1)

        # Load Config
        configs = self.load_config()
        self.consoles = [] # Keep track of console instances

        positions = [(0,0), (0,1), (1,0), (1,1)]
        
        for i, pos in enumerate(positions):
            if i < len(configs):
                cfg = configs[i]
            else:
                cfg = {"name": f"Unused Slot {i+1}", "usb_serial": "", "baud": 115200}
            
            frame = AutoSerialConsole(self.main_container, config=cfg)
            frame.grid(row=pos[0], column=pos[1], sticky="nsew", padx=5, pady=5)
            self.consoles.append(frame)

        # --- Global Controls ---
        # Clear All Button
        self.btn_clear_all = tk.Button(self.bottom_controls, text="CLEAR ALL CONSOLES", command=self.clear_all, bg="#ffcccc", height=2, width=20)
        self.btn_clear_all.pack()

    def load_config(self):
        config_path = "q_term_config.json"
        if not os.path.exists(config_path):
            messagebox.showerror("Config Error", "q_term_config.json not found!\nPlease create it with your device details.")
            return []
        try:
            with open(config_path, 'r') as f:
                return json.load(f)
        except json.JSONDecodeError as e:
            messagebox.showerror("Config Error", f"Invalid JSON format:\n{e}")
            return []

    def clear_all(self):
        """Iterates through all console instances and calls their clear method."""
        for console in self.consoles:
            console.clear_console()

if __name__ == "__main__":
    app = QuadSerialApp()
    app.mainloop()