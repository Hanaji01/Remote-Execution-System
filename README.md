# Remote Execution System

A client-server application in C that runs shell commands on behalf of remote clients over Unix domain sockets. I wrote it for the Operating Systems Lab course (University of Perugia, 2025/26).

## How it works

```
client ──socket──> server ──fork()──> executor (one per client)
                                         └─ fork() + execvp() for each command
```

- The **server** listens on `/tmp/esecuzione_remota.sock`. For every new connection it forks an **executor** process and goes back to `accept()`.
- The **executor** owns that client from then on. It reads a command, splits it into arguments with `strtok`, runs it with `fork()` + `execvp()` (never `system()`), and sends the output back.
- The **client** reads commands from the user, sends them and prints the output. Typing `exit` closes both the client and its executor.

All data goes through stream sockets; no files are used to exchange data.

## Shutdown

Only `SIGTERM` stops the server. When it arrives, the server tells every live executor to stop. An executor that is in the middle of a command kills it first, then closes its client connection. The server waits for all of them before removing the socket file and exiting.

`SIGINT` and `SIGQUIT` (Ctrl-C, Ctrl-\\) are ignored by every process in the application.

There is no busy waiting: `accept()`, `read()` and `waitpid()` all block.

[TODO: one line on what was hardest, e.g. handling SIGTERM while an executor is blocked on a running command.]

## Build and run

Linux (or WSL), GCC:

```bash
./run.sh build      # builds bin/server and bin/client
./run.sh server     # terminal 1
./run.sh client     # terminal 2, 3, ... (several clients at once)
```

Then type commands in a client, for example `ls -l /tmp`. To stop the server:

```bash
kill -TERM <server_pid>
```

`./run.sh clean` removes the binaries and any leftover socket file.

The code compiles with `-Wall -Wextra -std=c11` without warnings and needs neither root nor a graphical environment.

## Files

| File | Contents |
| --- | --- |
| `src/server.c` | Server and executor processes, signal handling |
| `src/client.c` | Interactive client |
| `src/common.h` | Socket path, buffer sizes and the `exit` command |
| `run.sh` | Build, run and clean script |

## License

MIT
