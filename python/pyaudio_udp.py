# Setup audio devices via pyaudiowpatch and optionally record a test .wav file.
# Initializes a UDP client to stream data to a target IP.
# Note: The UDP loop is currently configured to send generated test integers
# (for network debugging) rather than the live audio stream.

import pyaudiowpatch
import wave
import sys
import os
import socket
import time


def setup() -> int:
    p = pyaudiowpatch.PyAudio()
    p.print_detailed_system_info()
    p.terminate()
    index = int(input("\nSelect device index: "))
    test_record = input("Test recording device? (y/N) ")
    match test_record:
        case "Y":
            test_recording(index)
        case "y":
            test_recording(index)
        case _:
            return index
    
    
def test_recording(dev_index: int):
    """PyAudio Example: Record a few seconds of audio and save to a wave file."""
    p = pyaudiowpatch.PyAudio()
    
    dev_info = p.get_device_info_by_index(dev_index)
    data_format = pyaudiowpatch.paInt16
    channels = dev_info["maxInputChannels"]
    sample_rate = int(dev_info["defaultSampleRate"])
    chunk = 1024
    rec_seconds = 5
    filename = 'test_recording.wav'

    with wave.open(filename, 'wb') as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(p.get_sample_size(data_format))
        wf.setframerate(sample_rate)
        stream = p.open(input_device_index=dev_index, format=data_format, channels=channels, rate=sample_rate, input=True)
        print('Recording for ' + str(rec_seconds) + ' seconds...')
        for _ in range(0, sample_rate // chunk * rec_seconds):
            wf.writeframes(stream.read(chunk))
        print('Test recording saved as ' + os.getcwd() + filename)
        stream.close()
        p.terminate()
    print("exiting now...")
    sys.exit()

def udp_client(dev_index: int):
    # Create socket
    server_address = ('192.168.0.33', 3333)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    # Set up PyAudio
    p = pyaudiowpatch.PyAudio()
    dev_info = p.get_device_info_by_index(dev_index)
    data_format = pyaudiowpatch.paInt16
    channels = dev_info["maxInputChannels"]
    sample_rate = int(dev_info["defaultSampleRate"])
    chunk = 128
    stream = p.open(input_device_index=dev_index, format=data_format, channels=channels, rate=sample_rate, input=True)

    try:
        # Send audio data over UDP
        i = 0
        test_data = []
        for j in range(500):
            test_data.append(i)
        test_data = bytearray(test_data)
        while True:
            #Test data...
            for k in range(500):
                test_data[k] = i
            sock.sendto(test_data, server_address)
            i = (i+1)%256
            time.sleep(0.002)
            #audio_data = stream.read(chunk)
            #sock.sendto(audio_data, server_address)

    except KeyboardInterrupt:
        # Close the socket and PyAudio stream when interrupted by Ctrl+C
        sock.close()
        stream.stop_stream()
        stream.close()
        p.terminate()

####################
####################
dev_index = setup()
udp_client(dev_index)
