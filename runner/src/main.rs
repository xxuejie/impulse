//! OpenVM host runner for impulse (JoltPhysics on OpenVM).
//!
//! Three build configurations, selected at compile time via cargo features:
//!   * `softfloat` (default): rv32im only, no FPU. compiler-rt's softfloat.
//!   * `zfinx`:           rv32im_zfinx, native FPU in integer registers.
//!   * `f`:               rv32imf, native FPU in FP register file (AS 5).
//!
//! Pick exactly one via `--features`. Mutually-exclusive combinations are
//! caught by compile_error! below.
//!
//! Modes:
//!   impulse-runner [elf]            execute, read public values, check sha256
//!   impulse-runner prove [elf]      execute + prove; the proof artifact
//!                                   (verifying key + continuation proof,
//!                                   bincode) is persisted to <elf>.proof and
//!                                   verified again via the standalone path
//!   impulse-runner verify <proof> [expected-hash-hex]
//!                                   standalone verification of a persisted
//!                                   proof artifact — no ELF needed; prints
//!                                   the revealed (ptr, size, hash) and
//!                                   optionally MATCHes against a recorded
//!                                   session hash

#![allow(unexpected_cfgs)]

#[cfg(not(any(feature = "softfloat", feature = "zfinx", feature = "f")))]
compile_error!("Enable exactly one feature: --features softfloat|zfinx|f");
#[cfg(all(feature = "softfloat", any(feature = "zfinx", feature = "f")))]
compile_error!("`softfloat` is mutually exclusive with `zfinx`/`f`");
#[cfg(all(feature = "zfinx", feature = "f"))]
compile_error!("`zfinx` and `f` are mutually exclusive");

use eyre::Context;
use openvm_circuit::arch::hasher::poseidon2::vm_poseidon2_hasher;
use openvm_circuit::arch::vm::{
    verify_segments, ContinuationVmProver, ContinuationVmProof, VirtualMachine, VmInstance,
};
use openvm_circuit::arch::{execution_mode::InstructionInfo, Streams, VmExecutor};
use openvm_instructions::exe::VmExe;
use openvm_rv32im_transpiler::{
    Rv32ITranspilerExtension, Rv32IoTranspilerExtension, Rv32MTranspilerExtension,
};
use openvm_sha2_transpiler::Sha2TranspilerExtension;
use openvm_stark_backend::keygen::types::MultiStarkVerifyingKey;
use openvm_stark_backend::p3_field::PrimeField32;
use openvm_stark_backend::{StarkEngine, SystemParams};
use openvm_stark_sdk::config::baby_bear_poseidon2::{
    BabyBearPoseidon2Config, BabyBearPoseidon2CpuEngine,
};
use openvm_stark_sdk::p3_baby_bear::BabyBear;
use openvm_transpiler::{elf::Elf, openvm_platform::memory::MEM_SIZE, transpiler::Transpiler, FromElf};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::time::Instant;

#[cfg(feature = "softfloat")]
use openvm_sha2_circuit::{Sha2Rv32Config, Sha2Rv32CpuBuilder};

#[cfg(any(feature = "zfinx", feature = "f"))]
mod config;
#[cfg(any(feature = "zfinx", feature = "f"))]
use config::{Rv32FSha2CpuBuilder, Rv32FSha2Config};
#[cfg(any(feature = "zfinx", feature = "f"))]
use openvm_rv32f_transpiler::Rv32FTranspilerExtension;

type F = BabyBear;

#[cfg(feature = "softfloat")]
const DEFAULT_ELF_PATH: &str = "./build/hello";
#[cfg(feature = "zfinx")]
const DEFAULT_ELF_PATH: &str = "./build/hello";
#[cfg(feature = "f")]
const DEFAULT_ELF_PATH: &str = "./build/hello";

const STATE_OUT: &str = "state_openvm.bin";

