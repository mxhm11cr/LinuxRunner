# Native Unix socket backing paths

Long iOS container paths could exhaust Darwin sockaddr_un before a Linux client
added its own socket suffix. The app now uses an identity-checked short alias
for its socket backing directory; the backing encoder preserves both complete
IDs within a bounded path. Guest Unix-socket names and namespace behavior stay
unchanged.

`./scripts/test-native-unix-sockets.sh` checks bounds, complete IDs, alias identity
and failure cases on macOS. `unix_socket_namespace_contract.c` is the guest
contract for pathname/abstract bindings. The earlier physical Arch integration
passed this guest contract; this extraction is not a separate device run.
