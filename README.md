# Esecuzione Remota

Progetto di Laboratorio di Sistemi Operativi (a.a. 2025-26).

Implementa un'applicazione client-server per l'esecuzione remota di
comandi shell, secondo la traccia "Esecuzione Remota":

- **server** (`src/server.c`): resta in ascolto di connessioni dai
  client su un socket Unix (`/tmp/esecuzione_remota.sock`). Per ogni
  client crea con `fork()` un processo **esecutore**, che da quel
  momento gestisce in autonomia tutta la comunicazione con quel
  client.
- **esecutore**: riceve comandi dal client, li esegue con
  `fork()` + `execvp()` (mai `system()`), e rispedisce l'output al
  client. Supporta comandi con argomenti (tramite `strtok`).
- **client** (`src/client.c`): si connette al server, invia comandi
  inseriti dall'utente e ne mostra l'output.

La comunicazione client-server e client-esecutore avviene
esclusivamente tramite socket Unix (stream), nel rispetto delle
specifiche (nessun file su disco usato per lo scambio dati).

## Compilazione

```sh
./run.sh build
```

Crea gli eseguibili `bin/server` e `bin/client`.

## Esecuzione

In un terminale, avviare il server:

```sh
./run.sh server
```

In uno o più altri terminali, avviare uno o più client:

```sh
./run.sh client
```

Da ogni client è possibile inserire comandi shell (anche con
argomenti, es. `ls -l /tmp`) da eseguire in remoto. Digitando `exit`
il client e il relativo esecutore terminano.

## Terminazione dell'applicazione

Il server termina solo se riceve **SIGTERM**:

```sh
kill -TERM <pid_server>
```

Prima di terminare, il server avvisa tutti gli esecutori ancora
attivi (che chiudono la connessione con il proprio client e
terminano) e attende la terminazione di tutti loro prima di uscire e
rimuovere il file di socket.

SIGINT e SIGQUIT (Ctrl-C, Ctrl-\\) sono ignorati da tutti i processi
dell'applicazione.

## Pulizia

```sh
./run.sh clean
```

Rimuove gli eseguibili compilati e l'eventuale file di socket
residuo in `/tmp`.

## Note implementative

- Compilato con `-Wall -Wextra -std=c11`, nessun warning.
- Nessuna attesa attiva: `accept()`, `read()` e `wait()/waitpid()`
  sono usate in modalità bloccante.
- L'esecutore gestisce correttamente la richiesta di terminazione
  del server anche mentre è in esecuzione un comando: il comando in
  corso viene terminato (SIGTERM) per consentire una chiusura rapida
  dell'intera applicazione.
- L'applicazione non richiede privilegi di root né un ambiente
  grafico; il socket viene creato in `/tmp`.
=======
# Remote-Execution-System
> **Remote Execution System** is a lightweight C-based client-server application designed to handle remote command execution and network communication. It features modular client and server components, shared headers, and automated deployment scripts.
>>>>>>> 36e923408b5418b90482a634f2d8e5ea152b9c39
