import argparse
import os
import shutil
import sys


def main():
    parser = argparse.ArgumentParser(
        description="Automatically copy fota_transtrack_task.bin from the build folder."
    )

    # Only require the destination folder argument
    parser.add_argument(
        "dst_dir", type=str, help="Path to the destination directory"
    )

    args = parser.parse_args()

    # Hardcode the relative source file path based on your project layout
    src_file_path = os.path.abspath(
        os.path.join("build", "fota_transtrack_task.bin")
    )
    dest_dir_path = os.path.abspath(args.dst_dir)
    dest_file_path = os.path.join(
        dest_dir_path, "fota_transtrack_task.bin"
    )

    # Verify build file exists
    if not os.path.isfile(src_file_path):
        print(
            f"Error: Build file not found at '{src_file_path}'. Did you compile the project?",
            file=sys.stderr,
        )
        sys.exit(1)

    # Ensure target directory exists
    os.makedirs(dest_dir_path, exist_ok=True)

    try:
        shutil.copy2(src_file_path, dest_file_path)
        print(
            f"Successfully copied firmware to: {dest_file_path}"
        )
    except Exception as e:
        print(f"Error copying file: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
