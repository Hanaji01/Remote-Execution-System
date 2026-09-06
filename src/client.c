/*
 * client.c
 *
 * Processo CLIENT dell'applicazione "Esecuzione Remota".
 *
 * Compiti:
 *   - apre una connessione col server tramite il socket Unix
 *     condiviso
 *   - in ciclo:
 *       1. chiede all'utente un comando da eseguire in remoto
 *       2. lo invia al corrispondente esecutore
 *       3. attende l'output del comando e lo visualizza
 *       4. ripete dal punto 1
 *   - se l'utente inserisce "exit", invia la stringa all'esecutore
 *     e termina
 *   - se rileva che l'esecutore ha chiuso la connessione (per
 *     esempio in risposta a un comando), termina
 *
 * Nota: la connessione col server avviene sullo stesso socket Unix
 * su cui il server e' in ascolto; dopo la accept() lato server e la
 * fork() dell'esecutore, questo stesso file descriptor (lato
 * client) resta connesso direttamente all'esecutore. Dal punto di
 * vista del client non cambia nulla: continua semplicemente a
 * leggere/scrivere sullo stesso fd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "common.h"

/*
 * Installa i gestori dei segnali richiesti dalle specifiche per il
 * client: SIGINT e SIGQUIT vengono ignorati.
 */
static void setup_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction(SIGINT)");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGQUIT, &sa, NULL) == -1) {
        perror("sigaction(SIGQUIT)");
        exit(EXIT_FAILURE);
    }

    /* SIGPIPE viene ignorato: se l'esecutore ha chiuso la
     * connessione, una successiva write() deve fallire con
     * errno == EPIPE (gestito da send_command), invece di
     * terminare il processo con il segnale predefinito. */
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        exit(EXIT_FAILURE);
    }
}

/*
 * Si connette al server tramite il socket Unix in SOCKET_PATH.
 * Restituisce il file descriptor connesso, oppure termina il
 * programma in caso di errore.
 */
static int connect_to_server(void) {
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("connect");
        fprintf(stderr, "Impossibile connettersi al server. "
                        "E' in esecuzione?\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

/*
 * Legge esattamente 'len' byte da 'fd' in 'buf'.
 * Restituisce:
 *    1  successo (letti tutti i 'len' byte)
 *    0  connessione chiusa dal peer prima di completare la lettura
 *   -1  errore di lettura
 */
static int read_full(int fd, void *buf, size_t len) {
    char *p = (char *) buf;
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        got += (size_t) n;
    }
    return 1;
}

/*
 * Riceve dall'esecutore l'intera risposta a un comando, seguendo il
 * protocollo a chunk descritto in common.h, e la stampa su stdout
 * non appena ricevuta (cosi' anche output molto lunghi vengono
 * mostrati in modo incrementale).
 *
 * Restituisce:
 *    1  risposta completa ricevuta con successo
 *    0  l'esecutore ha chiuso la connessione (l'applicazione si
 *       sta chiudendo, oppure e' stato inviato "exit")
 *   -1  errore di comunicazione
 */
static int receive_output(int fd) {
    for (;;) {
        uint32_t net_len;
        uint32_t len;
        int rc;

        rc = read_full(fd, &net_len, sizeof(net_len));
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            perror("read");
            return -1;
        }

        len = ntohl(net_len);
        if (len == 0) {
            /* Chunk di lunghezza 0: fine dell'output */
            return 1;
        }

        {
            char buf[OUT_CHUNK_LEN];
            uint32_t remaining = len;

            while (remaining > 0) {
                size_t to_read = remaining < sizeof(buf)
                                      ? remaining : sizeof(buf);

                rc = read_full(fd, buf, to_read);
                if (rc == 0) {
                    return 0;
                }
                if (rc < 0) {
                    perror("read");
                    return -1;
                }

                if (fwrite(buf, 1, to_read, stdout) != to_read) {
                    perror("fwrite");
                    return -1;
                }

                remaining -= (uint32_t) to_read;
            }
        }
    }
}

/*
 * Invia il comando 'cmd' (stringa terminata da '\0') al server,
 * incluso il byte di terminazione.
 *
 * Restituisce 0 in caso di successo, -1 in caso di errore.
 */
static int send_command(int fd, const char *cmd) {
    size_t len = strlen(cmd) + 1; /* incluso '\0' */
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, cmd + sent, len - sent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t) n;
    }
    return 0;
}

int main(void) {
    int fd;
    char line[CMD_MAX_LEN];

    setup_signal_handlers();

    fd = connect_to_server();

    fprintf(stderr, "[client] connesso al server (pid %d)\n",
            (int) getpid());
    printf("Connessione stabilita. Digita un comando da eseguire in "
           "remoto (o 'exit' per uscire).\n");

    for (;;) {
        size_t len;

        printf("> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF su stdin (es. Ctrl-D): si comporta come 'exit' */
            fprintf(stderr, "\n[client] EOF su stdin: invio 'exit'\n");
            strncpy(line, EXIT_CMD, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        } else {
            /* Rimuove il newline finale, se presente */
            len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
        }

        if (send_command(fd, line) == -1) {
            fprintf(stderr, "[client] errore durante l'invio del "
                            "comando, oppure l'esecutore ha chiuso "
                            "la connessione\n");
            break;
        }

        if (strcmp(line, EXIT_CMD) == 0) {
            fprintf(stderr, "[client] comando 'exit' inviato: "
                            "termino\n");
            break;
        }

        {
            int rc = receive_output(fd);
            if (rc == 0) {
                fprintf(stderr, "\n[client] l'esecutore ha chiuso la "
                                "connessione: termino\n");
                break;
            }
            if (rc < 0) {
                fprintf(stderr, "[client] errore di comunicazione: "
                                "termino\n");
                break;
            }
        }
    }

    close(fd);
    return EXIT_SUCCESS;
}
