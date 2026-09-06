/*
 * common.h
 *
 * Definizioni comuni condivise da server, esecutore e client
 * del progetto "Esecuzione Remota".
 */

#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

/* Path del socket Unix su cui il server resta in ascolto.
 * Si usa /tmp per evitare problemi di permessi e di esecuzione
 * come utente root. */
#define SOCKET_PATH "/tmp/esecuzione_remota.sock"

/* Dimensione massima di un comando inviato dal client all'esecutore */
#define CMD_MAX_LEN 1024

/* Dimensione massima di un singolo "chunk" di output inviato
 * dall'esecutore al client. L'output puo' essere piu' lungo e
 * viene quindi spedito in piu' chunk consecutivi. */
#define OUT_CHUNK_LEN 4096

/* Stringa che il client invia per terminare la sessione */
#define EXIT_CMD "exit"

/*
 * Protocollo di comunicazione client <-> esecutore (su socket stream):
 *
 *  - Il client invia un comando come stringa terminata da '\0',
 *    di lunghezza massima CMD_MAX_LEN (incluso il terminatore).
 *
 *  - L'esecutore risponde inviando l'output del comando come
 *    sequenza di messaggi "framed":
 *
 *        [ 4 byte: lunghezza del chunk in network byte order (uint32_t) ]
 *        [ N byte: dati del chunk (N = lunghezza indicata) ]
 *
 *    L'ultimo chunk di una risposta ha lunghezza 0 (header con
 *    valore 0), che funge da "fine output". In questo modo il
 *    client sa quando ha ricevuto tutto l'output e puo' tornare
 *    a chiedere un nuovo comando, anche se l'output e' piu'
 *    grande di OUT_CHUNK_LEN o contiene byte qualsiasi.
 *
 *  - Se il client invia "exit", l'esecutore non invia alcun
 *    output (chiude direttamente la connessione e termina).
 *
 *  - Se l'esecutore termina per richiesta del server (SIGUSR1),
 *    chiude semplicemente la connessione: il client rileva la
 *    chiusura (read che ritorna 0) e termina a sua volta.
 */

#endif /* COMMON_H */
