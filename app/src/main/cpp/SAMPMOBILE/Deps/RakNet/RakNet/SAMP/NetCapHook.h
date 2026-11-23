#pragma once

// dir: 0 = received, 1 = sent. kind: 0 = RPC (id = RPC number), 1 = raw packet (id = first byte).
// data is bit-packed exactly as it travels; bitLength is its length in bits.
typedef void (*RakNetCapHook)(int dir, int kind, int id, const unsigned char* data, unsigned int bitLength);

extern RakNetCapHook g_rakNetCapHook;
