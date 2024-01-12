#!/bin/sh


suite_dir="./test/e2e"

set -e

for file in "$suite_dir"/*.c; do
    base_name=$(basename "$file" .c)
    echo "Compiling E2E..."
    echo "cc ./src/*.c "$file" -O3 -march=native -mtune=native $1 -o "$suite_dir/$base_name""
    cc -std=c11 ./src/*.c "$file" $1 -O3 -march=native -mtune=native -o "$suite_dir/$base_name"

    if [ $? -eq 0 ]; then
        echo "running $suite_dir/$base_name"
        "$suite_dir/$base_name"
        rm "$suite_dir/$base_name"
        echo "done running $suite_dir/$base_name"
    else
        echo "Compilation of $file failed."
    fi
done
