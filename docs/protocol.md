# Remote protocol

The remote protocol is intentionally not implemented in stage 0.

The planned protocol is a TLS-protected, versioned binary protocol with explicit
network-byte-order field encoding. It will use bounded control frames and one TCP
connection per relay worker; C++ object layouts will never be serialized directly.
Protocol framing and state validation will be implemented in stage 5.
