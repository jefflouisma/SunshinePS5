# Synthetic legacy identity

These are public test fixtures, never a real client identity or pairing credential.
The private key is intentionally disclosed for deterministic compatibility tests.

Generated on 2026-10-31 using the pre-migration `ClientIdentity::load_or_create`
implementation (`identity.rs` SHA-256
`a7acdc2cdb26ba36c2e454a0071ef11a16a59daf43c847e5acf3a91a7d234c62`),
`rsa 0.9.10`, `rcgen 0.13`, and the then-current locked dependencies
(`Cargo.lock` SHA-256
`687b2c6088f479694b65bb4e408bf995ab6f16c7f0ed090b2bad03c4e82bbb6d`).

`signature.hex` is the old backend's SHA-256/PKCS#1 v1.5 signature of the exact
UTF-8 bytes `SunshinePS5 legacy RSA compatibility fixture\n` (LF at the end).
The test loads this unchanged PKCS#8 key, certificate, and unique ID through the
normal persistence path and requires byte-identical signatures from AWS-LC.
It also rejects changed messages, signatures, lengths, and keys.

`weak-private-key.pem.txt` is a separate synthetic 1024-bit key generated with
`openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:1024`. Loading must fail
at private-key validation, before certificate matching; it is never used to sign.
