/*
 * INTEL CONFIDENTIAL
 * Copyright (c) 2015, Intel Corporation All Rights Reserved.
 *
 * The source code contained or described herein and all documents related to the
 * source code ("Material") are owned by Intel Corporation or its suppliers or
 * licensors. Title to the Material remains with Intel Corporation or its suppliers
 * and licensors. The Material contains trade secrets and proprietary and
 * confidential information of Intel or its suppliers and licensors. The Material
 * is protected by worldwide copyright and trade secret laws and treaty provisions.
 * No part of the Material may be used, copied, reproduced, modified, published,
 * uploaded, posted, transmitted, distributed, or disclosed in any way without
 * Intel's prior express written permission.
 *
 * No license under any patent, copyright, trade secret or other intellectual
 * property right is granted to or conferred upon you by disclosure or delivery of
 * the Materials, either expressly, by implication, inducement, estoppel or
 * otherwise. Any license under such intellectual property rights must be express
 * and approved by Intel in writing.
 */

#include "graph_x86.h"

#include "builder.h"
#include "ext_utility.h"
#include "loop_iterators.h"
#include "pretty_printer.h"
#include "ssa_builder.h"
#include "scoped_thread_state_change.h"

namespace art {

void HGraph_X86::Dump() {
  for (HBasicBlock* block : blocks_) {
    LOG(INFO) << "Block " << block->GetBlockId() << " has LoopInformation " << block->GetLoopInformation();
  }

  StringPrettyPrinter printer(this);
  printer.VisitInsertionOrder();
  const std::string& print_string = printer.str();

  LOG(INFO) << print_string;
}

void HGraph_X86::DeleteBlock(HBasicBlock* block) {
  // Remove all Phis.
  for (HInstructionIterator it2(block->GetPhis()); !it2.Done(); it2.Advance()) {
    HInstruction* insn = it2.Current();
    RemoveAsUser(insn);
    RemoveFromEnvironmentUsers(insn);
    block->RemovePhi(insn->AsPhi(), false);
  }

  // Remove the rest of the instructions.
  for (HInstructionIterator it2(block->GetInstructions()); !it2.Done(); it2.Advance()) {
    HInstruction* insn = it2.Current();
    RemoveAsUser(insn);
    RemoveFromEnvironmentUsers(insn);
    block->RemoveInstruction(insn, false);
  }

  // Remove all successors from the block.
  const ArenaVector<HBasicBlock*>& successors = block->GetSuccessors();
  for (size_t j = successors.size(); j > 0; j--) {
    HBasicBlock* successor = successors[j - 1];
    if (std::find(successor->GetPredecessors().begin(),
                  successor->GetPredecessors().end(),
                  block) != successor->GetPredecessors().end()) {
      successor->RemovePredecessor(block);
    }
    block->RemoveSuccessor(successor);
  }

  // Remove all predecessors.
  block->ClearAllPredecessors();

  // Remove all data structures pointing to the block.
  blocks_[block->GetBlockId()] = nullptr;
  RemoveElement(reverse_post_order_, block);
  if (linear_order_.size() > 0) {
    RemoveElement(linear_order_, block);
  }
}

void HGraph_X86::CreateLinkBetweenBlocks(HBasicBlock* existing_block,
                                         HBasicBlock* block_being_added,
                                         bool add_as_dominator,
                                         bool add_after) {
  if (add_after) {
    existing_block->AddSuccessor(block_being_added);
  } else {
    block_being_added->AddSuccessor(existing_block);
  }

  if (add_as_dominator) {
    if (add_after) {
      // Fix the domination information.
      block_being_added->SetDominator(existing_block);
      existing_block->AddDominatedBlock(block_being_added);
    } else {
      // Fix the domination information.
      existing_block->SetDominator(block_being_added);
      block_being_added->AddDominatedBlock(existing_block);
    }
  }

  // Fix reverse post ordering.
  size_t index = IndexOfElement(reverse_post_order_, existing_block);
  MakeRoomFor(&reverse_post_order_, 1, index);
  if (add_after) {
    reverse_post_order_[index + 1] = block_being_added;
  } else {
    reverse_post_order_[index] = block_being_added;
    reverse_post_order_[index + 1] = existing_block;
  }
}

void HGraph_X86::SplitCriticalEdgeAndUpdateLoopInformation(HBasicBlock* from, HBasicBlock* to) {
  // Remember index, to find a new added splitter.
  size_t index = to->GetPredecessorIndexOf(from);

  // First split.
  SplitCriticalEdge(from, to);

  // Find splitter.
  HBasicBlock* splitter = to->GetPredecessors()[index];

  // Set loop information for splitter.
  HLoopInformation* loop_information = to->IsLoopHeader() ?
          from->GetLoopInformation() : to->GetLoopInformation();
  if (loop_information != nullptr) {
    LOOPINFO_TO_LOOPINFO_X86(loop_information)->AddToAll(splitter);
  }
}

void HGraph_X86::RebuildDomination() {
  ClearDominanceInformation();
  ComputeDominanceInformation();
}

void HGraph_X86::MovePhi(HPhi* phi, HBasicBlock* to_block) {
  DCHECK(phi != nullptr);
  HBasicBlock* from_block = phi->GetBlock();
  if (from_block != to_block) {
    from_block->phis_.RemoveInstruction(phi);
    to_block->phis_.AddInstruction(phi);
    phi->SetBlock(to_block);
  }
}

void HGraph_X86::MoveInstructionBefore(HInstruction* instr, HInstruction* cursor) {
  DCHECK(instr != nullptr);
  HBasicBlock* from_block = instr->GetBlock();
  DCHECK(cursor != nullptr);
  DCHECK(!cursor->IsPhi());
  HBasicBlock* to_block = cursor->GetBlock();
  DCHECK(from_block != to_block);

  // Disconnect from the old block.
  from_block->RemoveInstruction(instr, false);

  // Connect up to the new block.
  DCHECK_NE(instr->GetId(), -1);
  DCHECK_NE(cursor->GetId(), -1);
  DCHECK(!instr->IsControlFlow());
  instr->SetBlock(to_block);
  to_block->instructions_.InsertInstructionBefore(instr, cursor);
}

HGraph_X86* CreateX86CFG(ArenaAllocator* allocator,
                         const uint16_t* data,
                         Primitive::Type return_type) {
  DexFile* df = reinterpret_cast<DexFile*>(allocator->Alloc(sizeof(DexFile)));
  HGraph_X86* graph = new (allocator) HGraph_X86(allocator, *df, -1, false, kRuntimeISA);
  if (data != nullptr) {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScopeCollection handles(soa.Self());
    const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
    HGraphBuilder builder(graph, *item, &handles, return_type);
    bool graph_built = (builder.BuildGraph() == kAnalysisSuccess);
    return graph_built ? graph : nullptr;
  }
  return graph;
}

}  // namespace art