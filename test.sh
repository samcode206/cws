#!/bin/sh


suite_dir="./test/suite"

set -e

for file in "$suite_dir"/*.c; do
    base_name=$(basename "$file" .c)
    gcc "$file" -lz  -O3 -march=native -mtune=native -o "$suite_dir/$base_name"

    if [ $? -eq 0 ]; then
        "$suite_dir/$base_name"
        rm "$suite_dir/$base_name"
    else
        echo "Compilation of $file failed."
    fi
done
