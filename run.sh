#!/bin/bash
#
# run.sh
#
# Script di compilazione e avvio per il progetto "Esecuzione Remota".
#
# Utilizzo:
#   ./run.sh build          compila server e client
#   ./run.sh server         compila (se necessario) e avvia il server
#   ./run.sh client          compila (se necessario) e avvia un client
#   ./run.sh clean           rimuove i file compilati
#
# Se invocato senza argomenti, equivale a "build".

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
BIN_DIR="$SCRIPT_DIR/bin"

CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -g -std=c11 -D_POSIX_C_SOURCE=200809L"

build() {
    mkdir -p "$BIN_DIR"
    echo "Compilazione del server..."
    "$CC" $CFLAGS -o "$BIN_DIR/server" "$SRC_DIR/server.c"
    echo "Compilazione del client..."
    "$CC" $CFLAGS -o "$BIN_DIR/client" "$SRC_DIR/client.c"
    echo "Compilazione completata. Eseguibili in: $BIN_DIR"
}

clean() {
    rm -rf "$BIN_DIR"
    rm -f /tmp/esecuzione_remota.sock
    echo "Rimossi i file compilati e il socket residuo."
}

run_server() {
    if [ ! -x "$BIN_DIR/server" ]; then
        build
    fi
    echo "Avvio del server (Ctrl-C e Ctrl-\\ sono ignorati; usare"
    echo "'kill -TERM <pid>' per terminare l'applicazione)..."
    exec "$BIN_DIR/server"
}

run_client() {
    if [ ! -x "$BIN_DIR/client" ]; then
        build
    fi
    exec "$BIN_DIR/client"
}

case "${1:-build}" in
    build)
        build
        ;;
    server)
        run_server
        ;;
    client)
        run_client
        ;;
    clean)
        clean
        ;;
    *)
        echo "Utilizzo: $0 {build|server|client|clean}" >&2
        exit 1
        ;;
esac
