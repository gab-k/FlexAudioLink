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
import re
import queue
import signal

ANSI_ESCAPE_RE = re.compile(r'\x1b\[([0-9;]*)m')
ANSI_COLOR_TAGS = {
    '31': 'err',
    '32': 'rx',
    '33': 'wrn',
}

class AutoSerialConsole(tk.LabelFrame):
    def __init__(self, parent, config, log_path, *args, **kwargs):
        """
        config: dict containing "name", "usb_serial", "baud"
        log_path: path to session log file (opened in 'w' mode, overwritten each run)
        """
        self.target_name = config.get("name", "Unknown")
        self.target_serial = config.get("usb_serial", "").strip()
        self.target_baud = config.get("baud", 115200)

        super().__init__(parent, text=f" {self.target_name} (Searching...) ", padx=5, pady=5, *args, **kwargs)

        self.serial_conn = None
        self.is_connected = False
        self.stop_thread = threading.Event()
        self._disconnecting = False
        self._log_lock = threading.Lock()
        self._log_file = open(log_path, 'w', buffering=1)  # line-buffered, truncates on open
        self._ui_log_queue = queue.SimpleQueue()
        self._tx_queue = queue.Queue()
        
        # Timestamp toggle (on by default)
        self.show_timestamp = True

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
        self.console_text.tag_config('wrn', foreground='#ffff55', font=("Consolas", 9, "bold"))
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

        self.btn_clear_log = tk.Button(input_frame, text="Clear Log", command=self.clear_log_file, width=8)
        self.btn_clear_log.pack(side=tk.RIGHT, padx=5)

        # Timestamp Toggle Button
        self.btn_ts = tk.Button(input_frame, text="TS", command=self.toggle_timestamp, width=3, relief=tk.SUNKEN)
        self.btn_ts.pack(side=tk.RIGHT)

        # Start the Auto-Connect Watchdog
        self.after(1000, self.auto_connect_watchdog)
        self.after(50, self.flush_ui_log_queue)

    def toggle_timestamp(self):
        self.show_timestamp = not self.show_timestamp
        self.btn_ts.config(relief=tk.SUNKEN if self.show_timestamp else tk.RAISED)

    def log(self, message, tag='sys'):
        display_tag = tag
        for codes in ANSI_ESCAPE_RE.findall(message):
            for code in codes.split(';'):
                display_tag = ANSI_COLOR_TAGS.get(code, display_tag)
        message = ANSI_ESCAPE_RE.sub('', message)
        timestamp = datetime.datetime.now().strftime("%H:%M:%S")
        with self._log_lock:
            try:
                self._log_file.write(f"[{timestamp}] [{tag.upper()}] {message}\n")
            except Exception:
                pass

        self._ui_log_queue.put((timestamp, message, display_tag))

    def flush_ui_log_queue(self):
        pending = []

        while len(pending) < 200:
            try:
                pending.append(self._ui_log_queue.get_nowait())
            except queue.Empty:
                break

        if pending:
            self.console_text.config(state='normal')
            for timestamp, message, tag in pending:
                prefix = f"[{timestamp}] " if self.show_timestamp else ""
                self.console_text.insert(tk.END, f"{prefix}{message}\n", tag)
            self.console_text.see(tk.END)
            self.console_text.config(state='disabled')

        self.after(50, self.flush_ui_log_queue)

    def clear_console(self):
        """Clears the text area for this specific console."""
        self.console_text.config(state='normal')
        self.console_text.delete(1.0, tk.END)
        self.console_text.config(state='disabled')

    def clear_log_file(self):
        with self._log_lock:
            try:
                self._log_file.seek(0)
                self._log_file.truncate()
                self._log_file.flush()
            except Exception:
                pass

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
        matches = []
        for port in ports:
            if port.serial_number and port.serial_number.strip().upper() == target_serial.strip().upper():
                matches.append(port.device)

        if matches:
            # Pick the highest-numbered port — nRF54 exposes two UARTs per Segger,
            # and the application UART is the higher one.
            matches.sort(key=lambda p: int(''.join(filter(str.isdigit, p)) or '0'))
            return matches[-1]

        # Linux fallback: pyserial sometimes doesn't populate serial_number right after
        # re-enumeration, but /dev/serial/by-id/ symlinks (which encode the serial in the
        # filename) are usually available sooner.
        by_id_dir = '/dev/serial/by-id'
        if os.path.isdir(by_id_dir):
            target_upper = target_serial.strip().upper()
            fallback_matches = []
            for link_name in os.listdir(by_id_dir):
                if target_upper in link_name.upper():
                    fallback_matches.append(os.path.realpath(os.path.join(by_id_dir, link_name)))
            if fallback_matches:
                fallback_matches.sort(key=lambda p: int(''.join(filter(str.isdigit, p)) or '0'))
                return fallback_matches[-1]

        return None

    def connect(self, port_name):
        try:
            self.serial_conn = serial.Serial(
                port_name,
                baudrate=self.target_baud,
                timeout=0.05,
                write_timeout=0.05,
            )
            self.is_connected = True
            self._disconnecting = False
            
            self.stop_thread.clear()
            self.btn_send.config(state="normal")
            self.lbl_status.config(text=f"Connected: {port_name}", fg="green")
            self.config(text=f" {self.target_name} (ONLINE) ", fg="green")
            
            self.log(f"Auto-connected to {port_name}", 'sys')

            threading.Thread(target=self.read_loop, daemon=True).start()
            threading.Thread(target=self.write_loop, daemon=True).start()
            
        except serial.SerialException as e:
            self.log(f"Connection Failed: {e}", 'err')

    def disconnect(self):
        if self._disconnecting:
            return

        self._disconnecting = True
        self.stop_thread.set()
        serial_conn = self.serial_conn
        self.serial_conn = None
        while True:
            try:
                self._tx_queue.get_nowait()
            except queue.Empty:
                break
        
        self.is_connected = False
        self.btn_send.config(state="disabled")
        self.lbl_status.config(text="Disconnected", fg="red")
        self.log("Device Disconnected.", 'err')
        self.config(text=f" {self.target_name} (Searching...) ", fg="black")

        if serial_conn is not None:
            threading.Thread(target=self._close_serial, args=(serial_conn,), daemon=True).start()
        else:
            self._disconnecting = False

    def _close_serial(self, serial_conn):
        try:
            if hasattr(serial_conn, 'cancel_read'):
                serial_conn.cancel_read()
            if hasattr(serial_conn, 'cancel_write'):
                serial_conn.cancel_write()
        except Exception:
            pass

        try:
            if serial_conn.is_open:
                serial_conn.close()
        except Exception:
            pass
        finally:
            self._disconnecting = False

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
                            if clean: self.log(f"{clean}", 'rx')
                        rx_buffer = lines[-1]
                else:
                    if rx_buffer and (time.time() - last_rx_time > BUFFER_TIMEOUT):
                        self.log(f"{rx_buffer.strip()}", 'rx')
                        rx_buffer = ""
                    time.sleep(0.01)

            except (serial.SerialException, OSError):
                self.stop_thread.set()
                self.after(0, self.disconnect)
                break

    def write_loop(self):
        while not self.stop_thread.is_set():
            try:
                payload = self._tx_queue.get(timeout=0.05)
            except queue.Empty:
                continue

            if payload is None:
                continue

            try:
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.write(payload)
            except (serial.SerialException, serial.SerialTimeoutException, OSError) as e:
                self.log(f"Send Error: {e}", 'err')
                self.stop_thread.set()
                self.after(0, self.disconnect)
                break

    def send_data(self):
        if self.is_connected and self.serial_conn:
            data = self.entry_input.get()
            if data:
                try:
                    self.log(f"TX: {data}", 'tx')
                    self._tx_queue.put_nowait((data + "\n").encode('utf-8'))
                    
                    if not self.command_history or self.command_history[-1] != data:
                        self.command_history.append(data)
                    self.history_index = len(self.command_history)
                    
                    self.entry_input.delete(0, tk.END)
                except queue.Full:
                    self.log("Send Error: TX queue full", 'err')
                except (serial.SerialException, serial.SerialTimeoutException, OSError) as e:
                    self.log(f"Send Error: {e}", 'err')
                    self.after(0, self.disconnect)
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

        # Prepare session log directory (../temp relative to this script)
        script_dir = os.path.dirname(os.path.abspath(__file__))
        self.log_dir = os.path.join(script_dir, '..', 'temp')
        os.makedirs(self.log_dir, exist_ok=True)

        # Load Config
        configs = self.load_config()
        self.consoles = [] # Keep track of console instances

        positions = [(0,0), (0,1), (1,0), (1,1)]

        for i, pos in enumerate(positions):
            if i < len(configs):
                cfg = configs[i]
            else:
                cfg = {"name": f"Unused Slot {i+1}", "usb_serial": "", "baud": 115200}

            safe_name = cfg.get("name", f"terminal_{i}").replace(' ', '_').replace('/', '_')
            log_path = os.path.join(self.log_dir, f"session_{safe_name}.log")

            frame = AutoSerialConsole(self.main_container, config=cfg, log_path=log_path)
            frame.grid(row=pos[0], column=pos[1], sticky="nsew", padx=5, pady=5)
            self.consoles.append(frame)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # --- Global Controls ---
        # Clear All Button
        self.btn_clear_all = tk.Button(self.bottom_controls, text="CLEAR ALL CONSOLES", command=self.clear_all, bg="#ffcccc", height=2, width=20)
        self.btn_clear_all.pack(side=tk.LEFT, padx=5)

        self.btn_clear_logs = tk.Button(
            self.bottom_controls,
            text="CLEAR SAVED LOGS",
            command=self.clear_saved_logs,
            bg="#ffe4b3",
            height=2,
            width=18,
        )
        self.btn_clear_logs.pack(side=tk.LEFT, padx=5)

        self._closing = False

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

    def clear_saved_logs(self):
        cleared = 0
        removed = 0

        for console in self.consoles:
            console.clear_log_file()
            cleared += 1

        for name in os.listdir(self.log_dir):
            if not (name.startswith("session_") and name.endswith(".log")):
                continue

            path = os.path.join(self.log_dir, name)
            if any(os.path.abspath(path) == os.path.abspath(console._log_file.name)
                   for console in self.consoles):
                continue

            try:
                os.remove(path)
                removed += 1
            except OSError:
                pass


    def _on_close(self):
        if self._closing:
            return

        self._closing = True
        for console in self.consoles:
            try:
                console.stop_thread.set()
                if console.serial_conn and console.serial_conn.is_open:
                    console.serial_conn.close()
            except Exception:
                pass
            try:
                console._log_file.close()
            except Exception:
                pass
        self.destroy()

if __name__ == "__main__":
    app = QuadSerialApp()
    def handle_sigint(signum, frame):
        try:
            app.after(0, app._on_close)
        except Exception:
            pass

    signal.signal(signal.SIGINT, handle_sigint)

    try:
        app.mainloop()
    except KeyboardInterrupt:
        app._on_close()
