/*
 * server.c
 *
 * Processo SERVER dell'applicazione "Esecuzione Remota".
 *
 * Compiti:
 *   - crea un socket Unix (stream) e resta in ascolto di nuove
 *     connessioni dai client
 *   - per ogni client connesso crea, tramite fork(2), un processo
 *     ESECUTORE che gestira' da quel momento in poi tutta la
 *     comunicazione con quel client
 *   - tiene traccia dei PID degli esecutori attivi
 *   - alla ricezione di SIGTERM:
 *       . smette di accettare nuove connessioni
 *       . avvisa tutti gli esecutori ancora attivi (SIGUSR1) che
 *         l'applicazione si sta chiudendo
 *       . attende (wait) la terminazione di tutti gli esecutori
 *       . chiude il socket di ascolto, rimuove il file di socket
 *         e termina
 *
 * Note IPC:
 *   Si utilizza un socket di tipo AF_UNIX/SOCK_STREAM: e' uno degli
 *   strumenti di IPC ammessi dalla specifica (sockets), e non
 *   comporta scambio di dati tramite file ordinari su disco (il
 *   file di socket e' un semplice "rendez-vous point", non viene
 *   usato per leggere/scrivere dati applicativi).
 *
 * Note su attese:
 *   - accept() e' bloccante (attesa passiva, nessun busy loop)
 *   - waitpid() e' usata in modalita' bloccante per attendere la
 *     terminazione degli esecutori, sia durante il funzionamento
 *     normale (per "raccogliere" gli esecutori che terminano da
 *     soli) sia durante la terminazione del server.
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
#include <sys/wait.h>
#include <arpa/inet.h>

#include "common.h"

/* Numero massimo di esecutori attivi gestibili contemporaneamente.
 * Limite semplice basato su array statico, sufficiente per un
 * progetto didattico. */
#define MAX_EXECUTORS 256

/* Array dei PID degli esecutori attualmente attivi.
 * Un valore 0 indica uno slot libero. */
static pid_t executors[MAX_EXECUTORS];
static int n_executors = 0;

/* Flag impostato dall'handler di SIGTERM. volatile sig_atomic_t
 * e' il tipo corretto per variabili modificate in un signal
 * handler e lette nel programma principale. */
static volatile sig_atomic_t terminating = 0;

/* File descriptor del socket di ascolto. */
static int listen_fd = -1;

/*
 * Handler di SIGTERM: si limita a impostare il flag globale.
 * Tutta la logica di terminazione viene eseguita nel main loop,
 * fuori dal contesto del signal handler (best practice: gli
 * handler devono fare il minimo indispensabile).
 */
static void sigterm_handler(int signo) {
    (void) signo;
    terminating = 1;
}

/*
 * Aggiunge il pid di un nuovo esecutore all'elenco di quelli attivi.
 */
static void add_executor(pid_t pid) {
    int i;
    for (i = 0; i < MAX_EXECUTORS; i++) {
        if (executors[i] == 0) {
            executors[i] = pid;
            n_executors++;
            return;
        }
    }
    /* Se l'array e' pieno semplicemente non lo si traccia: caso
     * limite non rilevante per gli scopi del progetto. */
    fprintf(stderr, "[server] attenzione: troppi esecutori attivi, "
                     "pid %d non tracciato\n", (int) pid);
}

/*
 * Rimuove il pid di un esecutore terminato dall'elenco di quelli attivi.
 */
static void remove_executor(pid_t pid) {
    int i;
    for (i = 0; i < MAX_EXECUTORS; i++) {
        if (executors[i] == pid) {
            executors[i] = 0;
            n_executors--;
            return;
        }
    }
}

/*
 * Raccoglie (in modo non bloccante) tutti gli esecutori che sono
 * eventualmente terminati nel frattempo, per evitare l'accumulo
 * di processi zombie. Viene chiamata periodicamente dal ciclo
 * principale, dopo ogni accept().
 */
