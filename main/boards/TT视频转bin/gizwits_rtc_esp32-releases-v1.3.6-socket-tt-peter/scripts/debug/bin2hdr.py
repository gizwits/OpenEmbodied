import sys
import os

def bin_to_header(bin_file, header_file):
    # Read binary file
    with open(bin_file, 'rb') as f:
        content = f.read()
    
    # Create header file
    with open(header_file, 'w') as f:
        # Write header guards
        guard = os.path.splitext(os.path.basename(header_file))[0].upper() + '_H'
        f.write(f'#ifndef {guard}\n')
        f.write(f'#define {guard}\n\n')
        
        # Write array declaration
        array_name = os.path.splitext(os.path.basename(bin_file))[0]
        f.write(f'const unsigned char {array_name}[] = {{\n    ')
        
        # Convert binary data to hex
        hex_data = [f'0x{byte:02x}' for byte in content]
        
        # Write data in rows of 16 bytes
        for i, byte in enumerate(hex_data):
            if i > 0:
                if i % 16 == 0:
                    f.write('\n    ')
                else:
                    f.write(' ')
            f.write(byte)
            if i < len(hex_data) - 1:
                f.write(',')
        
        # Write array size and closing
        f.write(f'\n}};\n\n')
        f.write(f'const unsigned int {array_name}_size = sizeof({array_name});\n\n')
        
        # Close header guard
        f.write(f'#endif // {guard}\n')
    
    print("Converting completed successfully")

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: python bin2header.py input.bin output.h")
        sys.exit(1)
    
    bin_to_header(sys.argv[1], sys.argv[2])