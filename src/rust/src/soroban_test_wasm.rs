use crate::RustBuf;

// Accessors for test wasms, compiled into soroban-test-wasms crate.
pub(crate) fn get_test_wasm_add_i32() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ADD_I32.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_sum_i32() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::SUM_I32.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_contract_data() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONTRACT_STORAGE
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_test_wasm_complex() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::COMPLEX.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_err() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::ERR.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_wasm_loadgen() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::LOADGEN.iter().cloned().collect(),
    })
}

pub(crate) fn get_test_contract_sac_transfer() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONTRACT_SAC_TRANSFER_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_write_bytes() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::WRITE_BYTES.iter().cloned().collect(),
    })
}

pub(crate) fn get_hostile_large_val_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::HOSTILE_LARGE_VALUE
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_auth_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::AUTH_TEST_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_no_arg_constructor_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::NO_ARGUMENT_CONSTRUCTOR_TEST_CONTRACT_P22
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_constructor_with_args_p21_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONSTRUCTOR_TEST_CONTRACT_P21
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_constructor_with_args_p22_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::CONSTRUCTOR_TEST_CONTRACT_P22
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_custom_account_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::SIMPLE_ACCOUNT_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_invoke_contract_wasm() -> Result<RustBuf, Box<dyn std::error::Error>> {
    Ok(RustBuf {
        data: soroban_test_wasms::INVOKE_CONTRACT
            .iter()
            .cloned()
            .collect(),
    })
}

pub(crate) fn get_random_wasm(
    size: usize,
    seed: u64,
) -> Result<RustBuf, Box<dyn std::error::Error>> {
    use rand::{rngs::StdRng, RngCore, SeedableRng};
    use soroban_synth_wasm::*;
    let mut fe = ModEmitter::default().func(Arity(0), 0);

    // Generate a very exciting wasm that pushes and drops random numbers.
    //
    // We want to generate a random i64 number that is exactly 6 bytes when
    // encoded as LEB128, so that that number plus two 1-byte opcodes gets us to
    // 8 bytes.
    //
    // LEB128 encodes 7 bits at a time into each byte. So a 6 byte output will
    // encode 6 * 7 = 42 bits of input. Or it would if it was unsigned; signed
    // LEB128 needs 1 bit from the MSB for sign-indication, so it's actually 41
    // bits.
    //
    // We want to set the 41st bit of that to 1 to guarantee it's always set,
    // and then the low 40 bits we will assign random data to.
    let mut prng = StdRng::seed_from_u64(seed);
    let n_loops = size / 8;
    for _ in 0..n_loops {
        let mut i = prng.next_u64();
        // Cut the random number down to 41 bits.
        i &= 0x0000_01ff_ffff_ffff;
        // Ensure the number has its 41st bit set.
        i |= 0x0000_0100_0000_0000;
        // Emit a 1-byte opcode + 6 byte LEB128 = 7 bytes.
        fe.i64_const(i as i64);
        // Emit 1 more byte of opcode, making 8.
        fe.drop();
    }
    // Push a return value.
    fe.i64_const(0);
    fe.ret();
    Ok(RustBuf {
        data: fe.finish_and_export("test").finish(),
    })
}

#[test]
fn test_get_random_wasm() {
    // HEADERSZ varies a bit as we fiddle with the definition of the wasm
    // synthesis system we want to check that it hasn't grown unreasonable and
    // we're _basically_ building something of the requested size, or at least
    // right order of magnitude.
    const HEADERSZ: usize = 150;
    for i in 1..10 {
        let wasm = get_random_wasm(1000 * i, i as u64).unwrap();
        assert!(wasm.data.len() < 1000 * i + HEADERSZ);
    }
}