static void reap_finished_executors(void) {
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        remove_executor(pid);
        if (WIFEXITED(status)) {
            fprintf(stderr, "[server] esecutore pid %d terminato "
                            "(exit status %d)\n",
                    (int) pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[server] esecutore pid %d terminato "
                            "da segnale %d\n",
                    (int) pid, WTERMSIG(status));
        }
    }
}

/*
 * Invia SIGUSR1 a tutti gli esecutori ancora attivi, per
 * informarli che l'applicazione si sta chiudendo e che quindi
 * devono terminare. Poi attende (in modo bloccante) la
 * terminazione di ciascuno di essi tramite wait().
 */
static void shutdown_executors(void) {
    int i;

    fprintf(stderr, "[server] invio richiesta di terminazione "
                    "a %d esecutore(i) attivo(i)...\n", n_executors);

    for (i = 0; i < MAX_EXECUTORS; i++) {
        if (executors[i] != 0) {
            if (kill(executors[i], SIGUSR1) == -1 && errno != ESRCH) {
                perror("[server] kill(SIGUSR1)");
            }
        }
    }

    /* Attesa bloccante di tutti i figli rimasti. wait() restituisce
     * -1 con errno == ECHILD quando non ci sono piu' figli. */
    for (;;) {
        pid_t pid;
        int status;

        pid = wait(&status);
        if (pid == -1) {
            if (errno == ECHILD) {
                break; /* nessun figlio rimasto: fine */
            }
            if (errno == EINTR) {
                continue;
            }
            perror("[server] wait");
            break;
        }
        remove_executor(pid);
        fprintf(stderr, "[server] esecutore pid %d terminato "
                        "(durante shutdown)\n", (int) pid);
    }

    fprintf(stderr, "[server] tutti gli esecutori sono terminati.\n");
}

/*
 * Crea, configura e mette in ascolto il socket Unix del server.
 * Restituisce il file descriptor del socket di ascolto, oppure
 * termina il programma in caso di errore irreversibile.
 */
static int create_listening_socket(void) {
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Rimuove un eventuale file di socket residuo da una
     * esecuzione precedente terminata in modo anomalo. */
    unlink(SOCKET_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        perror("bind");
        close(fd);
        exit(EXIT_FAILURE);
    }

    if (listen(fd, SOMAXCONN) == -1) {
        perror("listen");
        close(fd);
        exit(EXIT_FAILURE);
    }

    return fd;
}

/*
 * Installa i gestori dei segnali richiesti dalle specifiche per il
 * processo server:
 *  - SIGINT e SIGQUIT vengono ignorati
 *  - SIGTERM viene gestito da sigterm_handler (per la terminazione
 *    ordinata dell'applicazione)
 */
static void setup_signal_handlers(void) {
    struct sigaction sa;

    /* SIGINT e SIGQUIT ignorati */
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

    /* SIGPIPE ignorato: una write() su una connessione chiusa dal
     * peer deve fallire con errno == EPIPE (gestito controllando i
     * valori di ritorno), senza terminare il processo. Questa
     * disposizione viene ereditata anche dagli esecutori creati
     * con fork(). */
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction(SIGPIPE)");
        exit(EXIT_FAILURE);
    }

    /* SIGTERM gestito da sigterm_handler.
     * Non si usa SA_RESTART: si vuole che le chiamate bloccanti
     * (in particolare accept()) vengano interrotte con EINTR in
     * modo da poter controllare il flag 'terminating' e iniziare
     * la procedura di chiusura. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction(SIGTERM)");
        exit(EXIT_FAILURE);
    }
}

/* Funzione che implementa il corpo del processo ESECUTORE, definita
 * piu' avanti in questo stesso file. Non ritorna mai. */
static void run_executor(int client_fd);

