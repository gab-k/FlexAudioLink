# Script for calculating Average of absolute values of errors reported by audio feedback task

import re
import sys

def calculate_average_abs_error(filepath):
    try:
        with open(filepath, 'r') as f:
            content = f.read()
        
        # Find all numbers following "E:" (handles negative signs and spacing)
        # Regex explanation:
        # E:   -> matches the literal characters "E:"
        # \s* -> matches zero or more whitespace characters
        # (-?\d+) -> captures the number (optional negative sign + digits)
        e_values = re.findall(r"E:\s*(-?\d+)", content)
        
        if not e_values:
            print(f"No 'E:' values found in {filepath}")
            return

        # Convert strings to integers, take absolute value
        abs_values = [abs(int(val)) for val in e_values]
        
        # Calculate average
        average = sum(abs_values) / len(abs_values)
        
        print(f"File: {filepath}")
        print(f"  Count: {len(abs_values)}")
        print(f"  Average of Absolute Values: {average:.2f}")
        print("-" * 30)

    except FileNotFoundError:
        print(f"Error: File '{filepath}' not found.")
    except Exception as e:
        print(f"An error occurred processing {filepath}: {e}")

if __name__ == "__main__":
    # You can list your specific files here
    files_to_process = [
        "50ms_0p1alpha.yaml", 
        "100ms_0p1alpha.yaml", 
        "100ms_0p05alpha.yaml", 
        "200ms_0p1alpha.yaml",
	"200ms_0p05alpha.yaml"
    ]

    # Or uncomment the line below to run on files passed via command line
    # files_to_process = sys.argv[1:]

    if not files_to_process:
        print("Please provide files to process.")
    else:
        for file_name in files_to_process:
            calculate_average_abs_error(file_name)