//! Combined VM configuration: Rv32F (from openvm-floating) + SHA-2 (from
//! OpenVM core). Defined here in the runner rather than in openvm-floating
//! to keep openvm-floating a pure F-extension crate.
//!
//! Only compiled when the `zfinx` or `f` feature is enabled. The softfloat
//! config uses `Sha2Rv32Config` from openvm-sha2-circuit directly.

#![allow(unexpected_cfgs)]

use openvm_circuit::arch::{
    AirInventory, ChipInventoryError, InitFileGenerator, MatrixRecordArena, SystemConfig, VmBuilder,
    VmChipComplex, VmField, VmProverExtension,
};
use openvm_circuit::system::{SystemChipInventory, SystemCpuBuilder, SystemExecutor};
use openvm_circuit_derive::VmConfig;
use openvm_cpu_backend::{CpuBackend, CpuDevice};
use openvm_rv32f_circuit::{Rv32F, Rv32FCpuProverExt, Rv32FExecutor};
use openvm_rv32im_circuit::{
    Rv32I, Rv32IExecutor, Rv32ImCpuProverExt, Rv32Io, Rv32IoExecutor, Rv32M, Rv32MExecutor,
};
use openvm_sha2_circuit::{Sha2, Sha2CpuProverExt, Sha2Executor};
use openvm_stark_backend::{StarkEngine, StarkProtocolConfig, Val};
use serde::{Deserialize, Serialize};

/// VM config combining RV32I + RV32M + RV32Io + RV32F + SHA-2.
#[derive(Clone, Debug, VmConfig, Serialize, Deserialize)]
pub struct Rv32FSha2Config {
    #[config(executor = "SystemExecutor<F>")]
    pub system: SystemConfig,
    #[extension]
    pub base: Rv32I,
    #[extension]
    pub io: Rv32Io,
    #[extension]
    pub mul: Rv32M,
    #[extension]
    pub f: Rv32F,
    #[extension]
    pub sha2: Sha2,
}

impl InitFileGenerator for Rv32FSha2Config {}

impl Rv32FSha2Config {
    /// Construct for Zfinx mode (floats in integer registers, AS 1).
    #[cfg(feature = "zfinx")]
    pub fn new_zfinx() -> Self {
        Self {
            // openvm-floating's fsub binade-boundary fix (rev 115c21e)
            // raises FsubCoreAir's max constraint degree to 5; the
            // default budget is lower and keygen rejects it.
            system: SystemConfig::default().with_max_constraint_degree(5),
            base: Default::default(),
            io: Default::default(),
            mul: Default::default(),
            f: Rv32F::default(),
            sha2: Sha2,
        }
    }

    /// Construct for F-extension mode (separate FP register file, AS 5).
    #[cfg(feature = "f")]
    pub fn new_f() -> Self {
        use openvm_circuit::arch::MemoryCellType;
        let f = Rv32F {
            fp_register_as: 5,
            ..Default::default()
        };
        let mut system = SystemConfig::default();
        system.memory_config.addr_spaces[5].num_cells = 32 * 4;
        system.memory_config.addr_spaces[5].layout = MemoryCellType::U8;
        Self {
            system,
            base: Default::default(),
            io: Default::default(),
            mul: Default::default(),
            f,
            sha2: Sha2,
        }
    }
}

/// CPU builder for Rv32FSha2Config — chains system + base + io + mul + F + SHA-2
/// prover extensions. Used by `air_test` for proving.
#[derive(Clone)]
pub struct Rv32FSha2CpuBuilder;

impl<SC, E> VmBuilder<E> for Rv32FSha2CpuBuilder
where
    SC: StarkProtocolConfig,
    E: StarkEngine<SC = SC, PB = CpuBackend<SC>, PD = CpuDevice<SC>>,
    Val<SC>: VmField,
    SC::EF: Ord,
{
    type VmConfig = Rv32FSha2Config;
    type SystemChipInventory = SystemChipInventory<SC>;
    type RecordArena = MatrixRecordArena<Val<SC>>;

    fn create_chip_complex(
        &self,
        config: &Rv32FSha2Config,
        circuit: AirInventory<SC>,
        device_ctx: &openvm_stark_backend::EngineDeviceCtx<E>,
    ) -> Result<
        VmChipComplex<SC, Self::RecordArena, E::PB, Self::SystemChipInventory>,
        ChipInventoryError,
    > {
        let mut chip_complex = VmBuilder::<E>::create_chip_complex(
            &SystemCpuBuilder,
            &config.system,
            circuit,
            device_ctx,
        )?;
        let inventory = &mut chip_complex.inventory;
        VmProverExtension::<E, _, _>::extend_prover(&Rv32ImCpuProverExt, &config.base, inventory)?;
        VmProverExtension::<E, _, _>::extend_prover(&Rv32ImCpuProverExt, &config.io, inventory)?;
        VmProverExtension::<E, _, _>::extend_prover(&Rv32ImCpuProverExt, &config.mul, inventory)?;
        VmProverExtension::<E, _, _>::extend_prover(&Rv32FCpuProverExt, &config.f, inventory)?;
        VmProverExtension::<E, _, _>::extend_prover(&Sha2CpuProverExt, &config.sha2, inventory)?;
        Ok(chip_complex)
    }
}