int main(void) {
    int rc;

    memset(executors, 0, sizeof(executors));

    setup_signal_handlers();

    listen_fd = create_listening_socket();

    fprintf(stderr, "[server] in ascolto su %s (pid %d)\n",
            SOCKET_PATH, (int) getpid());

    /* Ciclo principale del server */
    while (!terminating) {
        int client_fd;
        pid_t pid;

        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR) {
                /* Interrotto da un segnale (probabilmente SIGTERM):
                 * si ricontrolla il flag 'terminating' all'inizio
                 * del while. */
                continue;
            }
            perror("accept");
            continue;
        }

        fprintf(stderr, "[server] nuova connessione accettata "
                        "(fd %d)\n", client_fd);

        pid = fork();
        if (pid == -1) {
            perror("fork");
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            /* Processo ESECUTORE */

            /* L'esecutore non deve avere il socket di ascolto
             * aperto: lo chiude immediatamente. */
            close(listen_fd);

            run_executor(client_fd); /* non ritorna */
            _exit(EXIT_FAILURE);     /* mai raggiunto */
        }

        /* Processo SERVER (padre):
         * la connessione e' ora gestita interamente
         * dall'esecutore appena creato; il server chiude la sua
         * copia del file descriptor e torna in ascolto. */
        close(client_fd);
        add_executor(pid);

        fprintf(stderr, "[server] creato esecutore pid %d "
                        "(esecutori attivi: %d)\n",
                (int) pid, n_executors);

        /* Raccoglie eventuali esecutori gia' terminati, per
         * evitare zombie senza dover usare wait bloccante qui. */
        reap_finished_executors();
    }

    /* --- Procedura di terminazione (SIGTERM ricevuto) --- */
    fprintf(stderr, "[server] ricevuto SIGTERM: avvio terminazione "
                    "ordinata...\n");

    /* Non si accettano piu' nuove connessioni */
    close(listen_fd);

    /* Avvisa gli esecutori attivi e attende la loro terminazione */
    shutdown_executors();

    /* Rimuove il file di socket */
    rc = unlink(SOCKET_PATH);
    if (rc == -1 && errno != ENOENT) {
        perror("[server] unlink");
    }

    fprintf(stderr, "[server] terminazione completata.\n");
    return EXIT_SUCCESS;
}

/* ====================================================================
 *  PROCESSO ESECUTORE
 *
 *  Il codice seguente viene eseguito esclusivamente nei processi
 *  figli creati con fork() dal server, uno per ogni client connesso.
 * ==================================================================== */

/*
 * Flag impostato dall'handler di SIGUSR1 nell'esecutore: indica
 * che il server ha richiesto la terminazione dell'applicazione.
 */
static volatile sig_atomic_t executor_must_terminate = 0;

static void sigusr1_handler(int signo) {
    (void) signo;
    executor_must_terminate = 1;
}

/*
 * Invia al client un singolo "chunk" di output, con l'header di
 * lunghezza richiesto dal protocollo (vedi common.h).
 * len puo' essere 0 per indicare la fine dell'output.
 *
 * Restituisce 0 in caso di successo, -1 in caso di errore (per
 * esempio se il client ha chiuso la connessione).
 */
static int send_chunk(int fd, const char *data, size_t len) {
    uint32_t net_len = htonl((uint32_t) len);
    ssize_t n;

    n = write(fd, &net_len, sizeof(net_len));
    if (n != (ssize_t) sizeof(net_len)) {
        return -1;
    }

    if (len > 0) {
        size_t sent = 0;
        while (sent < len) {
            n = write(fd, data + sent, len - sent);
            if (n <= 0) {
                return -1;
            }
            sent += (size_t) n;
        }
    }

    return 0;
}

#define MAX_ARGS 64

/*
 * Esegue il comando descritto da argv[] (terminato da NULL) e invia
 * l'output prodotto (stdout + stderr) al client identificato da
 * client_fd, seguendo il protocollo a chunk descritto in common.h.
 *
 * L'esecuzione avviene OBBLIGATORIAMENTE tramite fork() + una
 * funzione della famiglia exec*(), come richiesto dalle specifiche
 * (NON system()).
 *
 * L'output del comando viene catturato tramite una pipe: lo stdout
 * e lo stderr del processo figlio vengono ridirezionati sul lato di
 * scrittura della pipe; l'esecutore legge dal lato di lettura e
 * spedisce i dati al client in chunk via socket.
 */
