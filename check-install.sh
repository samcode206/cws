 #!/bin/sh


LIB_PATH="/usr/local/lib"
INCLUDE_PATH="/usr/local/include"


exit_with_error() {
    echo "$1"
    exit 1
}

case "$OSTYPE" in
    darwin*)
        LIB_EXT=".dylib"
        ;;
    *)
        LIB_EXT=".so"
        ;;
esac


if [ -f "$LIB_PATH/libws$LIB_EXT" ]; then
    echo "libws$LIB_EXT found in $LIB_PATH"
else
    exit_with_error "ERROR: libws$LIB_EXT not found in $LIB_PATH"
fi


if [ -f "$LIB_PATH/libws.a" ]; then
    echo "libws.a found in $LIB_PATH"
else
    exit_with_error "ERROR: libws.a not found in $LIB_PATH"
fi


if [ -f "$INCLUDE_PATH/ws.h" ]; then
    echo "ws.h found in $INCLUDE_PATH"
else
    exit_with_error "ERROR: ws.h not found in $INCLUDE_PATH"
fi


echo "SUCCESS: library installed"

exit 0
