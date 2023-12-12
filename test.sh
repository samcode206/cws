#!/bin/bash


suite_dir="./test/suite"


for file in "$suite_dir"/*.c; do
    base_name=$(basename "$file" .c)
    gcc "$file" -lz -lcrypto -O3 -march=native -mtune=native -Wall --pedantic -o "$suite_dir/$base_name"

    if [ $? -eq 0 ]; then
        "$suite_dir/$base_name"
        rm "$suite_dir/$base_name"
    else
        echo "Compilation of $file failed."
    fi
done