static void execute_and_reply(int client_fd, char *argv[]) {
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) == -1) {
        perror("[esecutore] pipe");
        send_chunk(client_fd, "errore interno (pipe)\n", 22);
        send_chunk(client_fd, NULL, 0);
        return;
    }

    pid = fork();
    if (pid == -1) {
        perror("[esecutore] fork");
        close(pipefd[0]);
        close(pipefd[1]);
        send_chunk(client_fd, "errore interno (fork)\n", 22);
        send_chunk(client_fd, NULL, 0);
        return;
    }

    if (pid == 0) {
        /* Processo figlio: esegue il comando richiesto */
        struct sigaction sa;

        /* Ripristina i segnali a default nel processo figlio,
         * cosi' un comando come "sleep 100" si comporta in modo
         * normale rispetto ai segnali. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGPIPE, &sa, NULL);

        close(pipefd[0]); /* non serve il lato di lettura */

        /* Ridirige stdout e stderr sul lato di scrittura della pipe */
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
            _exit(127);
        }
        if (dup2(pipefd[1], STDERR_FILENO) == -1) {
            _exit(127);
        }
        close(pipefd[1]);

        /* Il processo che esegue il comando non deve avere accesso
         * al socket col client. */
        close(client_fd);

        execvp(argv[0], argv);

        /* Se arriviamo qui, execvp e' fallita (es. comando non
         * trovato). Si stampa un messaggio su stderr (ridiretto
         * verso la pipe, quindi arrivera' al client) e si termina
         * con exit(), come richiesto dalle specifiche. */
        fprintf(stderr, "esecuzione remota: comando non trovato: %s\n",
                argv[0]);
        exit(127);
    }

    /* Processo padre (esecutore): legge l'output dalla pipe e lo
     * spedisce al client in chunk. */
    close(pipefd[1]); /* non serve il lato di scrittura */

    for (;;) {
        char buf[OUT_CHUNK_LEN];
        ssize_t n;

        n = read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                if (executor_must_terminate) {
                    /* Il server ha richiesto la terminazione mentre
                     * un comando era in esecuzione: termina anche
                     * il comando in corso, per non restare bloccati
                     * in attesa del suo completamento. */
                    kill(pid, SIGTERM);
                    break;
                }
                continue;
            }
            perror("[esecutore] read pipe");
            break;
        }
        if (n == 0) {
            break; /* EOF: il comando ha terminato la scrittura */
        }

        if (send_chunk(client_fd, buf, (size_t) n) == -1) {
            /* Il client ha chiuso la connessione: si interrompe la
             * lettura, ma si attende comunque la terminazione del
             * processo figlio per evitare zombie. */
            break;
        }
    }

    close(pipefd[0]);

    /* Attende la terminazione del comando eseguito (attesa
     * bloccante, non attiva). Se nel frattempo arriva la richiesta
     * di terminazione dal server (SIGUSR1), il comando in corso
     * viene terminato con SIGTERM per non restare bloccati. */
    {
        int status;
        pid_t w;
        for (;;) {
            w = waitpid(pid, &status, 0);
            if (w == -1 && errno == EINTR) {
                if (executor_must_terminate) {
                    kill(pid, SIGTERM);
                }
                continue;
            }
            break;
        }
    }

    /* Chunk finale di lunghezza 0: segnala al client la fine
     * dell'output. */
    send_chunk(client_fd, NULL, 0);
}

/*
 * Suddivide la riga di comando 'line' in argv[] usando strtok(3),
 * cosi' come suggerito dalle specifiche per supportare comandi con
 * argomenti (nota di merito). Il delimitatore e' lo spazio/tab.
 * argv viene terminato da un puntatore NULL, come richiesto da
 * execvp().
 *
 * 'line' viene modificata (strtok inserisce dei '\0'), quindi deve
 * essere un buffer scrivibile.
 *
 * Restituisce il numero di token trovati (0 se la riga e' vuota o
 * contiene solo spazi).
 */
