//! Symmetric primitives used by pairing and the streams.

use aes::cipher::{BlockDecrypt, BlockDecryptMut, BlockEncrypt, KeyInit, KeyIvInit};
use aes::Aes128;
use aes_gcm::aead::{Aead, Payload};
use aes_gcm::{Aes128Gcm, Nonce};
use anyhow::{anyhow, bail, Result};

/// AES-128-ECB without padding, as pairing uses it. Input must be whole blocks.
pub fn aes_ecb_encrypt(key: &[u8; 16], data: &[u8]) -> Result<Vec<u8>> {
    if !data.len().is_multiple_of(16) {
        bail!(
            "ECB input of {} bytes is not a whole number of blocks",
            data.len()
        );
    }
    let cipher = Aes128::new(key.into());
    let mut out = data.to_vec();
    for block in out.chunks_exact_mut(16) {
        cipher.encrypt_block(block.into());
    }
    Ok(out)
}

pub fn aes_ecb_decrypt(key: &[u8; 16], data: &[u8]) -> Result<Vec<u8>> {
    if !data.len().is_multiple_of(16) {
        bail!(
            "ECB input of {} bytes is not a whole number of blocks",
            data.len()
        );
    }
    let cipher = Aes128::new(key.into());
    let mut out = data.to_vec();
    for block in out.chunks_exact_mut(16) {
        cipher.decrypt_block(block.into());
    }
    Ok(out)
}

/// AES-128-GCM with a 16-byte tag supplied separately, as the stream protocols carry it.
pub fn aes_gcm_decrypt(
    key: &[u8; 16],
    nonce: &[u8; 12],
    ciphertext: &[u8],
    tag: &[u8],
) -> Result<Vec<u8>> {
    if tag.len() != 16 {
        bail!("GCM tag must be 16 bytes, got {}", tag.len());
    }
    let mut combined = Vec::with_capacity(ciphertext.len() + 16);
    combined.extend_from_slice(ciphertext);
    combined.extend_from_slice(tag);
    Aes128Gcm::new(key.into())
        .decrypt(
            Nonce::from_slice(nonce),
            Payload {
                msg: &combined,
                aad: &[],
            },
        )
        .map_err(|_| anyhow!("GCM authentication failed"))
}

/// Returns `(ciphertext, tag)`.
pub fn aes_gcm_encrypt(
    key: &[u8; 16],
    nonce: &[u8; 12],
    plaintext: &[u8],
) -> Result<(Vec<u8>, [u8; 16])> {
    let mut out = Aes128Gcm::new(key.into())
        .encrypt(
            Nonce::from_slice(nonce),
            Payload {
                msg: plaintext,
                aad: &[],
            },
        )
        .map_err(|_| anyhow!("GCM encryption failed"))?;
    let tag_start = out.len() - 16;
    let tag: [u8; 16] = out[tag_start..].try_into().expect("16-byte tag");
    out.truncate(tag_start);
    Ok((out, tag))
}

/// AES-128-GCM with a 16-byte nonce: the legacy control-stream IV used when the client and
/// host have not negotiated `SS_ENC_CONTROL_V2` (protocol-notes §9).
type Aes128Gcm16 = aes_gcm::AesGcm<Aes128, aes_gcm::aead::consts::U16>;

pub fn aes_gcm16_encrypt(
    key: &[u8; 16],
    nonce: &[u8; 16],
    plaintext: &[u8],
) -> Result<(Vec<u8>, [u8; 16])> {
    let mut out = Aes128Gcm16::new(key.into())
        .encrypt(
            nonce.into(),
            Payload {
                msg: plaintext,
                aad: &[],
            },
        )
        .map_err(|_| anyhow!("GCM encryption failed"))?;
    let tag_start = out.len() - 16;
    let tag: [u8; 16] = out[tag_start..].try_into().expect("16-byte tag");
    out.truncate(tag_start);
    Ok((out, tag))
}

pub fn aes_gcm16_decrypt(
    key: &[u8; 16],
    nonce: &[u8; 16],
    ciphertext: &[u8],
    tag: &[u8],
) -> Result<Vec<u8>> {
    if tag.len() != 16 {
        bail!("GCM tag must be 16 bytes, got {}", tag.len());
    }
    let mut combined = Vec::with_capacity(ciphertext.len() + 16);
    combined.extend_from_slice(ciphertext);
    combined.extend_from_slice(tag);
    Aes128Gcm16::new(key.into())
        .decrypt(
            nonce.into(),
            Payload {
                msg: &combined,
                aad: &[],
            },
        )
        .map_err(|_| anyhow!("GCM authentication failed"))
}

/// AES-128-CBC decryption with PKCS#7 padding removed.
pub fn aes_cbc_decrypt_pkcs7(key: &[u8; 16], iv: &[u8; 16], data: &[u8]) -> Result<Vec<u8>> {
    type Dec = cbc::Decryptor<Aes128>;
    let mut buf = data.to_vec();
    let plain = Dec::new(key.into(), iv.into())
        .decrypt_padded_mut::<aes::cipher::block_padding::Pkcs7>(&mut buf)
        .map_err(|_| anyhow!("CBC decryption or padding check failed"))?;
    Ok(plain.to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ecb_matches_fips197_vector() {
        // FIPS-197 Appendix C.1 (AES-128).
        let key: [u8; 16] = hex::decode("000102030405060708090a0b0c0d0e0f")
            .unwrap()
            .try_into()
            .unwrap();
        let pt = hex::decode("00112233445566778899aabbccddeeff").unwrap();
        let ct = aes_ecb_encrypt(&key, &pt).unwrap();
        assert_eq!(hex::encode(&ct), "69c4e0d86a7b0430d8cdb78070b4c55a");
        assert_eq!(aes_ecb_decrypt(&key, &ct).unwrap(), pt);
        assert!(aes_ecb_encrypt(&key, &pt[..15]).is_err());
    }

    #[test]
    fn gcm_round_trip_and_tamper_detection() {
        let key = [7u8; 16];
        let nonce = [1u8; 12];
        let (ct, tag) = aes_gcm_encrypt(&key, &nonce, b"control message").unwrap();
        assert_eq!(
            aes_gcm_decrypt(&key, &nonce, &ct, &tag).unwrap(),
            b"control message"
        );
        let mut bad = ct.clone();
        bad[0] ^= 1;
        assert!(aes_gcm_decrypt(&key, &nonce, &bad, &tag).is_err());
    }

    #[test]
    fn legacy_gcm_round_trip() {
        let (key, nonce) = ([2u8; 16], [3u8; 16]);
        let (ct, tag) = aes_gcm16_encrypt(&key, &nonce, b"legacy").unwrap();
        assert_eq!(
            aes_gcm16_decrypt(&key, &nonce, &ct, &tag).unwrap(),
            b"legacy"
        );
    }

    #[test]
    fn cbc_decrypts_pkcs7() {
        use aes::cipher::BlockEncryptMut;
        type Enc = cbc::Encryptor<Aes128>;
        let (key, iv) = ([3u8; 16], [9u8; 16]);
        let msg = b"opus payload bytes";
        let mut buf = vec![0u8; 32];
        buf[..msg.len()].copy_from_slice(msg);
        let ct = Enc::new(&key.into(), &iv.into())
            .encrypt_padded_mut::<aes::cipher::block_padding::Pkcs7>(&mut buf, msg.len())
            .unwrap()
            .to_vec();
        assert_eq!(aes_cbc_decrypt_pkcs7(&key, &iv, &ct).unwrap(), msg);
    }
}