const PV_PTR_OFF: usize = 0;
const PV_SIZE_OFF: usize = 4;
const PV_HASH_OFF: usize = 8;
const PV_HASH_LEN: usize = 32;
const NUM_PUBLIC_VALUES: usize = 64;

// ---------------------------------------------------------------------------
// Proof artifact
// ---------------------------------------------------------------------------

const PROOF_MAGIC: &[u8; 8] = b"IMPPRF01";

/// The persisted proof artifact: everything needed to verify the execution
/// later, anywhere, without the ELF or the guest. The verifying key pins the
/// circuit (and carries its SystemParams); the continuation proof carries the
/// per-segment proofs plus the user public values (revealed ptr/size/hash)
/// with their Merkle proof against the final memory root.
#[derive(Serialize, Deserialize)]
struct ProofFile {
    magic: [u8; 8],
    vk: MultiStarkVerifyingKey<BabyBearPoseidon2Config>,
    proof: ContinuationVmProof<BabyBearPoseidon2Config>,
}

/// openvm's `air_test` hardcodes SystemParams::new_for_testing(22) whose
/// max_constraint_degree is 4; openvm-floating's fsub binade-boundary fix
/// (rev 115c21e) raises FsubCoreAir to degree 5.
fn vm_system_params() -> SystemParams {
    let mut params = SystemParams::new_for_testing(22);
    params.max_constraint_degree = 5;
    params
}

fn enlarge_public_values(system: &mut SystemConfig2) {
    system.num_public_values = NUM_PUBLIC_VALUES;
    system.memory_config.addr_spaces[3].num_cells = NUM_PUBLIC_VALUES;
}

// Alias to keep the fn signature readable.
use openvm_circuit::arch::SystemConfig as SystemConfig2;

/// Build the VM (keygen) and prove the program; returns the artifact.
fn prove_guest(exe: VmExe<F>) -> eyre::Result<ProofFile> {
    let engine: BabyBearPoseidon2CpuEngine = BabyBearPoseidon2CpuEngine::new(vm_system_params());

    #[cfg(feature = "softfloat")]
    let mut config = Sha2Rv32Config::default();
    #[cfg(feature = "zfinx")]
    let mut config = Rv32FSha2Config::new_zfinx();
    #[cfg(feature = "f")]
    let mut config = Rv32FSha2Config::new_f();
    enlarge_public_values(&mut config.system);

    #[cfg(feature = "softfloat")]
    let builder = Sha2Rv32CpuBuilder;
    #[cfg(any(feature = "zfinx", feature = "f"))]
    let builder = Rv32FSha2CpuBuilder;

    let (vm, pk) = VirtualMachine::new_with_keygen(engine, builder, config)
        .with_context(|| "VM keygen failed")?;
    let vk = pk.get_vk().clone();

    let cached_program_trace = vm.commit_program_on_device(&exe.program);
    let mut instance = VmInstance::new(vm, std::sync::Arc::new(exe), cached_program_trace)
        .with_context(|| "failed to create proving instance")?;

    let proof = instance
        .prove(Streams::<F>::default())
        .with_context(|| "proving failed")?;

    Ok(ProofFile {
        magic: *PROOF_MAGIC,
        vk,
        proof,
    })
}