static int tokenize_command(char *line, char *argv[MAX_ARGS + 1]) {
    int argc = 0;
    char *tok;

    tok = strtok(line, " \t");
    while (tok != NULL && argc < MAX_ARGS) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    return argc;
}

/*
 * Legge una riga di comando inviata dal client.
 * Restituisce:
 *    >0  numero di byte letti
 *     0  il client ha chiuso la connessione (EOF), oppure la read
 *        e' stata interrotta da SIGUSR1
 *    -1  errore di lettura
 */
static ssize_t recv_command(int fd, char *buf, size_t bufsize) {
    ssize_t n;

    memset(buf, 0, bufsize);

    n = read(fd, buf, bufsize - 1);
    if (n < 0) {
        if (errno == EINTR) {
            /* Interrotti da SIGUSR1 (richiesta di terminazione dal
             * server): si tratta il caso come EOF, cosi' il
             * chiamante puo' chiudere ordinatamente la connessione. */
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        return 0; /* connessione chiusa dal client */
    }

    buf[n] = '\0';

    return n;
}

/*
 * Corpo principale del processo ESECUTORE.
 *
 * Cicla:
 *   1. attende un comando dal client assegnato
 *   2. se il comando e' "exit" o la connessione e' chiusa, termina
 *   3. altrimenti esegue il comando (fork+exec) e ne rispedisce
 *      l'output al client
 *
 * Gestisce SIGUSR1 (richiesta di terminazione dal server): in tal
 * caso chiude la connessione col client e termina, come previsto
 * dalle specifiche.
 */
static void run_executor(int client_fd) {
    struct sigaction sa;
    char buf[CMD_MAX_LEN];

    fprintf(stderr, "[esecutore %d] avviato\n", (int) getpid());

    /* SIGINT e SIGQUIT ignorati anche nell'esecutore */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);

    /* SIGUSR1: richiesta di terminazione da parte del server.
     * Non si usa SA_RESTART, in modo che read() venga interrotta
     * con EINTR mentre si e' in attesa di un comando dal client. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    for (;;) {
        ssize_t n;
        char *argv[MAX_ARGS + 1];
        int argc;

        if (executor_must_terminate) {
            fprintf(stderr, "[esecutore %d] richiesta di terminazione "
                            "dal server: chiudo la connessione e termino\n",
                    (int) getpid());
            break;
        }

        n = recv_command(client_fd, buf, sizeof(buf));
        if (n == -1) {
            perror("[esecutore] read");
            break;
        }
        if (n == 0) {
            if (executor_must_terminate) {
                fprintf(stderr, "[esecutore %d] richiesta di "
                                "terminazione dal server\n",
                        (int) getpid());
            } else {
                fprintf(stderr, "[esecutore %d] connessione chiusa "
                                "dal client\n", (int) getpid());
            }
            break;
        }

        /* Rimuove eventuale '\n' finale */
        {
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') {
                buf[len - 1] = '\0';
            }
        }

        if (strcmp(buf, EXIT_CMD) == 0) {
            fprintf(stderr, "[esecutore %d] comando 'exit' ricevuto: "
                            "termino\n", (int) getpid());
            break;
        }

        /* Copia la riga originale per il logging, prima che
         * tokenize_command la modifichi inserendo dei '\0'. */
        {
            char logbuf[CMD_MAX_LEN];
            strncpy(logbuf, buf, sizeof(logbuf) - 1);
            logbuf[sizeof(logbuf) - 1] = '\0';

            /* Tokenizza il comando in argv[] per supportare comandi
             * con argomenti (nota di merito). */
            argc = tokenize_command(buf, argv);
            if (argc == 0) {
                /* Riga vuota: si invia subito il chunk di fine
                 * output, senza eseguire nulla. */
                send_chunk(client_fd, NULL, 0);
                continue;
            }

            fprintf(stderr, "[esecutore %d] eseguo comando: %s\n",
                    (int) getpid(), logbuf);
        }

        execute_and_reply(client_fd, argv);
    }

    close(client_fd);
    fprintf(stderr, "[esecutore %d] terminato\n", (int) getpid());
    exit(EXIT_SUCCESS);
}
