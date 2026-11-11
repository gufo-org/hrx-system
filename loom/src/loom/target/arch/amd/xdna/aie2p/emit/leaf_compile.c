// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/emit/leaf_compile.h"

#include <stdio.h>
#include <stdlib.h>

#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/frame.h"
#include "loom/codegen/low/function_model.h"
#include "loom/codegen/low/packet.h"
#include "loom/codegen/low/representation_binding.h"
#include "loom/error/error_catalog.h"
#include "loom/ops/low/ops.h"
#include "loom/target/arch/amd/xdna/aie2p/descriptors/low_registry.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/bundle_plan.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/program_capacity.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/spill.h"
#include "loom/target/arch/amd/xdna/aie2p/emit/stack_frame.h"
#include "loom/target/reporting/low.h"

// LOOM_AIE2P_SCHEDULE_STRATEGY=<0..3> fixes one strategy (diagnostics only);
// otherwise leaves compare two strategies and keep the faster plan.
static bool loom_aie2p_leaf_forced_strategy(
    loom_low_schedule_strategy_t* out_strategy) {
  const char* value = getenv("LOOM_AIE2P_SCHEDULE_STRATEGY");
  if (value && value[0] >= '0' && value[0] <= '3' && !value[1]) {
    *out_strategy = (loom_low_schedule_strategy_t)(value[0] - '0');
    return true;
  }
  return false;
}

// Uses the scheduler's retained call inventory. A call-bearing frame is
// admitted only when program planning supplied the complete direct-call ABI.
static iree_status_t loom_aie2p_leaf_admit_calls(
    void* user_data, const loom_low_emission_frame_t* frame,
    iree_arena_allocator_t* arena, bool* out_accepted) {
  (void)arena;
  const loom_aie2p_leaf_compile_options_t* options = user_data;
  *out_accepted = frame->schedule.call_node_count == 0 ||
                  options->call_contracts.query != NULL;
  if (*out_accepted) {
    return iree_ok_status();
  }
  const loom_op_t* op =
      frame->schedule.nodes[frame->schedule.call_node_indices[0]].op;
  const loom_diagnostic_param_t params[] = {
      loom_param_string(loom_low_diagnostic_target_key(&frame->target)),
      loom_param_string(loom_low_diagnostic_export_name(&frame->target)),
      loom_param_string(loom_low_diagnostic_config_key(&frame->target)),
      loom_param_string(loom_low_diagnostic_symbol_name(
          frame->module, loom_low_func_def_callee(frame->function_op))),
      loom_param_string(loom_low_diagnostic_operation_name(frame->module, op)),
      loom_param_with_field_ref(
          loom_param_string(loom_low_diagnostic_symbol_name(
              frame->module, loom_low_func_call_callee(op))),
          loom_low_func_call_callee_diagnostic_ref()),
      loom_param_string(IREE_SV(
          "the selected target has no native ABI contract for this call")),
  };
  const loom_diagnostic_emission_t emission = {
      .op = op,
      .error = LOOM_ERR_TARGET_072,
      .params = params,
      .param_count = IREE_ARRAYSIZE(params),
  };
  return iree_diagnostic_emit(options->diagnostic_emitter, &emission);
}

