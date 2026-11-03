//! Independent video erasure decoder; no upstream implementation is linked or translated.
//!
//! Wire facts: nanors `rs.c:111-139` at
//! a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29 specifies a systematic Cauchy code:
//! parity coefficient (row r, data column c) = inverse((P+c) XOR r).
//! `deps/obl/gf2_8_tables.h:1` pins polynomial 285 (0x11d).
//! https://github.com/sleepybishop/nanors/blob/a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29/rs.c#L111-L139
//! See docs/protocol-notes.md §7 for packet geometry and post-encode header edits.

use anyhow::{bail, ensure, Result};

fn multiply(mut a: u8, mut b: u8) -> u8 {
    let mut product = 0;
    while b != 0 {
        if b & 1 != 0 {
            product ^= a;
        }
        a = (a << 1) ^ if a & 0x80 != 0 { 0x1d } else { 0 };
        b >>= 1;
    }
    product
}

fn inverse(value: u8) -> u8 {
    // Fermat's theorem in GF(256): a^-1 = a^254 for nonzero a.
    let mut result = 1;
    let mut power = value;
    let mut exponent = 254;
    while exponent != 0 {
        if exponent & 1 != 0 {
            result = multiply(result, power);
        }
        power = multiply(power, power);
        exponent >>= 1;
    }
    result
}

fn coefficient(parity: usize, row: usize, column: usize) -> u8 {
    inverse(((parity + column) ^ row) as u8)
}

/// Rebuild missing data columns from unique indexed shards, zero-padding short packets.
///
/// Payload columns can be decoded independently of the RTP/NV header columns that Sunshine
/// overwrites after encoding. Only data shards are reconstructed; parity is left untouched.
/// Errors leave missing shards absent. This is erasure recovery, not corruption detection.
/// `size` is the padded payload size, excluding the 32-byte packet headers.
pub fn reconstruct(
    data: usize,
    parity: usize,
    shards: &mut [Option<Vec<u8>>],
    size: usize,
) -> Result<usize> {
    ensure!(
        data > 0 && data <= 255 && parity <= 255 && data + parity <= 255,
        "invalid FEC geometry"
    );
    ensure!(shards.len() == data + parity, "invalid shard array length");
    ensure!(size > 0 && size <= 65536 - 32, "invalid FEC payload size");
    ensure!(
        shards.iter().flatten().all(|s| s.len() <= size),
        "oversized FEC shard"
    );
    let missing: Vec<usize> = (0..data).filter(|&i| shards[i].is_none()).collect();
    let count = missing.len();
    if count == 0 {
        return Ok(0);
    }
    let rows: Vec<usize> = (0..parity)
        .filter(|&r| shards[data + r].is_some())
        .take(count)
        .collect();
    ensure!(rows.len() == count, "insufficient unique parity shards");

    // Solve only for erased data: known data terms move to the right hand side.
    // The largest matrix is bounded by the protocol's 255 total symbols.
    let mut matrix = vec![vec![0u8; count]; count];
    let mut rhs = vec![vec![0u8; size]; count];
    for (r, &parity_row) in rows.iter().enumerate() {
        let received = shards[data + parity_row].as_ref().expect("selected parity");
        rhs[r][..received.len()].copy_from_slice(received);
        for (column, shard) in shards.iter().take(data).enumerate() {
            if let Some(bytes) = shard {
                let factor = coefficient(parity, parity_row, column);
                for (out, &byte) in rhs[r].iter_mut().zip(bytes) {
                    *out ^= multiply(factor, byte);
                }
            }
        }
        for (c, &column) in missing.iter().enumerate() {
            matrix[r][c] = coefficient(parity, parity_row, column);
        }
    }
    for c in 0..count {
        let Some(pivot) = (c..count).find(|&r| matrix[r][c] != 0) else {
            bail!("singular FEC matrix");
        };
        matrix.swap(c, pivot);
        rhs.swap(c, pivot);
        let scale = inverse(matrix[c][c]);
        for byte in &mut matrix[c] {
            *byte = multiply(*byte, scale);
        }
        for byte in &mut rhs[c] {
            *byte = multiply(*byte, scale);
        }
        let (before, after) = matrix.split_at_mut(c);
        let (pivot_row, after) = after.split_first_mut().expect("pivot row");
        let (rhs_before, rhs_after) = rhs.split_at_mut(c);
        let (pivot_rhs, rhs_after) = rhs_after.split_first_mut().expect("pivot RHS");
        for (row, value) in before
            .iter_mut()
            .chain(after)
            .zip(rhs_before.iter_mut().chain(rhs_after))
        {
            let factor = row[c];
            for (out, &source) in row.iter_mut().zip(pivot_row.iter()) {
                *out ^= multiply(factor, source);
            }
            for (out, &source) in value.iter_mut().zip(pivot_rhs.iter()) {
                *out ^= multiply(factor, source);
            }
        }
    }
    for (column, bytes) in missing.into_iter().zip(rhs) {
        shards[column] = Some(bytes);
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Oracle: unmodified rs.c + deps/obl/{oblas_lite,oblas_common}.c from the pinned
    // nanors commit above, compiled with clang -O2 -std=c11 -Wall -Wextra -Werror.
    // reed_solomon_new(3,2); reed_solomon_encode(rs, shards, 5, 16).
    // Inputs are synthetic protocol payloads, not a host or console recording.
    pub const VECTOR: [&str; 5] = [
        "01340002020000000000016511223344",
        "5566778899aabbccddeeff0010203040",
        "abcd0000000000000000000000000000",
        "5e4c2d79766669444b5adbbc7dfa87e9",
        "d54fb5b13755d366e0770523070e091c",
    ];

    #[test]
    fn pinned_nanors_vector_recovers_every_one_and_two_erasure_combination() {
        for a in 0..5 {
            for b in a..5 {
                let mut shards: Vec<_> = VECTOR
                    .iter()
                    .enumerate()
                    .map(|(i, s)| (i != a && i != b).then(|| hex::decode(s).unwrap()))
                    .collect();
                let expected = shards[..3].iter().filter(|s| s.is_none()).count();
                assert_eq!(reconstruct(3, 2, &mut shards, 16).unwrap(), expected);
                for (i, shard) in shards.iter().take(3).enumerate() {
                    assert_eq!(shard.as_ref().unwrap(), &hex::decode(VECTOR[i]).unwrap());
                }
            }
        }
    }

    #[test]
    fn short_last_data_is_padded_and_insufficient_or_invalid_shards_are_rejected() {
        let mut shards: Vec<_> = VECTOR
            .iter()
            .map(|s| Some(hex::decode(s).unwrap()))
            .collect();
        shards[2].as_mut().unwrap().truncate(2);
        shards[0] = None;
        reconstruct(3, 2, &mut shards, 16).unwrap();
        assert_eq!(shards[0], Some(hex::decode(VECTOR[0]).unwrap()));
        shards[0] = None;
        shards[1] = None;
        shards[3] = None;
        assert!(reconstruct(3, 2, &mut shards, 16).is_err());
        assert!(shards[0].is_none());
        assert!(reconstruct(0, 0, &mut [], 16).is_err());
        assert!(reconstruct(3, 2, &mut shards, 1).is_err());
        assert!(reconstruct(3, 2, &mut shards, usize::MAX).is_err());
        assert!(reconstruct(3, usize::MAX, &mut shards, 16).is_err());
    }
}
