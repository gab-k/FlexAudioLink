import numpy as np
import matplotlib.pyplot as plt
import struct

# --- Configuration ---
SAMPLE_RATE      = 48000
NUM_SAMPLES      = 336       # The number of stereo sample frames (L/R pairs)
FREQUENCY_LEFT   = 142.8571
FREQUENCY_RIGHT  = 1428.571
AMPLITUDE        = 32767.0   # Max amplitude for a 16-bit signed integer
OUTPUT_FILENAME  = "generated_sine.pcm"

# 1. Generate the Time Vector
# This creates a time axis from 0 to (NUM_SAMPLES-1)/SAMPLE_RATE
t = np.arange(NUM_SAMPLES) / SAMPLE_RATE

# 2. Generate the Sine Waves as Floating-Point Numbers
# This is the pure mathematical representation of the waves from -1.0 to 1.0
left_wave_float = np.sin(2 * np.pi * FREQUENCY_LEFT * t)
right_wave_float = np.sin(2 * np.pi * FREQUENCY_RIGHT * t)

# 3. Scale and Convert to 16-bit Signed Integers
# This converts the waves to the format needed for PCM audio
left_samples = (left_wave_float * AMPLITUDE).astype(np.int16)
right_samples = (right_wave_float * AMPLITUDE).astype(np.int16)

# 4. Interleave the Left and Right Channels
# The final array must be in the format [L0, R0, L1, R1, L2, R2, ...]
pcm_data_int16 = np.empty(NUM_SAMPLES * 2, dtype=np.int16)
pcm_data_int16[0::2] = left_samples  # Assign to even indices
pcm_data_int16[1::2] = right_samples # Assign to odd indices

# 5. --- THIS IS THE CRITICAL STEP ---
# Convert the int16 array into a raw byte stream (uint8_t equivalent)
# This mimics exactly what the linker script does with _binary_..._start
# '<h' means little-endian signed short (16-bit)
byte_data = b''
for sample in pcm_data_int16:
    byte_data += struct.pack('<h', sample)

# Optional: Save the raw PCM file to disk to test with a media player
# You can play this with: ffplay -f s16le -ar 48000 -ac 2 generated_sine.pcm
with open(OUTPUT_FILENAME, 'wb') as f:
    f.write(byte_data)
print(f"Successfully saved raw PCM data to '{OUTPUT_FILENAME}'")

# 6. Print the C Array
# This formats the byte stream into a C-compatible uint8_t array
print("\n--- Copy and paste the C array below into your project ---")
print(f"// {NUM_SAMPLES} samples of a {FREQUENCY_LEFT:.0f}Hz (L) and {FREQUENCY_RIGHT:.0f}Hz (R) sine wave")
print("// Sample Rate: {SAMPLE_RATE}Hz, Format: 16-bit Little-Endian Stereo")
print("static const uint8_t sine_wave_pcm[] = {")

line = "    "
for i, byte in enumerate(byte_data):
    line += f"0x{byte:02x}, "
    if (i + 1) % 12 == 0: # Newline every 12 bytes for readability
        print(line)
        line = "    "
if line.strip() != "":
    print(line)
print("};")


# 7. Plotting for Verification
fig, axs = plt.subplots(2, 1, sharex=True, figsize=(12, 6))
fig.suptitle(f'Generated PCM Sine Waves (First {NUM_SAMPLES} Samples)', fontsize=16)

# Plot Left Channel
axs[0].plot(t, left_samples, '.-', label=f'{FREQUENCY_LEFT:.0f} Hz')
axs[0].set_title('Left Channel')
axs[0].set_ylabel('Amplitude')
axs[0].grid(True)
axs[0].legend()

# Plot Right Channel
axs[1].plot(t, right_samples, '.-', color='orange', label=f'{FREQUENCY_RIGHT:.0f} Hz')
axs[1].set_title('Right Channel')
axs[1].set_xlabel('Time (seconds)')
axs[1].set_ylabel('Amplitude')
axs[1].grid(True)
axs[1].legend()

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.show()