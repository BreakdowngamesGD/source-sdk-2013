from pathlib import Path
import shutil

# 1. Configure your folders and target filenames
ROOT_DIR = Path(__file__).parent.resolve()
TARGET_SUBDIR = ROOT_DIR / "src"  # <-- Change this

# Put the exact .cpp and .h filenames you want to copy here
TARGET_FILES = {
    "player.h",
    "player.cpp",
    "hl2_player.h",
    "hl2_player.cpp",
    "ez2_player.h",
    "ez2_player.cpp",
    "c_hl2_player.cpp",
    "c_hl2_player.cpp",
    "server_ez2.vpc",
    "client_ez2.vpc",
    "func_tank.h",
    "func_tank.cpp",
    "vehicle_base.h",
    "vehicle_base.cpp",
}


def export_targeted_files():
    if not TARGET_SUBDIR.exists() or not TARGET_SUBDIR.is_dir():
        print(f"Error: Subdirectory '{TARGET_SUBDIR}' not found.")
        return

    # Normalize target names to lowercase for reliable matching on Windows
    target_names_lower = {name.lower() for name in TARGET_FILES}
    copied_count = 0

    # 2. Recursively search the subdirectory and all child folders
    for file_path in TARGET_SUBDIR.rglob("*"):
        if file_path.is_file() and file_path.name.lower() in target_names_lower:

            # Create the new .txt filename (e.g., player.cpp -> player.cpp.txt)
            new_filename = f"{file_path.name}.txt"
            dest_path = ROOT_DIR / new_filename

            # 3. Prevent overwriting if multiple subfolders contain a matching filename
            counter = 1
            while dest_path.exists():
                dest_path = ROOT_DIR / f"{file_path.name}_{counter}.txt"
                counter += 1

            # 4. Copy the file to the root folder
            shutil.copy2(file_path, dest_path)
            print(f"Copied: {file_path.relative_to(ROOT_DIR)} -> {dest_path.name}")
            copied_count += 1

    print(f"\nDone! Exported {copied_count} files to {ROOT_DIR}")


if __name__ == "__main__":
    export_targeted_files()