/// Standalone verification of a proof artifact. Returns the revealed
/// (ptr, size, hash) public values on success.
fn verify_proof_artifact(file: &ProofFile) -> eyre::Result<(u32, u32, [u8; 32])> {
    if &file.magic != PROOF_MAGIC {
        return Err(eyre::eyre!("bad proof artifact magic"));
    }
    // The engine is rebuilt from the params carried in the verifying key, so
    // the artifact is self-describing (no hardcoded system params needed).
    let engine: BabyBearPoseidon2CpuEngine = BabyBearPoseidon2CpuEngine::new(file.vk.inner.params.clone());
    let payload = verify_segments(&engine, &file.vk, &file.proof.per_segment)
        .with_context(|| "segment proof verification failed")?;

    // Memory dimensions must match the guest configuration.
    #[cfg(feature = "softfloat")]
    let mut config = Sha2Rv32Config::default();
    #[cfg(feature = "zfinx")]
    let mut config = Rv32FSha2Config::new_zfinx();
    #[cfg(feature = "f")]
    let mut config = Rv32FSha2Config::new_f();
    enlarge_public_values(&mut config.system);

    file.proof
        .user_public_values
        .verify(
            &vm_poseidon2_hasher(),
            config.system.memory_config.memory_dimensions(),
            payload.final_memory_root,
        )
        .with_context(|| "user public values verification failed")?;

    // Parse the revealed values: each field element carries one BYTE of
    // the public-values space (canonical u32 = byte value); layout matches
    // the execution path: ptr@0, size@4, hash@8..40, little-endian.
    let pvs = &file.proof.user_public_values.public_values;
    if pvs.len() < PV_HASH_OFF + PV_HASH_LEN {
        return Err(eyre::eyre!("public values too short"));
    }
    let bytes: Vec<u8> = pvs.iter().map(|e| e.as_canonical_u32() as u8).collect();
    let ptr = u32::from_le_bytes(bytes[PV_PTR_OFF..PV_PTR_OFF + 4].try_into().unwrap());
    let size = u32::from_le_bytes(bytes[PV_SIZE_OFF..PV_SIZE_OFF + 4].try_into().unwrap());
    let mut hash = [0u8; 32];
    hash.copy_from_slice(&bytes[PV_HASH_OFF..PV_HASH_OFF + PV_HASH_LEN]);
    Ok((ptr, size, hash))
}

// ---------------------------------------------------------------------------