static iree_status_t loom_aie2p_leaf_build_frame(
    loom_module_t* module, loom_op_t* function_op,
    const loom_aie2p_leaf_compile_options_t* options,
    loom_low_schedule_strategy_t strategy,
    loom_aie2p_stack_layout_t* stack_layout, iree_arena_allocator_t* arena,
    loom_low_emission_frame_t* out_frame, bool* out_accepted) {
  loom_low_planning_statistics_t planning_statistics = {0};
  const loom_low_emission_frame_options_t frame_options = {
      .descriptor_registry = options->descriptor_registry,
      .function_target_facts = options->function_target_facts,
      .allocation_fixed_values = options->allocation_fixed_values,
      .allocation_fixed_value_count = options->allocation_fixed_value_count,
      .allocation_entry_locations = options->allocation_entry_locations,
      .allocation_entry_location_count =
          options->allocation_entry_location_count,
      .allocation_exit_locations = options->allocation_exit_locations,
      .allocation_exit_location_count = options->allocation_exit_location_count,
      .call_contracts = options->call_contracts,
      .allocation_reserved_ranges = options->allocation_reserved_ranges,
      .allocation_reserved_range_count =
          options->allocation_reserved_range_count,
      .allocation_diagnostic_flags = options->allocation_diagnostic_flags,
      .memory_accesses = options->memory_accesses,
      .loop_execution = options->loop_execution,
      .schedule_structural_models = loom_aie2p_low_structural_schedule_models(),
      .schedule_strategy = strategy,
      .schedule_flags = LOOM_LOW_SCHEDULE_FLAG_RETAIN_DEPENDENCY_INDEX |
                        loom_target_compile_report_required_schedule_flags(
                            options->compile_report),
      .emitter = options->diagnostic_emitter,
      .statistics =
          options->compile_report != NULL ? &planning_statistics : NULL,
  };
  const loom_low_emission_frame_spill_free_options_t spill_free_options = {
      .validate_frame = loom_aie2p_leaf_admit_calls,
      .validate_frame_user_data = (void*)options,
      .materialization_options =
          {
              .has_supported_storage_spaces = true,
              .supported_storage_spaces = LOOM_LOW_STORAGE_SPACE_SET_STACK,
              .plan_traffic = loom_aie2p_spill_plan,
              .plan_traffic_user_data = stack_layout,
              .record_materialized_spills =
                  loom_target_compile_report_wants_details(
                      options->compile_report,
                      LOOM_TARGET_COMPILE_REPORT_DETAIL_SPILL_ROWS),
              .emitter = options->diagnostic_emitter,
          },
  };
  iree_status_t status = loom_low_emission_frame_build_spill_free(
      module, function_op, &frame_options, &spill_free_options, arena,
      out_frame, out_accepted);
  if (options->compile_report != NULL) {
    loom_target_compile_report_record_low_planning(options->compile_report,
                                                   &planning_statistics);
  }
  if (iree_status_is_ok(status) && options->compile_report != NULL) {
    if (*out_accepted) {
      const loom_target_bundle_t* bundle =
          loom_low_resolved_target_bundle(&out_frame->target);
      if (bundle != NULL) {
        loom_target_compile_report_record_target_bundle(options->compile_report,
                                                        bundle);
      }
      status = loom_target_compile_report_record_low_emission_frame(
          options->compile_report, out_frame);
    } else if (out_frame->allocation.function_op != NULL) {
      status = loom_target_compile_report_record_low_allocation(
          options->compile_report, &out_frame->allocation);
    }
  }
  return status;
}

// One complete schedule/allocation/bundle planning attempt. Each attempt owns
// its scratch and stack arenas; the structure must not move once started.
typedef struct loom_aie2p_leaf_attempt_t {
  iree_arena_allocator_t scratch_arena;
  iree_arena_allocator_t stack_arena;
  loom_aie2p_stack_layout_t stack_layout;
  loom_low_emission_frame_t frame;
  loom_aie2p_leaf_program_plan_t plan;
  bool planned;
} loom_aie2p_leaf_attempt_t;

static iree_status_t loom_aie2p_leaf_attempt_run(
    loom_module_t* module, loom_op_t* function_op,
    const loom_aie2p_leaf_compile_options_t* options,
    loom_low_schedule_strategy_t strategy, iree_arena_block_pool_t* block_pool,
    loom_aie2p_leaf_attempt_t* attempt) {
  iree_arena_initialize(block_pool, &attempt->scratch_arena);
  iree_arena_initialize(block_pool, &attempt->stack_arena);
  attempt->stack_layout =
      (loom_aie2p_stack_layout_t){.arena = &attempt->stack_arena};
  attempt->frame = (loom_low_emission_frame_t){0};
  attempt->plan = (loom_aie2p_leaf_program_plan_t){0};
  attempt->planned = false;
  bool frame_accepted = false;
  iree_status_t status = loom_aie2p_leaf_build_frame(
      module, function_op, options, strategy, &attempt->stack_layout,
      &attempt->scratch_arena, &attempt->frame, &frame_accepted);
  loom_aie2p_stack_frame_t stack = {0};
  bool stack_accepted = false;
  if (iree_status_is_ok(status) && frame_accepted) {
    status = loom_aie2p_stack_frame_build(
        module, function_op,
        &attempt->frame.schedule.requirements.storage_layout,
        &attempt->stack_layout, options->callee_stack_byte_length,
        options->is_helper, options->diagnostic_emitter, &stack_accepted,
        &stack);
  }
  if (iree_status_is_ok(status) && stack_accepted) {
    const loom_aie2p_bundle_plan_options_t plan_options = {
        .stack = stack,
        .diagnostic_emitter = options->diagnostic_emitter,
        .preserve_link_register = options->preserve_link_register,
        .link_shadow_register = options->link_shadow_register,
    };
    status = loom_aie2p_bundle_plan_build(&attempt->frame, &plan_options,
                                          &attempt->scratch_arena,
                                          &attempt->planned, &attempt->plan);
  }
  return status;
}

