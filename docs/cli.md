# Command-line interface

The current baseline provides:

```text
minitun --help
minitun --version
minitun help
minitun version
minitund --help
minitund --version
minitun-server --help
minitun-server --version
```

Invalid arguments return exit code `2`. The server, tunnel, daemon-status, and JSON
commands will be introduced after the local IPC layer exists.
