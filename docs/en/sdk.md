# SDK

The current source provides two mutually independent SDKs, both with SOVERSION 1:

- `libminitun-client.so.1` only controls the local `minitund` through the Unix IPC; it does
  not embed the daemon/server and does not expose JSON, Asio, SQLite or internal records;
- `libminitun-remote-protocol.so.1` provides Remote Protocol v2 strongly-typed messages, an
  incremental frame decoder, codec and authentication digest helpers; it does not create
  sockets, TLS sessions or a runtime.

## Installation and linking

Debian/Ubuntu installs `libminitun-client1` and `libminitun-client-dev`; RPM installs
`libminitun-client1` and `libminitun-client-devel`.

CMake (the package exports both targets):

```cmake
find_package(MiniTun 1.1 REQUIRED CONFIG)
target_link_libraries(my_tool PRIVATE MiniTun::Client)
target_link_libraries(my_protocol_tool PRIVATE MiniTun::RemoteProtocol)
```

pkg-config:

```bash
cc -std=c11 tool.c $(pkg-config --cflags --libs minitun-client)
c++ -std=c++20 tool.cpp $(pkg-config --cflags --libs minitun-client)
c++ -std=c++20 protocol.cpp \
  $(pkg-config --cflags --libs minitun-remote-protocol)
```

## Local control SDK

## C11 example

```c
#include <minitun/client.h>
#include <stdio.h>

int main(void) {
    minitun_client *client = NULL;
    minitun_error *error = NULL;
    minitun_client_options options = {sizeof(options), NULL};
    if (minitun_client_abi_version() != MINITUN_CLIENT_ABI_VERSION ||
        minitun_client_create(&options, &client, &error) != 0) {
        fprintf(stderr, "%s\n", error ? error->message : "ABI mismatch");
        minitun_error_free(error);
        return 1;
    }
    minitun_status status = {0};
    if (minitun_client_status_get(client, &status, &error) == 0) {
        printf("active tunnels: %llu\n", (unsigned long long)status.tunnel_active);
    }
    minitun_error_free(error);
    minitun_client_destroy(client);
    return 0;
}
```

Every returned object has a corresponding explicit free function. The ABI boundary does not
throw C++ exceptions; failure returns a non-zero value and optionally fills a stable error
code and non-sensitive message. Input structs must set `struct_size` to the size the caller
compiled against.

## C++20 example

```cpp
#include <minitun/client.hpp>
#include <iostream>

int main() {
    auto created = minitun::Client::create();
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto status = created.value().status();
    if (!status) {
        std::cerr << status.error().message << '\n';
        return 1;
    }
    std::cout << status.value().tunnel_active << '\n';
}
```

The C++ wrapper provides an RAII handle, strongly-typed models, `Result<T>`,
`UpdateField<T>` and lifecycle actions. The same `Client` object may be called from
multiple threads; each operation builds an independent bounded local IPC request.

`TunnelCreate::protocol` accepts `tcp`, `udp`, `socks5` or `p2p`, and `remote_host`
controls the numeric bind address on the server side; the C API's
`minitun_tunnel_create_request` and `minitun_tunnel_update_request` provide the
corresponding fields at the end of the struct. Older callers with a smaller `struct_size`
are still accepted and interpreted as `tcp`/`0.0.0.0`, so 1.0 binaries keep working without
recompiling.

## Remote Protocol C++20 SDK

The header is `<minitun/remote_protocol.hpp>`. The example below encodes a strongly-typed
`HELLO` into a complete wire frame, then decodes it with a decoder that accepts arbitrary
TCP/TLS fragmentation:

```cpp
#include <minitun/remote_protocol.hpp>

using namespace minitun;

remote::Message message = protocol::HelloMessage{
    .client_id = "client_0123456789abcdef0123456789abcdef",
    .capabilities = protocol::kSupportedCapabilities,
};
auto frame = remote::Codec::make_frame(protocol::MessageType::hello, 1, message);
if (!frame) {
    return 1;
}
auto wire = remote::Codec::encode_frame(*frame);
if (!wire) {
    return 1;
}

remote::Decoder decoder;
auto frames = decoder.feed(*wire);
if (!frames || frames->size() != 1 || !decoder.finish()) {
    return 1;
}
auto decoded = remote::Codec::decode_message(frames->front());
return decoded ? 0 : 1;
```

`Decoder::feed()` may return zero to several complete frames; `finish()` must be called at
EOF and rejects a truncated header/payload. `reset()` reuses the decoder.
`Codec::control_authentication_data()` and `worker_authentication_data()` generate the
protocol-specified HMAC input results; the caller remains responsible for TLS 1.2+, nonce
randomness/replay cache, clock window, state machine and secret lifetime.

## ABI commitment

Both shared libraries hide symbols by default. The local SDK only exports the stable
`minitun_*` API; the Remote SDK uses a separate linker export list and only exports the
public construction, decoder and codec symbols of `minitun::remote`. CI checks both dynamic
symbol boundaries and compiles C11, C++20, pkg-config and downstream
`find_package(MiniTun)` examples. 1.x only allows backward-compatible additions; removing,
renaming, changing existing struct field meanings or calling conventions is deferred to
2.0.