static void loom_aie2p_leaf_attempt_deinitialize(
    loom_aie2p_leaf_attempt_t* attempt) {
  iree_arena_deinitialize(&attempt->stack_arena);
  iree_arena_deinitialize(&attempt->scratch_arena);
}

// LOOM_AIE2P_DUMP_BUNDLES=1 prints every planned bundle (issue cycle, Low
// block, slot mnemonics) to stderr: the only view of the final VLIW schedule.
// True when the verified Low function contains a matrix (mma.*) packet. Reads
// the IR only; planning has not started.
static iree_status_t loom_aie2p_leaf_function_has_matrix(
    loom_module_t* module, const loom_op_t* function_op,
    const loom_aie2p_leaf_compile_options_t* options,
    iree_arena_block_pool_t* block_pool, bool* out_has_matrix) {
  *out_has_matrix = false;
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);
  loom_low_function_context_t context = {0};
  iree_status_t status = loom_low_function_context_initialize(
      module, function_op, NULL, options->function_target_facts,
      options->descriptor_registry, (iree_diagnostic_emitter_t){0}, 0, 0,
      &arena, &context);
  if (iree_status_is_ok(status) && context.body != NULL &&
      context.target.descriptor_set != NULL) {
    const loom_low_descriptor_set_t* descriptor_set =
        context.target.descriptor_set;
    for (uint16_t b = 0; b < context.body->block_count && !*out_has_matrix;
         ++b) {
      const loom_block_t* block = context.body->blocks[b];
      for (const loom_op_t* op = block->first_op; op != NULL;
           op = op->next_op) {
        loom_low_descriptor_packet_t packet;
        loom_low_descriptor_packet_initialize(descriptor_set, op, &packet);
        if (packet.descriptor == NULL) {
          continue;
        }
        const iree_string_view_t name = loom_low_descriptor_set_string(
            descriptor_set, packet.descriptor->mnemonic_string_ref);
        if (iree_string_view_starts_with(name, IREE_SV("mma."))) {
          *out_has_matrix = true;
          break;
        }
      }
    }
  }
  loom_low_function_context_deinitialize(&context);
  iree_arena_deinitialize(&arena);
  return status;
}

static void loom_aie2p_leaf_dump_bundles(
    const loom_aie2p_leaf_attempt_t* attempt) {
  {
    const loom_low_descriptor_set_t* descriptor_set =
        attempt->frame.target.descriptor_set;
    fprintf(stderr, "AIE2P bundles %.*s: %zu bundles, %u cycles, %zu bytes\n",
            (int)attempt->plan.function_name.size,
            attempt->plan.function_name.data,
            (size_t)attempt->plan.bundle_count, attempt->plan.issue_cycle_count,
            (size_t)attempt->plan.encoded_byte_length);
    for (iree_host_size_t i = 0; i < attempt->plan.bundle_count; ++i) {
      const loom_aie2p_planned_bundle_t* bundle = &attempt->plan.bundles[i];
      fprintf(stderr, "  c%u l%u b%u:", bundle->issue_cycle,
              bundle->logical_issue_cycle, bundle->block_index);
      for (uint8_t j = 0; j < bundle->slot_count; ++j) {
        const loom_aie2p_planned_slot_t* slot =
            &attempt->plan.slots[bundle->slot_start + j];
        if (slot->scheduled_packet_index == UINT32_MAX) {
          fprintf(stderr, " <planner>");
          continue;
        }
        const loom_low_packet_view_t packet = loom_low_packet_at(
            &attempt->frame.schedule, slot->scheduled_packet_index);
        iree_string_view_t name = IREE_SV("<structural>");
        if (packet.descriptor != NULL) {
          name = loom_low_descriptor_set_string(
              descriptor_set, packet.descriptor->mnemonic_string_ref);
        }
        fprintf(stderr, " %.*s#%u", (int)name.size, name.data,
                slot->scheduled_packet_index);
      }
      fprintf(stderr, "\n");
    }
  }
}

