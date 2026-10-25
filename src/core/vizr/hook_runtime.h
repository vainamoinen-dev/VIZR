// SPDX-FileCopyrightText: Copyright 2026 VIZR contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Hook runtime: loads the game profile, installs JIT host callbacks at guest
// PCs, reads and writes profile fields at those hooks, and feeds the pose ledger.

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}
namespace Core::Memory {
class Memory;
}
namespace Loader {
class AppLoader;
}

namespace VIZR::Hooks {

/// Guest register access at a hook (the JIT state at block entry).
struct RegReader {
    const void* ctx;
    u64 (*x)(const void* ctx, u32 index);
    u64 (*sp)(const void* ctx);
    /// Register writes before the hooked instruction runs (the hook is at a block start,
    /// so the block reads them from the JIT state). set_s writes the low 32 bits of V<index> (s<index>).
    void (*set_x)(const void* ctx, u32 index, u64 value);
    void (*set_s)(const void* ctx, u32 index, u32 bits);
};

/// dynarmic A64::UserConfig::is_hook_pc; `process` (hook_ctx) is the JIT's KProcess. Only the
/// application process with an installed profile matches. Cheap when no profile is installed.
bool IsHookPc(void* process, u64 pc);

/// From DynarmicCallbacks64::ExceptionRaised(pc, UserHook), on the guest core thread, before the
/// instruction at `pc` executes. Ignored for any process but the application. Never throws.
void OnHook(const void* process, u64 pc, const RegReader& regs, Core::Memory::Memory& memory);

/// After the application process is loaded, before its threads run. Loads the profile for the
/// running build (none = stock behaviour), checks every hook and installs them all or none.
void Install(Core::System& system, Loader::AppLoader& loader);

/// After the guest cores stopped (shutdown or before the next launch).
void Shutdown();

} // namespace VIZR::Hooks