fn main() -> eyre::Result<()> {
    tracing_subscriber::fmt::init();

    let args: Vec<String> = std::env::args().collect();

    // Standalone artifact verification: no ELF, no execution.
    if args.get(1).map(String::as_str) == Some("verify") {
        let proof_path = args
            .get(2)
            .cloned()
            .ok_or_else(|| eyre::eyre!("usage: impulse-runner verify <proof-file> [expected-hash-hex]"))?;
        let expected = args.get(3).cloned();

        #[cfg(feature = "softfloat")]
        println!("[config] softfloat (rv32im)");
        #[cfg(feature = "zfinx")]
        println!("[config] zfinx (rv32im_zfinx)");
        #[cfg(feature = "f")]
        println!("[config] F extension (rv32imf)");
        println!("proof:    {proof_path}");
        let bytes = std::fs::read(&proof_path)
            .with_context(|| format!("failed to read proof at {proof_path}"))?;
        let t = Instant::now();
        let file: ProofFile =
            bincode::deserialize(&bytes).with_context(|| "failed to decode proof artifact")?;
        let (ptr, size, hash) = verify_proof_artifact(&file)?;
        let secs = t.elapsed().as_secs_f64();

        println!("verified:  {secs:.2}s");
        println!(
            "revealed:  ptr {ptr:#010x}, {size} bytes, hash {}",
            hex::encode(hash)
        );
        if let Some(exp) = expected {
            let exp = exp.to_ascii_lowercase();
            let got = hex::encode(hash);
            if got == exp {
                println!("session:   MATCH — proof reproduces the recorded final state hash");
            } else {
                eprintln!(
                    "session:   MISMATCH\n  recorded: {exp}\n  proof:    {got}"
                );
                return Err(eyre::eyre!("proof hash mismatch"));
            }
        }
        println!("✓ proof verified");
        return Ok(());
    }

    let prove = args.iter().any(|a| a == "prove");
    let elf_path = args
        .iter()
        .skip(1)
        .find(|a| *a != "prove")
        .cloned()
        .unwrap_or_else(|| DEFAULT_ELF_PATH.to_string());

    #[cfg(feature = "softfloat")]
    println!("[config] softfloat (rv32im)");
    #[cfg(feature = "zfinx")]
    println!("[config] zfinx (rv32im_zfinx)");
    #[cfg(feature = "f")]
    println!("[config] F extension (rv32imf, FP regs in AS 5)");
    println!("ELF: {elf_path}");

    // 1. Load and decode the ELF.
    let elf_bytes = std::fs::read(&elf_path)
        .with_context(|| format!("failed to read ELF at {elf_path}"))?;
    let elf = Elf::decode(&elf_bytes, MEM_SIZE as u32)
        .with_context(|| "failed to decode ELF")?;

    // 2. Build the transpiler chain (feature-selected).
    let exe = VmExe::from_elf(elf, build_transpiler())
        .with_context(|| "failed to transpile ELF")?;

    // 3. Build config (feature-selected).
    #[cfg(feature = "softfloat")]
    let mut config = Sha2Rv32Config::default();
    #[cfg(feature = "zfinx")]
    let mut config = Rv32FSha2Config::new_zfinx();
    #[cfg(feature = "f")]
    let mut config = Rv32FSha2Config::new_f();
    enlarge_public_values(&mut config.system);

    // 4. Execute. Use profiling hook when PROFILE_FILE is set.
    let executor = VmExecutor::new(config)?;
    let instance = executor.instance(&exe)?;
    let profile_path = std::env::var("PROFILE_FILE").ok();
    let t = Instant::now();
    let state = if let Some(path) = &profile_path {
        let prof = std::sync::Arc::new(std::sync::Mutex::new(
            openvm_profiler::FoldedProfile::new(&elf_bytes)
                .map_err(|e| eyre::eyre!("failed to parse ELF for profiling: {e}"))?,
        ));
        let prof_hook = prof.clone();
        let state = instance
            .execute_with_hook(
                Streams::<F>::default(),
                Box::new(move |info: InstructionInfo| {
                    prof_hook.lock().unwrap().on_instruction(info.pc)
                }),
            )
            .with_context(|| "execution failed")?;
        let prof = prof.lock().unwrap();
        let mut f = std::fs::File::create(path)
            .with_context(|| format!("failed to create profile file {path}"))?;
        prof.write(&mut f)
            .map_err(|e| eyre::eyre!("failed to write folded profile: {e}"))?;
        println!("profile:    wrote {path}");
        state
    } else {
        instance
            .execute(Streams::<F>::default(), None)
            .with_context(|| "execution failed")?
    };
    let exec_secs = t.elapsed().as_secs_f64();
    println!("execution:   {exec_secs:.2}s");

    // 5. Read public values: ptr, size, hash.
    let pvs: &[u8] =
        unsafe { state.memory.get_slice::<u8>(3, 0, NUM_PUBLIC_VALUES) };
    let ptr = u32::from_le_bytes(
        pvs[PV_PTR_OFF..PV_PTR_OFF + 4].try_into().expect("ptr"),
    );
    let size = u32::from_le_bytes(
        pvs[PV_SIZE_OFF..PV_SIZE_OFF + 4].try_into().expect("size"),
    );
    let revealed_hash = &pvs[PV_HASH_OFF..PV_HASH_OFF + PV_HASH_LEN];

    println!(
        "guest:      state blob at AS2 {ptr:#010x}, {size} bytes, hash {}",
        hex::encode(revealed_hash)
    );

    // 6. Read the state blob bytes out of AS 2.
    if size == 0 || ptr == 0 {
        return Err(eyre::eyre!(
            "guest did not populate public values (ptr={ptr}, size={size})"
        ));
    }
    let blob: &[u8] =
        unsafe { state.memory.get_slice::<u8>(2, ptr, size as usize) };

    std::fs::write(STATE_OUT, blob)
        .with_context(|| format!("failed to write {STATE_OUT}"))?;
    println!("wrote:      {STATE_OUT} ({size} bytes)");

    // 7. Verify host SHA-256 matches the revealed hash.
    let mut hasher = Sha256::new();
    hasher.update(blob);
    let computed = hasher.finalize();
    if computed.as_slice() == revealed_hash {
        println!("sha256:     matches");
    } else {
        eprintln!(
            "sha256:     MISMATCH\n  revealed: {}\n  computed: {}",
            hex::encode(revealed_hash),
            hex::encode(computed),
        );
        return Err(eyre::eyre!("hash mismatch"));
    }

    // 8. Prove, persist the artifact, and verify it via the standalone path.
    if prove {
        println!("\n=== Proving (this may take a while) ===");
        let t = Instant::now();

        let file = prove_guest(exe.clone())?;
        let prove_secs = t.elapsed().as_secs_f64();

        let artifact_path = format!("{elf_path}.proof");
        let bytes = bincode::serialize(&file)
            .with_context(|| "failed to serialize proof artifact")?;
        std::fs::write(&artifact_path, &bytes)
            .with_context(|| format!("failed to write {artifact_path}"))?;
        println!(
            "artifact:   {artifact_path} ({} bytes, {} segment proof(s))",
            bytes.len(),
            file.proof.per_segment.len()
        );

        // Verify through the exact standalone path a third party would use.
        let t = Instant::now();
        let reloaded: ProofFile = bincode::deserialize(&bytes)
            .with_context(|| "failed to reload proof artifact")?;
        let (v_ptr, v_size, v_hash) = verify_proof_artifact(&reloaded)?;
        let verify_secs = t.elapsed().as_secs_f64();
        if v_ptr != ptr || v_size != size || v_hash[..] != revealed_hash[..] {
            return Err(eyre::eyre!(
                "standalone verification public values differ from execution"
            ));
        }

        // Read peak memory from /proc/self/status
        let vm_peak_kb: u64 = std::fs::read_to_string("/proc/self/status")
            .ok()
            .and_then(|s| {
                s.lines()
                    .find(|l| l.starts_with("VmPeak:"))
                    .and_then(|l| l.split_whitespace().nth(1).and_then(|n| n.parse().ok()))
            })
            .unwrap_or(0);
        let vm_peak_gb = vm_peak_kb as f64 / 1_048_576.0;
        println!(
            "prove:      {prove_secs:.2}s (peak memory: {vm_peak_gb:.1} GB)"
        );
        println!("reverify:   {verify_secs:.2}s (standalone path, artifact reloaded)");
        println!("✓ proof verified and persisted: {artifact_path}");
        println!(
            "  anyone can check it later: impulse-runner verify {artifact_path}"
        );
    }

    Ok(())
}

/// Build the transpiler with extensions appropriate for the active feature.
#[cfg(feature = "softfloat")]
fn build_transpiler() -> Transpiler<F> {
    Transpiler::<F>::default()
        .with_extension(Rv32ITranspilerExtension)
        .with_extension(Rv32MTranspilerExtension)
        .with_extension(Rv32IoTranspilerExtension)
        .with_extension(Sha2TranspilerExtension)
}

#[cfg(feature = "zfinx")]
fn build_transpiler() -> Transpiler<F> {
    Transpiler::<F>::default()
        .with_extension(Rv32ITranspilerExtension)
        .with_extension(Rv32MTranspilerExtension)
        .with_extension(Rv32IoTranspilerExtension)
        .with_extension(Rv32FTranspilerExtension::default())
        .with_extension(Sha2TranspilerExtension)
}

#[cfg(feature = "f")]
fn build_transpiler() -> Transpiler<F> {
    Transpiler::<F>::default()
        .with_extension(Rv32ITranspilerExtension)
        .with_extension(Rv32MTranspilerExtension)
        .with_extension(Rv32IoTranspilerExtension)
        .with_extension(Rv32FTranspilerExtension::new_f())
        .with_extension(Sha2TranspilerExtension)
}