iree_status_t loom_aie2p_leaf_compile(
    loom_module_t* module, loom_op_t* function_op,
    const loom_aie2p_leaf_compile_options_t* options,
    iree_arena_allocator_t* arena, bool* out_compiled,
    loom_aie2p_leaf_contribution_t* out_contribution) {
  *out_compiled = false;
  *out_contribution = (loom_aie2p_leaf_contribution_t){0};
  // The contribution and optional report retain no frame scratch. Schedule,
  // allocation, and packet-planning storage is reusable by the next worker.
  loom_low_schedule_strategy_t strategy =
      LOOM_LOW_SCHEDULE_STRATEGY_RESOURCE_STALL;
  const bool forced = loom_aie2p_leaf_forced_strategy(&strategy);
  // Resource-stall scoring serializes independent element-wise chains that
  // source order interleaves (SwiGLU 1.5x faster in source order) but wins on
  // matrix loops (5x). Planning rewrites the function, so the strategy is
  // chosen once, before planning, from the leaf's matrix content.
  if (!forced) {
    bool has_matrix = true;
    iree_status_t scan_status = loom_aie2p_leaf_function_has_matrix(
        module, function_op, options, arena->block_pool, &has_matrix);
    if (!iree_status_is_ok(scan_status)) {
      has_matrix = true;
    }
    iree_status_ignore(scan_status);
    if (!has_matrix) {
      strategy = LOOM_LOW_SCHEDULE_STRATEGY_SOURCE_PRIORITY;
    }
  }
  loom_aie2p_leaf_attempt_t attempts[1];
  loom_aie2p_leaf_attempt_t* attempt = &attempts[0];
  iree_status_t status = loom_aie2p_leaf_attempt_run(
      module, function_op, options, strategy, arena->block_pool, attempt);
  const bool program_planned = attempt->planned;
  if (iree_status_is_ok(status) && program_planned &&
      getenv("LOOM_AIE2P_DUMP_BUNDLES")) {
    loom_aie2p_leaf_dump_bundles(attempt);
  }
  loom_aie2p_leaf_program_plan_t program_plan = attempt->plan;
  if (iree_status_is_ok(status) && program_planned &&
      options->compile_report != NULL) {
    loom_target_compile_report_emission_breakdown_t emission_breakdown = {
        .body_instruction_count = program_plan.issue_cycle_count,
    };
    for (iree_host_size_t i = 0; i < program_plan.bundle_count; ++i) {
      const uint8_t slot_count = program_plan.bundles[i].slot_count;
      if (slot_count <= 1) {
        continue;
      }
      ++emission_breakdown.coissued_instruction_count;
      emission_breakdown.coissued_component_count += slot_count;
    }
    loom_target_compile_report_record_emission(
        options->compile_report, program_plan.issue_cycle_count,
        program_plan.encoded_byte_length, program_plan.encoded_byte_length);
    loom_target_compile_report_record_emission_breakdown(
        options->compile_report, &emission_breakdown);
  }
  bool capacity_admitted = false;
  if (iree_status_is_ok(status) && program_planned) {
    status = loom_aie2p_program_capacity_admit(
        module, function_op, program_plan.encoded_byte_length,
        options->diagnostic_emitter, &capacity_admitted);
  }
  if (iree_status_is_ok(status) && program_planned && capacity_admitted) {
    status =
        loom_aie2p_leaf_object_emit(&program_plan, arena, out_contribution);
  }
  if (iree_status_is_ok(status) && program_planned && capacity_admitted) {
    *out_compiled = true;
  }
  loom_aie2p_leaf_attempt_deinitialize(&attempts[0]);
  return status;
}
