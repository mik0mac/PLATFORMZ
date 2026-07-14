import soundfile as sf
import argparse
import os
from pathlib import Path

AUDIO_FILE_EXT = ('.wav', '.mp3', '.aif', '.aiff')

BLOCK_FRAMES = 65536  # libsndfile's vorbis encoder segfaults on huge single writes

def convert_to_ogg(input_file, output_file, compression_level=0.4):
    with sf.SoundFile(input_file) as infile:
        with sf.SoundFile(output_file, 'w', samplerate=infile.samplerate,
                          channels=infile.channels, format='OGG',
                          compression_level=compression_level) as outfile:
            for block in infile.blocks(blocksize=BLOCK_FRAMES):
                outfile.write(block)

def main(path, compression):
    if compression < 0 or compression > 1:
        print('Compression must be a value between 0.0 and 1.0')
        return

    orig_files = []
    if os.path.isdir(path):
        for file in os.listdir(path):
            if file.lower().endswith(AUDIO_FILE_EXT):
                orig_files.append(os.path.join(path, file))
    else:
        if os.path.exists(path) and path.lower().endswith(AUDIO_FILE_EXT):
            orig_files.append(path)

    if orig_files == []:
        print('No audio files found.')
        return

    try:
        print('new files created:')

        for file in orig_files:
            output_path = Path(file).with_suffix('.ogg')
            convert_to_ogg(file, output_path, compression)
            print(output_path.name)
    except Exception as e:
        print(f'Error occurred: {e}')

    

if __name__ == "__main__":
    # 1. Initialize Parser
    parser = argparse.ArgumentParser(description="Process arguments.")

    # 2. Define Arguments
    parser.add_argument("path", help="File or folder to process") # Positional
    parser.add_argument("-c", "--compression", type=float, default=0.4, help="Compression level. 0.0 = highest quality/least compressed, 1.0 = most compressed.")
    
    # 3. Parse and Access
    args = parser.parse_args()
    print(f"Path: {args.path}, Compression: {args.compression}")

    main(args.path, args.compression)

    