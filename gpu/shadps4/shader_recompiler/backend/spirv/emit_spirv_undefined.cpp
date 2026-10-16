// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

// A register read on a path where it was never written has no defined value, and each driver gives it a
// different one. Handing back zero everywhere makes the result the same on every host.
Id EmitUndefU1(EmitContext& ctx) {
    return ctx.false_value;
}

Id EmitUndefU8(EmitContext&) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitUndefU16(EmitContext&) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitUndefU32(EmitContext& ctx) {
    return ctx.u32_zero_value;
}

Id EmitUndefU32x2(EmitContext& ctx) {
    return ctx.ConstantNull(ctx.U32[2]);
}

Id EmitUndefU32x3(EmitContext& ctx) {
    return ctx.ConstantNull(ctx.U32[3]);
}

Id EmitUndefU32x4(EmitContext& ctx) {
    return ctx.ConstantNull(ctx.U32[4]);
}

Id EmitUndefF32(EmitContext& ctx) {
    return ctx.f32_zero_value;
}

Id EmitUndefU64(EmitContext&) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

} // namespace Shader::Backend::SPIRV
