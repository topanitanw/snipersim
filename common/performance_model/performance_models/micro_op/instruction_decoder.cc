#include "instruction_decoder.h"
#include "instruction.h"
#include "micro_op.h"

extern "C" {
#include <xed-reg-class.h>
}


void InstructionDecoder::addSrcs(std::set<xed_reg_enum_t> regs, MicroOp * currentMicroOp) {
   for(std::set<xed_reg_enum_t>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (*it != XED_REG_INVALID) {
         xed_reg_enum_t reg = xed_get_largest_enclosing_register(*it);
         if (reg == XED_REG_EIP || reg == XED_REG_RIP) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addSourceRegister((uint32_t)reg, String(xed_reg_enum_t2str(reg)));
      }
}

void InstructionDecoder::addDsts(std::set<xed_reg_enum_t> regs, MicroOp * currentMicroOp) {
   for(std::set<xed_reg_enum_t>::iterator it = regs.begin(); it != regs.end(); ++it)
      if (*it != XED_REG_INVALID) {
         xed_reg_enum_t reg = xed_get_largest_enclosing_register(*it);
         if (reg == XED_REG_EIP || reg == XED_REG_RIP) continue; // eip/rip is known at decode time, shouldn't be a dependency
         currentMicroOp->addDestinationRegister((uint32_t)reg, String(xed_reg_enum_t2str(reg)));
      }
}


  //////////////////////////////////////////
 ///// IMPLEMENTATION OF INSTRUCTIONS /////
//////////////////////////////////////////

const std::vector<const MicroOp*>* InstructionDecoder::decode(IntPtr address, const xed_decoded_inst_t *ins, Instruction *ins_ptr)
{
   // Determine register dependencies and number of microops per type

   std::vector<std::set<xed_reg_enum_t> > regs_loads, regs_stores;
   std::set<xed_reg_enum_t> regs_mem, regs_src, regs_dst;

   int numLoads = 0;
   int numExecs = 0;
   int numStores = 0;

   // Ignore memory-referencing operands in NOP instructions
   if (!xed_decoded_inst_get_attribute(ins, XED_ATTRIBUTE_NOP))
   {
      for(uint32_t mem_idx = 0; mem_idx < xed_decoded_inst_number_of_memory_operands(ins); ++mem_idx)
      {
         std::set<xed_reg_enum_t> regs;
         regs.insert(xed_decoded_inst_get_base_reg(ins, mem_idx));
         regs.insert(xed_decoded_inst_get_index_reg(ins, mem_idx));

         if (xed_decoded_inst_mem_read(ins, mem_idx)) {
            regs_loads.push_back(regs);
            numLoads++;
         }

         if (xed_decoded_inst_mem_written(ins, mem_idx)) {
            regs_stores.push_back(regs);
            numStores++;
         }

         regs_mem.insert(regs.begin(), regs.end());
      }
   }

   const xed_inst_t *inst = xed_decoded_inst_inst(ins);
   for(uint32_t idx = 0; idx < xed_inst_noperands(inst); ++idx)
   {
      const xed_operand_t *op = xed_inst_operand(inst, idx);
      xed_operand_enum_t name = xed_operand_name(op);

      if (name == XED_OPERAND_AGEN)
      {
         /* LEA instruction */
         regs_src.insert(regs_mem.begin(), regs_mem.end());
      }
      else if (xed_operand_is_register(name))
      {
         xed_reg_enum_t reg = xed_decoded_inst_get_reg(ins, name);

         if (xed_operand_read(op) && regs_mem.count(reg) == 0)
            regs_src.insert(reg);
         if (xed_operand_written(op))
            regs_dst.insert(reg);
      }
   }

   // Not sure if this is needed, the hardware is doing a lot of optimizations on stack operations
   #if 0
   if (INS_IsStackRead(ins) || INS_IsStackWrite(ins))
   {
      // For push/pop, add the stack pointer as explicit destination register
      // (It's already listed as a source memory operand for the load/store, but the modified rsp needs to be written back)
      // (It should be one of the implicit operands, but those are rather difficult to parse correctly)
      regs_dst.insert(XED_REG_RSP);
   }
   #endif

   if (xed_decoded_inst_get_category(ins) == XED_CATEGORY_DATAXFER || xed_decoded_inst_get_category(ins) == XED_CATEGORY_CMOV
      || xed_decoded_inst_get_iclass(ins) == XED_ICLASS_PUSH || xed_decoded_inst_get_iclass(ins) == XED_ICLASS_POP)
   {
      if ((numLoads + numStores) == 0)
         numExecs = 1;
      else
         numExecs = 0;
   }
   else
   {
      numExecs = 1;
   }


   // Determine some extra instruction characteristics that will affect timing

   // Determine instruction operand width
   uint16_t operand_size = 0;
   for(uint32_t idx = 0; idx < xed_inst_noperands(inst); ++idx)
   {
      const xed_operand_t *op = xed_inst_operand(inst, idx);
      xed_operand_enum_t name = xed_operand_name(op);

      if (xed_operand_is_register(name))
      {
         xed_reg_enum_t reg = xed_decoded_inst_get_reg(ins, name);
         switch(reg)
         {
            case XED_REG_RFLAGS:
            case XED_REG_RIP:
            case XED_REG_RSP:
               continue;
            default:
               ;
         }
      }
      operand_size = std::max(operand_size, (uint16_t)xed_decoded_inst_get_operand_width(ins));
   }
   if (operand_size == 0)
      operand_size = 64;


   bool is_serializing = false;
   switch(xed_decoded_inst_get_iclass(ins)) {
      // TODO: There may be more (newer) instructions, but they are all kernel only
      case XED_ICLASS_JMP_FAR:
      case XED_ICLASS_CALL_FAR:
      case XED_ICLASS_RET_FAR:
      case XED_ICLASS_IRET:
      case XED_ICLASS_CPUID:
      case XED_ICLASS_LGDT:
      case XED_ICLASS_LIDT:
      case XED_ICLASS_LLDT:
      case XED_ICLASS_LTR:
      case XED_ICLASS_LMSW:
      case XED_ICLASS_WBINVD:
      case XED_ICLASS_INVD:
      case XED_ICLASS_INVLPG:
      case XED_ICLASS_RSM:
      case XED_ICLASS_WRMSR:
      case XED_ICLASS_SYSENTER:
      case XED_ICLASS_SYSRET:
         is_serializing = true;
         break;
      default:
         is_serializing = false;
         break;
   }


   // Generate list of microops

   std::vector<const MicroOp*> *uops = new std::vector<const MicroOp*>(); //< Return value
   int totalMicroOps = numLoads + numExecs + numStores;

   for(int index = 0; index < totalMicroOps; ++index)
   {

      MicroOp *currentMicroOp = new MicroOp();
      currentMicroOp->setInstructionPointer(Memory::make_access(address));

      // We don't necessarily know the address at this point as it could
      // be dependent on register values.  Therefore, fill it in at simulation time.

      if (index < numLoads) /* LOAD */
      {
         size_t loadIndex = index;
         currentMicroOp->makeLoad(
                 loadIndex
               , xed_decoded_inst_get_iclass(ins)
               , xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
               );
      }
      else if (index < numLoads + numExecs) /* EXEC */
      {
         currentMicroOp->makeExecute(
                 0
               , numLoads
               , xed_decoded_inst_get_iclass(ins)
               , xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
               , xed_decoded_inst_get_category(ins) == XED_CATEGORY_COND_BR /* is conditional branch? */);
      }
      else /* STORE */
      {
         size_t storeIndex = index - numLoads - numExecs;
         currentMicroOp->makeStore(
                 storeIndex
               , numExecs
               , xed_decoded_inst_get_iclass(ins)
               , xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))
               );
      }


      // Fill in the destination registers for both loads and executes, also on stores if there are no loads or executes

      if (index < numLoads) /* LOAD */
      {
         size_t loadIndex = index;
         addSrcs(regs_loads[loadIndex], currentMicroOp);

         if (numExecs == 0) {
            // No execute microop: we inherit its read operands
            addSrcs(regs_src, currentMicroOp);
            if (numStores == 0)
               // No store microop either: we also inherit its write operands
               addDsts(regs_dst, currentMicroOp);
         }
      }

      else if (index < numLoads + numExecs) /* EXEC */
      {
         addSrcs(regs_src, currentMicroOp);
         addDsts(regs_dst, currentMicroOp);

         if (xed_decoded_inst_get_iclass(ins) == XED_ICLASS_MFENCE
            || xed_decoded_inst_get_iclass(ins) == XED_ICLASS_LFENCE
            || xed_decoded_inst_get_iclass(ins) == XED_ICLASS_SFENCE
         )
            currentMicroOp->setMemBarrier(true);
      }

      else /* STORE */
      {
         size_t storeIndex = index - numLoads - numExecs;
         addSrcs(regs_stores[storeIndex], currentMicroOp);

         if (numExecs == 0) {
            // No execute microop: we inherit its write operands
            addDsts(regs_dst, currentMicroOp);
            if (numLoads == 0)
               // No load microops either: we also inherit its read operands
               addSrcs(regs_src, currentMicroOp);
         }
      }


      /* Extra information on all micro ops */

      currentMicroOp->setOperandSize(operand_size);
      currentMicroOp->setInstruction(ins_ptr);


      /* Extra information of first micro op */

      if (index == 0)
      {
         currentMicroOp->setFirst(true);

         // Use of x87 FPU?
         if (toupper(xed_iclass_enum_t2str(xed_decoded_inst_get_iclass(ins))[0]) == 'F')
            currentMicroOp->setIsX87(true);
      }


      /* Extra information on last micro op */

      if (index == totalMicroOps - 1) {
         currentMicroOp->setLast(true);

         // Interrupts and serializing instructions are added to the last microOp
         // Since we're user-level, there are no interrupts
         #if 0
         // Check if the instruction is an interrupt, place the interrupt flag
         if (disassembler_instruction->insType != IT_BRANCH
               && disassembler_instruction->insType != IT_CALL
               && disassembler_instruction->insType != IT_RETURN
               && jump) {
            currentMicroOp->setInterrupt(true);
         }
         #endif

         // Check if the instruction is serializing, place the serializing flag
         if (is_serializing)
            currentMicroOp->setSerializing(true);
      }

      uops->push_back(currentMicroOp);
   }

   return uops;
}