// Copyright 2009-2022 NTESS. Under the terms
// of Contract DE-NA0003525 with NTESS, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2022, NTESS
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// of the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>
#include "vanadis.h"

#include "decoder/vmipsdecoder.h"
#include "decoder/vriscv64decoder.h"
#include "inst/vinstall.h"
#include "velf/velfinfo.h"

#include "os/resp/vosexitresp.h"
#include "os/vgetthreadstate.h"
#include "os/vdumpregsreq.h"

#include <cstdio>
#include <sst/core/output.h>
#include <vector>

using namespace SST::Vanadis;

VANADIS_COMPONENT::VANADIS_COMPONENT(SST::ComponentId_t id, SST::Params& params) : Component(id), current_cycle(0)
{

    instPrintBuffer = new char[1024];
    pipelineTrace   = nullptr;

    max_cycle = params.find<uint64_t>("max_cycle", std::numeric_limits<uint64_t>::max());

    const int32_t dbg_mask = params.find<int32_t>("dbg_mask", 0);
    const int32_t verbosity = params.find<int32_t>("verbose", 0);
    core_id                 = params.find<uint32_t>("core_id", 0);

    char* outputPrefix = (char*)malloc(sizeof(char) * 256);
    sprintf(outputPrefix, "[Core: %4" PRIu32 "/@t]: ", core_id);

    output = new SST::Output(outputPrefix, verbosity, dbg_mask, Output::STDOUT);
    free(outputPrefix);

    std::string clock_rate = params.find<std::string>("clock", "1GHz");
    output->verbose(CALL_INFO, 2, 0, "Registering clock at %s.\n", clock_rate.c_str());
    cpuClockHandler = new Clock::Handler<VANADIS_COMPONENT>(this, &VANADIS_COMPONENT::tick);
    cpuClockTC      = registerClock(clock_rate, cpuClockHandler);

    const uint32_t rob_count = params.find<uint32_t>("reorder_slots", 64);
    dCacheLineWidth          = params.find<uint64_t>("dcache_line_width", 64);
    iCacheLineWidth          = params.find<uint64_t>("icache_line_width", 64);

    output->verbose(CALL_INFO, 2, 0, "Core L1 Cache Configurations:\n");
    output->verbose(CALL_INFO, 2, 0, "-> D-Cache Line Width:       %" PRIu64 " bytes\n", dCacheLineWidth);
    output->verbose(CALL_INFO, 2, 0, "-> I-Cache Line Width:       %" PRIu64 " bytes\n", iCacheLineWidth);

    hw_threads = params.find<uint32_t>("hardware_threads", 1);
    output->verbose(CALL_INFO, 2, 0, "Creating %" PRIu32 " SMT threads.\n", hw_threads);

    print_int_reg = params.find<bool>("print_int_reg", verbosity > 16 ? 1 : 0);
    print_fp_reg  = params.find<bool>("print_fp_reg", verbosity > 16 ? 1 : 0);

    print_retire_tables = params.find<bool>("print_retire_tables", true);
    print_issue_tables  = params.find<bool>("print_issue_tables", true);
    print_rob  = params.find<bool>("print_rob", true);

    const uint16_t int_reg_count = params.find<uint16_t>("physical_integer_registers", 128);
    const uint16_t fp_reg_count  = params.find<uint16_t>("physical_fp_registers", 128);

    output->verbose(
        CALL_INFO, 2, 0,
        "Creating physical register files (quantities are per "
        "hardware thread)...\n");
    output->verbose(CALL_INFO, 2, 0, "Physical Integer Registers (GPRs): %5" PRIu16 "\n", int_reg_count);
    output->verbose(CALL_INFO, 2, 0, "Physical Floating-Point Registers: %5" PRIu16 "\n", fp_reg_count);

    const uint16_t issue_queue_len = params.find<uint16_t>("issue_queue_length", 4);

    halted_masks = new bool[hw_threads];

    os_link = configureLink("os_link", "0ns", new Event::Handler<VANADIS_COMPONENT>(this, &VANADIS_COMPONENT::recvOSEvent));
    if ( nullptr == os_link ) {
        output->fatal(CALL_INFO, -1, "Error: was unable to configureLink %s \n", "os_link");
    }

    //////////////////////////////////////////////////////////////////////////////////////

    char* decoder_name = new char[64];

    for ( uint32_t i = 0; i < hw_threads; ++i ) {

        sprintf(decoder_name, "decoder%" PRIu32 "", i);
        VanadisDecoder* thr_decoder = loadUserSubComponent<SST::Vanadis::VanadisDecoder>(decoder_name);

		  fp_flags.push_back(new VanadisFloatingPointFlags());
		  thr_decoder->setFPFlags(fp_flags[i]);

        //     thr_decoder->setHardwareThread( i );

        output->verbose(
            CALL_INFO, 8, 0, "Loading decoder%" PRIu32 ": %s.\n", i,
            (nullptr == thr_decoder) ? "failed" : "successful");

        if ( nullptr == thr_decoder ) {
            output->fatal(CALL_INFO, -1, "Error: was unable to load %s on thread %" PRIu32 "\n", decoder_name, i);
        }
        else {
            output->verbose(CALL_INFO, 8, 0, "-> Decoder configured for %s\n", thr_decoder->getISAName());
        }

        thr_decoder->setHardwareThread(i);
        thread_decoders.push_back(thr_decoder);


        thread_decoders[i]->getOSHandler()->setOS_link(os_link);

        if ( 0 == thread_decoders[i]->getInsCacheLineWidth() ) {
            output->verbose(
                CALL_INFO, 2, 0, "Auto-setting icache line width in decoder to %" PRIu64 "\n", iCacheLineWidth);
            thread_decoders[i]->setInsCacheLineWidth(iCacheLineWidth);
        }
        else {
            if ( iCacheLineWidth < thread_decoders[i]->getInsCacheLineWidth() ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Decoder for thr %" PRIu32 " has an override icache-line-width of %" PRIu64
                    ", this exceeds the core icache-line-with of %" PRIu64
                    " and is likely to result in cache load failures. Set "
                    "this to less than equal to %" PRIu64 "\n",
                    i, thread_decoders[i]->getInsCacheLineWidth(), iCacheLineWidth, iCacheLineWidth);
            }
            else {
                output->verbose(
                    CALL_INFO, 2, 0,
                    "Decoder for thr %" PRIu32 " is already set to %" PRIu64
                    ", will not auto-set. The core icache-line-width is "
                    "currently: %" PRIu64 "\n",
                    (uint32_t)i, thread_decoders[i]->getInsCacheLineWidth(), iCacheLineWidth);
            }
        }

        isa_options.push_back(thread_decoders[i]->getDecoderOptions());

        output->verbose(
            CALL_INFO, 8, 0, "Thread: %6" PRIu32 " ISA set to: %s [Int-Reg: %" PRIu16 "/FP-Reg: %" PRIu16 "]\n", i,
            thread_decoders[i]->getISAName(), thread_decoders[i]->countISAIntReg(),
            thread_decoders[i]->countISAFPReg());

        register_files.push_back(new VanadisRegisterFile(
            i, thread_decoders[i]->getDecoderOptions(), int_reg_count, fp_reg_count, thr_decoder->getFPRegisterMode()));
        int_register_stacks.push_back(new VanadisRegisterStack(int_reg_count));
        fp_register_stacks.push_back(new VanadisRegisterStack(fp_reg_count));

        output->verbose(
            CALL_INFO, 8, 0, "Reorder buffer set to %" PRIu32 " entries, these are shared by all threads.\n",
            rob_count);
        rob.push_back(new VanadisCircularQueue<VanadisInstruction*>(rob_count));
        // WE NEED ISA INTEGER AND FP COUNTS HERE NOT ZEROS
        issue_isa_tables.push_back(new VanadisISATable( "issue",
            thread_decoders[i]->getDecoderOptions(), thread_decoders[i]->countISAIntReg(),
            thread_decoders[i]->countISAFPReg()));

        thread_decoders[i]->setThreadROB(rob[i]);

        for ( uint16_t j = 0; j < thread_decoders[i]->countISAIntReg(); ++j ) {
            issue_isa_tables[i]->setIntPhysReg(j, int_register_stacks[i]->pop());
        }

        for ( uint16_t j = 0; j < thread_decoders[i]->countISAFPReg(); ++j ) {
            issue_isa_tables[i]->setFPPhysReg(j, fp_register_stacks[i]->pop());
        }

        retire_isa_tables.push_back(new VanadisISATable( "retire",
            thread_decoders[i]->getDecoderOptions(), thread_decoders[i]->countISAIntReg(),
            thread_decoders[i]->countISAFPReg()));
        retire_isa_tables[i]->reset(issue_isa_tables[i]);

        halted_masks[i] = true;
    }

    delete[] decoder_name;

    uint16_t max_int_regs = 0;
    uint16_t max_fp_regs  = 0;

    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        max_int_regs = std::max(max_int_regs, thread_decoders[i]->countISAIntReg());
        max_fp_regs  = std::max(max_fp_regs, thread_decoders[i]->countISAFPReg());
    }

    //	printf("MAX INT: %" PRIu16 ", MAX FP: %" PRIu16 "\n", max_int_regs,
    // max_fp_regs );

    tmp_not_issued_int_reg_read = new uint8_t[max_int_regs];
    tmp_int_reg_write = new uint8_t[max_int_regs];

    tmp_not_issued_fp_reg_read = new uint8_t[max_fp_regs];
    tmp_fp_reg_write = new uint8_t[max_fp_regs];

    resetRegisterUseTemps(max_int_regs, max_fp_regs);

    //	memDataInterface =
    // loadUserSubComponent<Interfaces::SimpleMem>("mem_interface_data",
    // ComponentInfo::SHARE_NONE, cpuClockTC, 		new
    // SimpleMem::Handler<SST::Vanadis::VanadisComponent>(this,
    //&VanadisComponent::handleIncomingDataCacheEvent ));
    memInstInterface = loadUserSubComponent<Interfaces::StandardMem>(
        "mem_interface_inst", ComponentInfo::SHARE_NONE, cpuClockTC,
        new StandardMem::Handler<SST::Vanadis::VANADIS_COMPONENT>(
            this, &VANADIS_COMPONENT::handleIncomingInstCacheEvent));

    if ( nullptr == memInstInterface ) {
        output->fatal(
            CALL_INFO, -1,
            "Error: unable ot load memory interface subcomponent for "
            "instruction cache.\n");
    }

    output->verbose(CALL_INFO, 1, 0, "Successfully loaded memory interface.\n");

    for ( uint32_t i = 0; i < thread_decoders.size(); ++i ) {
        output->verbose(CALL_INFO, 8, 0, "Configuring thread instruction cache interface (thread %" PRIu32 ")\n", i);
        thread_decoders[i]->getInstructionLoader()->setMemoryInterface(memInstInterface);
    }

    lsq = loadUserSubComponent<SST::Vanadis::VanadisLoadStoreQueue>("lsq");

    if ( nullptr == lsq ) {
        output->fatal(CALL_INFO, -1, "Error - unable to load the load-store queue (lsq subcomponent)\n");
    }

    lsq->setRegisterFiles(&register_files);

    //////////////////////////////////////////////////////////////////////////////////////

    uint16_t fu_id = 0;

    const uint16_t int_arith_units  = params.find<uint16_t>("integer_arith_units", 2);
    const uint16_t int_arith_cycles = params.find<uint16_t>("integer_arith_cycles", 2);

    output->verbose(
        CALL_INFO, 2, 0, "Creating %" PRIu16 " integer arithmetic units, latency = %" PRIu16 "...\n", int_arith_units,
        int_arith_cycles);

    for ( uint16_t i = 0; i < int_arith_units; ++i ) {
        fu_int_arith.push_back(new VanadisFunctionalUnit(fu_id++, INST_INT_ARITH, int_arith_cycles));
    }

    const uint16_t int_div_units  = params.find<uint16_t>("integer_div_units", 1);
    const uint16_t int_div_cycles = params.find<uint16_t>("integer_div_cycles", 4);

    output->verbose(
        CALL_INFO, 2, 0, "Creating %" PRIu16 " integer division units, latency = %" PRIu16 "...\n", int_div_units,
        int_div_cycles);

    for ( uint16_t i = 0; i < int_div_units; ++i ) {
        fu_int_div.push_back(new VanadisFunctionalUnit(fu_id++, INST_INT_DIV, int_div_cycles));
    }

    const uint16_t branch_units  = params.find<uint16_t>("branch_units", 1);
    const uint16_t branch_cycles = params.find<uint16_t>("branch_unit_cycles", int_arith_cycles);

    output->verbose(
        CALL_INFO, 2, 0, "Creating %" PRIu16 " branching units, latency = %" PRIu16 "...\n", branch_units,
        branch_cycles);

    for ( uint16_t i = 0; i < branch_units; ++i ) {
        fu_branch.push_back(new VanadisFunctionalUnit(fu_id++, INST_BRANCH, branch_cycles));
    }

    //////////////////////////////////////////////////////////////////////////////////////

    const uint16_t fp_arith_units  = params.find<uint16_t>("fp_arith_units", 2);
    const uint16_t fp_arith_cycles = params.find<uint16_t>("fp_arith_cycles", 8);

    output->verbose(
        CALL_INFO, 2, 0, "Creating %" PRIu16 " floating point arithmetic units, latency = %" PRIu16 "...\n",
        fp_arith_units, fp_arith_cycles);

    for ( uint16_t i = 0; i < fp_arith_units; ++i ) {
        fu_fp_arith.push_back(new VanadisFunctionalUnit(fu_id++, INST_FP_ARITH, fp_arith_cycles));
    }

    const uint16_t fp_div_units  = params.find<uint16_t>("fp_div_units", 1);
    const uint16_t fp_div_cycles = params.find<uint16_t>("fp_div_cycles", 80);

    output->verbose(
        CALL_INFO, 2, 0, "Creating %" PRIu16 " floating point division units, latency = %" PRIu16 "...\n", fp_div_units,
        fp_div_cycles);

    for ( uint16_t i = 0; i < fp_div_units; ++i ) {
        fu_fp_div.push_back(new VanadisFunctionalUnit(fu_id++, INST_FP_DIV, fp_div_cycles));
    }

    //////////////////////////////////////////////////////////////////////////////////////
    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        thread_decoders[i]->getOSHandler()->setCoreID(core_id);
        thread_decoders[i]->getOSHandler()->setHWThread(i);
        thread_decoders[i]->getOSHandler()->setRegisterFile(register_files[i]);
        thread_decoders[i]->getOSHandler()->setISATable(retire_isa_tables[i]);
    }

    //////////////////////////////////////////////////////////////////////////////////////

    fetches_per_cycle = params.find<uint32_t>("fetches_per_cycle", 2);
    decodes_per_cycle = params.find<uint32_t>("decodes_per_cycle", 2);
    issues_per_cycle  = params.find<uint32_t>("issues_per_cycle", 2);
    retires_per_cycle = params.find<uint32_t>("retires_per_cycle", 2);

    output->verbose(CALL_INFO, 8, 0, "Configuring hardware parameters:\n");
    output->verbose(CALL_INFO, 8, 0, "-> Fetches/cycle:                %" PRIu32 "\n", fetches_per_cycle);
    output->verbose(CALL_INFO, 8, 0, "-> Decodes/cycle:                %" PRIu32 "\n", decodes_per_cycle);
    output->verbose(CALL_INFO, 8, 0, "-> Retires/cycle:                %" PRIu32 "\n", retires_per_cycle);

    std::string pipeline_trace_path = params.find<std::string>("pipeline_trace_file", "");

    if ( pipeline_trace_path == "" ) {
        output->verbose(CALL_INFO, 8, 0, "Pipeline trace output not specified, disabling.\n");
    }
    else {
        output->verbose(CALL_INFO, 8, 0, "Opening a pipeline trace output at: %s\n", pipeline_trace_path.c_str());
        pipelineTrace = fopen(pipeline_trace_path.c_str(), "wt");

        if ( pipelineTrace == nullptr ) { output->fatal(CALL_INFO, -1, "Failed to open pipeline trace file.\n"); }
    }

    pause_on_retire_address = params.find<uint64_t>("pause_when_retire_address", 0);
    start_verbose_when_issue_address = params.find<uint64_t>("start_verbose_when_issue_address", 0);

    // Register statistics ///////////////////////////////////////////////////////
    stat_ins_retired          = registerStatistic<uint64_t>("instructions_retired", "1");
    stat_ins_decoded          = registerStatistic<uint64_t>("instructions_decoded", "1");
    stat_ins_issued           = registerStatistic<uint64_t>("instructions_issued", "1");
    stat_loads_issued         = registerStatistic<uint64_t>("loads_issued", "1");
    stat_stores_issued        = registerStatistic<uint64_t>("stores_issued", "1");
    stat_branch_mispredicts   = registerStatistic<uint64_t>("branch_mispredicts", "1");
    stat_branches             = registerStatistic<uint64_t>("branches", "1");
    stat_cycles               = registerStatistic<uint64_t>("cycles", "1");
    stat_rob_entries          = registerStatistic<uint64_t>("rob_slots_in_use", "1");
    stat_rob_cleared_entries  = registerStatistic<uint64_t>("rob_cleared_entries", "1");
    stat_syscall_cycles       = registerStatistic<uint64_t>("syscall-cycles", "1");
    stat_int_phys_regs_in_use = registerStatistic<uint64_t>("phys_int_reg_in_use", "1");
    stat_fp_phys_regs_in_use  = registerStatistic<uint64_t>("phys_fp_reg_in_use", "1");

    //registerAsPrimaryComponent();
    //primaryComponentDoNotEndSim();
}

VANADIS_COMPONENT::~VANADIS_COMPONENT()
{
    delete[] instPrintBuffer;
    delete lsq;

    if ( pipelineTrace != nullptr ) { fclose(pipelineTrace); }

	for( VanadisFloatingPointFlags* next_fp_flags : fp_flags ) {
		delete next_fp_flags;
	}

    delete[] tmp_not_issued_int_reg_read;
    delete[] tmp_int_reg_write;
    delete[] tmp_not_issued_fp_reg_read;
    delete[] tmp_fp_reg_write;
}

void
VANADIS_COMPONENT::startThread(int thr, uint64_t stackStart, uint64_t instructionPointer ) {

    halted_masks[thr]            = false;
    uint64_t initial_config_ip = thread_decoders[thr]->getInstructionPointer();

    // This wasn't provided, or its explicitly set to zero which means
    // we should auto-calculate it
    output->verbose(CALL_INFO, 8, 0, "Configuring core-%d, thread-%d entry point = %p stack = %#" PRIx64  "\n", core_id, thr, (void*)instructionPointer, stackStart);
    thread_decoders[thr]->setInstructionPointer(instructionPointer);

    thread_decoders[thr]->setStackPointer( output, issue_isa_tables[thr], register_files[thr], stackStart );

    // Force retire table to sync with issue table
    retire_isa_tables[thr]->reset(issue_isa_tables[thr]);

    if ( initial_config_ip > 0 ) {
        output->verbose(
                CALL_INFO, 8, 0, "Overrding entry point for core-0, thread-0, set to 0x%llx\n", initial_config_ip);
        thread_decoders[thr]->setInstructionPointer(initial_config_ip);
    }
    else {
        output->verbose(
            CALL_INFO, 8, 0, "Utilizing entry point from binary (auto-detected) 0x%llx\n",
            thread_decoders[thr]->getInstructionPointer());
    }
}

void
VANADIS_COMPONENT::setHalt(uint32_t thr, int64_t halt_code)
{
    output->verbose(
        CALL_INFO, 2, 0, "-> Receive halt request on thread %" PRIu32 " / code: %" PRId64 "\n", thr, halt_code);

    if ( thr >= hw_threads ) {
        // Incorrect thread, ignore? error?
    }
    else {
        switch ( halt_code ) {
        default:
        {
            halted_masks[thr] = true;

            // Reset address to zero
            handleMisspeculate(thr, 0);

            bool all_halted = true;

            for ( uint32_t i = 0; i < hw_threads; ++i ) {
                all_halted = all_halted & halted_masks[i];
            }

            if ( all_halted ) {
                output->verbose(
                    CALL_INFO, 2, 0,
                    "-> all threads on core are halted, tell core we can "
                    "exit further simulation unless we recv a wake up.\n");
                primaryComponentOKToEndSim();
            }
        } break;
        }
    }
}

int
VANADIS_COMPONENT::performFetch(const uint64_t cycle)
{
    // This is handled by the decoder step, so just keep it empty.
    return 0;
}

int
VANADIS_COMPONENT::performDecode(const uint64_t cycle)
{

    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        const int64_t rob_before_decode = (int64_t)rob[i]->size();

        // If thread is not masked then decode from it
        if ( !halted_masks[i] ) { thread_decoders[i]->tick(output, (uint64_t)cycle); }

        const int64_t rob_after_decode = (int64_t)rob[i]->size();
        const int64_t decoded_cycle    = (rob_after_decode - rob_before_decode);
        ins_decoded_this_cycle += (decoded_cycle > 0) ? static_cast<uint64_t>(decoded_cycle) : 0;
    }

    return 0;
}

void
VANADIS_COMPONENT::resetRegisterUseTemps(const uint16_t int_reg_count, const uint16_t fp_reg_count)
{
    std::memset(tmp_not_issued_int_reg_read, 0, int_reg_count);
    std::memset(tmp_int_reg_write, 0, int_reg_count);

    std::memset(tmp_not_issued_fp_reg_read, 0, fp_reg_count);
    std::memset(tmp_fp_reg_write, 0, fp_reg_count);
}

int
VANADIS_COMPONENT::performIssue(const uint64_t cycle, uint32_t& rob_start, bool& unallocated_memory_op_seen)
{
    const int output_verbosity = output->getVerboseLevel();
    bool      issued_an_ins    = false;

    for ( auto i = 0; i < hw_threads; ++i ) {
        if ( LIKELY(! halted_masks[i] )) {
#ifdef VANADIS_BUILD_DEBUG
            if ( output->getVerboseLevel() >= 4 ) {
                if(print_issue_tables) {
                    issue_isa_tables[i]->print(output, register_files[i], print_int_reg, print_fp_reg);
                }
            }
#endif
            // we have not issued an instruction this cycle
            issued_an_ins = false;

            // Find the next instruction which has not been issued yet
            const auto rob_size = rob[i]->size();

            for ( auto j = rob_start; j < rob_size; ++j ) {
                VanadisInstruction* ins = rob[i]->peekAt(j);

                if ( ! ins->completedIssue() ) {
#ifdef VANADIS_BUILD_DEBUG
                    if ( output_verbosity >= 8 ) {
                        ins->printToBuffer(instPrintBuffer, 1024);
                        output->verbose(
                            CALL_INFO, 9, 0, "--> Attempting issue for: rob[%" PRIu32 "]: 0x%llx / %s\n", j,
                            ins->getInstructionAddress(), instPrintBuffer);
                    }
#endif
                    const int resource_check = checkInstructionResources(
                        ins, int_register_stacks[i], fp_register_stacks[i], issue_isa_tables[i]);

#ifdef VANADIS_BUILD_DEBUG
                    if ( output_verbosity >= 8 ) {
                        output->verbose(
                            CALL_INFO, 9, 0, "----> Check if registers are usable? result: %d (%s)\n", resource_check,
                            (0 == resource_check) ? "success" : "cannot issue");
                    }
#endif
                    const auto ins_type = ins->getInstFuncType();

                    if ( 0 == resource_check ) {
                        int allocate_fu = 1;

                        if( (ins_type == INST_LOAD || ins_type == INST_STORE || ins_type == INST_FENCE) ) {
                            if(unallocated_memory_op_seen) {
                                // the instruction should not be allocated because memory operations
                                // must be issued to the LSQ in order to maintain memory ordering 
                                // semantics
                                allocate_fu = 1;
                            } else {
                                allocate_fu = allocateFunctionalUnit(ins);
                            }
                        } else {
                            allocate_fu = allocateFunctionalUnit(ins);
                        }

#ifdef VANADIS_BUILD_DEBUG
                        if ( output_verbosity >= 8 ) {
                            output->verbose(
                                CALL_INFO, 9, 0, "----> allocated functional unit: %s\n",
                                (0 == allocate_fu) ? "yes" : "no");
                        }
#endif
                        if ( 0 == allocate_fu ) {
                            const int status = assignRegistersToInstruction(
                                thread_decoders[i]->countISAIntReg(), thread_decoders[i]->countISAFPReg(), ins,
                                int_register_stacks[i], fp_register_stacks[i], issue_isa_tables[i]);

                            if ( ins->getInstructionAddress() == start_verbose_when_issue_address ) {
                                output->setVerboseLevel(16);
                                output->setVerboseMask(VANADIS_DBG_ISSUE_FLG);
                            }

#ifdef VANADIS_BUILD_DEBUG
                            if ( output_verbosity >= 8 ) {
                                ins->printToBuffer(instPrintBuffer, 1024);
                                output->verbose(
                                    CALL_INFO, 8, VANADIS_DBG_ISSUE_FLG, "----> Issued for: %s / 0x%llx / status: %d\n",
                                    instPrintBuffer, ins->getInstructionAddress(), status);
                                if ( print_rob ) {
                                    printRob(rob[i]);
                                }
                            }
#endif
                            ins->markIssued();
                            ins_issued_this_cycle++;
                            issued_an_ins = true;
                        } else {
                            if(ins_type == INST_LOAD || ins_type == INST_STORE || ins_type == INST_FENCE) {
                                // we have seen a memory operation which is not issued, downstream operations
                                // cannot issue yet to maintain ordering
                                unallocated_memory_op_seen = true;
                            }
                        }
                    } else {
                        if(ins_type == INST_LOAD || ins_type == INST_STORE || ins_type == INST_FENCE) {
                            // we have seen a memory operation which is not issued, downstream operations
                            // cannot issue yet to maintain ordering
                            unallocated_memory_op_seen = true;
                        }
                    }

                    // if the instruction is *not* issued yet, we need to keep track
                    // of which instructions are being read
                    for ( auto k = 0; k < ins->countISAIntRegIn(); ++k ) {
                        tmp_not_issued_int_reg_read[ins->getISAIntRegIn(k)] = 1;
                    }

                    for ( auto k = 0; k < ins->countISAFPRegIn(); ++k ) {
                        tmp_not_issued_fp_reg_read[ins->getISAFPRegIn(k)] = 1;
                    }
                }

                // Collect up all integer registers we write to
                for ( auto k = 0; k < ins->countISAIntRegOut(); ++k ) {
                    tmp_int_reg_write[ins->getISAIntRegOut(k)] = 1;
                }

                // Collect up all fp registers we write to
                for ( auto k = 0; k < ins->countISAFPRegOut(); ++k ) {
                    tmp_fp_reg_write[ins->getISAFPRegOut(k)] = 1;
                }

                // We issued an instruction this cycle, so exit
                if ( issued_an_ins ) {
                    // tell the caller where we got this from
                    rob_start = j;
                    break;
                }
            }

            // Only print the table if we issued an instruction, reduce print out
            // clutter
            if ( (output_verbosity >= 8) && issued_an_ins ) {
                if(print_issue_tables) {
                    issue_isa_tables[i]->print(output, register_files[i], print_int_reg, print_fp_reg, output_verbosity );
                }
            }
        }
        else {
            if(output_verbosity >= 8) {
                output->verbose(
                    CALL_INFO, 8, 0, "thread %" PRIu32 " is halted, did not process for issue this cycle.\n", i);
            }
        }
    }

    // if we issued an instruction tell the caller we want to be called again
    // (return 0)
    return issued_an_ins ? 0 : 1;
}

int
VANADIS_COMPONENT::performExecute(const uint64_t cycle)
{
    const uint32_t verbose_level = output->getVerboseLevel();

    for ( VanadisFunctionalUnit* next_fu : fu_int_arith ) {
        next_fu->tick(cycle, output, register_files);

        if(verbose_level >= 16)
            next_fu->print(output);
    }

    for ( VanadisFunctionalUnit* next_fu : fu_int_div ) {
        next_fu->tick(cycle, output, register_files);

        if(verbose_level >= 16)
            next_fu->print(output);
    }

    for ( VanadisFunctionalUnit* next_fu : fu_fp_arith ) {
        next_fu->tick(cycle, output, register_files);

        if(verbose_level >= 16)
            next_fu->print(output);
    }

    for ( VanadisFunctionalUnit* next_fu : fu_fp_div ) {
        next_fu->tick(cycle, output, register_files);

        if(verbose_level >= 16)
            next_fu->print(output);
    }

    for ( VanadisFunctionalUnit* next_fu : fu_branch ) {
        next_fu->tick(cycle, output, register_files);

        if(verbose_level >= 16)
            next_fu->print(output);
    }

    // Tick the load/store queue
    lsq->tick((uint64_t)cycle);

    return 0;
}

void VANADIS_COMPONENT::printRob(VanadisCircularQueue<VanadisInstruction*>* rob)
{
    output->verbose(
            CALL_INFO, 8, 0, "-- ROB: %" PRIu32 " out of %" PRIu32 " entries:\n", (uint32_t)rob->size(),
            (uint32_t)rob->capacity());

    for ( int j = (int)rob->size() - 1; j >= 0; --j ) {
        output->verbose(
            CALL_INFO, 8, 0,
            "----> ROB[%2d]: ins: 0x%016llx / %10s / error: %3s / "
            "issued: %3s / spec: %3s / rob-front: %3s / exe: %3s\n",
            j, rob->peekAt(j)->getInstructionAddress(), rob->peekAt(j)->getInstCode(),
            rob->peekAt(j)->trapsError() ? "yes" : "no", rob->peekAt(j)->completedIssue() ? "yes" : "no",
            rob->peekAt(j)->isSpeculated() ? "yes" : "no", rob->peekAt(j)->checkFrontOfROB() ? "yes" : "no",
            rob->peekAt(j)->completedExecution() ? "yes" : "no");
    }
}

int
VANADIS_COMPONENT::performRetire(VanadisCircularQueue<VanadisInstruction*>* rob, const uint64_t cycle)
{

#ifdef VANADIS_BUILD_DEBUG
    if ( output->getVerboseLevel() >= 9 ) {
        printRob( rob );
    }
#endif

    // if empty, nothing to do here, return 1 to prevent being called again as
    // nothing we can do this cycle. This is likely the result of a branch
    // mis-predict
    if ( rob->empty() ) { return 1; }

    VanadisInstruction* rob_front              = rob->peek();
    bool                perform_pipeline_clear = false;
    const uint32_t      ins_thread             = rob->peekAt(0)->getHWThread();

    // Instruction is flagging error, print out and halt
    if ( UNLIKELY(rob_front->trapsError()) ) {
        output->verbose(
            CALL_INFO, 0, 0,
            "Error has been detected in retired instruction. Retired "
            "register status:\n");

        retire_isa_tables[rob_front->getHWThread()]->print(
            output, register_files[ins_thread], print_int_reg, print_fp_reg, 0);

        char* inst_asm_buffer = new char[32768];
        rob_front->printToBuffer(inst_asm_buffer, 32768);

        output->fatal(
            CALL_INFO, -1,
            "Instruction 0x%llx flags an error (instruction-type=%s) at "
            "cycle %" PRIu64 " (inst: %s)\n",
            rob_front->getInstructionAddress(), rob_front->getInstCode(), cycle, inst_asm_buffer);

        delete[] inst_asm_buffer;
    }

    if ( rob_front->completedIssue() && rob_front->completedExecution() ) {
        bool     perform_cleanup       = true;
        bool     perform_delay_cleanup = false;
        uint64_t pipeline_reset_addr   = 0;

        if ( rob_front->isSpeculated() ) {
#ifdef VANADIS_BUILD_DEBUG
            if(output->getVerboseLevel() >= 8) {
                output->verbose(CALL_INFO, 9, 0, "--> instruction is speculated\n");
            }
#endif
            VanadisSpeculatedInstruction* spec_ins = dynamic_cast<VanadisSpeculatedInstruction*>(rob_front);

            if ( nullptr == spec_ins ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Error - instruction is speculated, but not able to "
                    "perform a cast to a speculated instruction.\n");
            }

            stat_branches->addData(1);

            switch ( spec_ins->getDelaySlotType() ) {
            case VANADIS_SINGLE_DELAY_SLOT:
            case VANADIS_CONDITIONAL_SINGLE_DELAY_SLOT:
            {
                // is there an instruction behind us in the ROB queue?
                if ( rob->size() >= 2 ) {
                    VanadisInstruction* delay_ins = rob->peekAt(1);

                    if ( delay_ins->completedExecution() ) {
                        if ( UNLIKELY(delay_ins->trapsError()) ) {
                            output->fatal(
                                CALL_INFO, -1,
                                "Instruction (delay-slot) 0x%llx flags an error "
                                "(instruction-type: %s)\n",
                                delay_ins->getInstructionAddress(), delay_ins->getInstCode());
                        }

                        perform_delay_cleanup = true;
                    }
                    else {
#ifdef VANADIS_BUILD_DEBUG
                        if(output->getVerboseLevel() >= 8) {
                            output->verbose(
                                CALL_INFO, 8, 0,
                                "----> delay slot has not completed execution, "
                                "stall to wait.\n");
                        }
#endif
                        if ( !delay_ins->checkFrontOfROB() ) { delay_ins->markFrontOfROB(); }

                        perform_cleanup = false;
                    }
                }
                else {
                    // The instruction is not in the ROB yet, so we must wait
                    perform_cleanup       = false;
                    perform_delay_cleanup = false;
                }
            } break;
            case VANADIS_NO_DELAY_SLOT:
                break;
            }

            // If we are performing a clean up (means we executed and the delays are
            // processed OK, then we are good to calculate branch-to locations.
            if ( perform_cleanup ) {
                pipeline_reset_addr    = spec_ins->getTakenAddress();
                perform_pipeline_clear = (pipeline_reset_addr != spec_ins->getSpeculatedAddress());

#ifdef VANADIS_BUILD_DEBUG
                if(output->getVerboseLevel() >= 8) {
                output->verbose(
                    CALL_INFO, 8, 0,
                    "----> Retire: speculated addr: 0x%llx / result addr: 0x%llx / "
                    "pipeline-clear: %s\n",
                    spec_ins->getSpeculatedAddress(), pipeline_reset_addr, perform_pipeline_clear ? "yes" : "no");

                output->verbose(
                    CALL_INFO, 9, 0,
                    "----> Updating branch predictor with new information "
                    "(new addr: 0x%llx)\n",
                    pipeline_reset_addr);
                if ( print_rob ) {
                    printRob(rob);
                }
                }
#endif
                thread_decoders[ins_thread]->getBranchPredictor()->push(
                    spec_ins->getInstructionAddress(), pipeline_reset_addr);

                if ( (pause_on_retire_address > 0) &&
                     (rob_front->getInstructionAddress() == pause_on_retire_address) ) {

                    // print the register and pipeline status
                    printStatus((*output));

                    output->verbose(
                        CALL_INFO, 0, 0,
                        "ins: 0x%llx / speculated-address: 0x%llx / taken: 0x%llx / "
                        "reset: 0x%llx / clear-check: %3s / pipe-clear: %3s / "
                        "delay-cleanup: %3s\n",
                        spec_ins->getInstructionAddress(), spec_ins->getSpeculatedAddress(),
                        spec_ins->getTakenAddress(), pipeline_reset_addr,
                        spec_ins->getSpeculatedAddress() != pipeline_reset_addr ? "yes" : "no",
                        perform_pipeline_clear ? "yes" : "no", perform_delay_cleanup ? "yes" : "no");

                    // stop simulation
                    output->fatal(
                        CALL_INFO, -2,
                        "Retired instruction address 0x%llx, requested "
                        "terminate on retire this address.\n",
                        pause_on_retire_address);
                }
            }
        }

        // is the instruction completed (including anything like delay slots) and
        // can be cleared from the ROB
        if ( perform_cleanup ) {
            rob->pop();

#ifdef VANADIS_BUILD_DEBUG
            if ( output->getVerboseLevel() >= 8 ) {
                char* inst_asm_buffer = new char[32768];
                rob_front->printToBuffer(inst_asm_buffer, 32768);

                output->verbose(
                    CALL_INFO, 8, 0, "----> Retire: 0x%0llx / %s\n", rob_front->getInstructionAddress(),
                    inst_asm_buffer);

                delete[] inst_asm_buffer;
                if ( print_rob ) {
                    printRob(rob);
                }
            }
#endif
            if ( pipelineTrace != nullptr ) {
                fprintf(pipelineTrace, "0x%08llx %s\n", rob_front->getInstructionAddress(), rob_front->getInstCode());
            }

			if(UNLIKELY(rob_front->updatesFPFlags())) {
                output->verbose(CALL_INFO, 16, 0, "------> updating floating-point flags.\n");
				rob_front->updateFPFlags();
			}

            recoverRetiredRegisters(
                rob_front, int_register_stacks[ins_thread], fp_register_stacks[ins_thread],
                issue_isa_tables[ins_thread], retire_isa_tables[ins_thread]);

#ifdef VANADIS_BUILD_DEBUG
		    if(output->getVerboseLevel() >= 8) {
                if(print_retire_tables) {
                    retire_isa_tables[ins_thread]->print(output, register_files[ins_thread], print_int_reg, print_fp_reg, output->getVerboseLevel());
                }
                if(print_issue_tables) {
                    issue_isa_tables[ins_thread]->print(output, register_files[ins_thread], print_int_reg, print_fp_reg, output->getVerboseLevel());
                }
            }
#endif
			if(output->getVerboseLevel() >= 16) {
				fp_flags.at(ins_thread)->print(output);
			}

            ins_retired_this_cycle++;

            if ( perform_delay_cleanup ) {

                VanadisInstruction* delay_ins = rob->pop();
#ifdef VANADIS_BUILD_DEBUG
                output->verbose(
                    CALL_INFO, 8, 0, "----> Retire delay: 0x%llx / %s\n", delay_ins->getInstructionAddress(),
                    delay_ins->getInstCode());
#endif
                if ( pipelineTrace != nullptr ) {
                    fprintf(
                        pipelineTrace, "0x%08llx %s\n", delay_ins->getInstructionAddress(), delay_ins->getInstCode());
                }

				if(UNLIKELY(rob_front->updatesFPFlags())) {
                    output->verbose(CALL_INFO, 16, 0, "------> updating floating-point flags.\n");
					rob_front->updateFPFlags();
				}

                recoverRetiredRegisters(
                    delay_ins, int_register_stacks[delay_ins->getHWThread()],
                    fp_register_stacks[delay_ins->getHWThread()], issue_isa_tables[delay_ins->getHWThread()],
                    retire_isa_tables[delay_ins->getHWThread()]);

				if(output->getVerboseLevel() >= 16) {
					fp_flags.at(rob_front->getHWThread())->print(output);
			    }

                ins_retired_this_cycle++;

                delete delay_ins;
            }

            if ( output->getVerboseLevel() > 0 ) {
                if(print_retire_tables) {
                    retire_isa_tables[rob_front->getHWThread()]->print(
                        output, register_files[rob_front->getHWThread()], print_int_reg, print_fp_reg);
                }
            }

            if ( UNLIKELY(
                     (pause_on_retire_address > 0) &&
                     (rob_front->getInstructionAddress() == pause_on_retire_address)) ) {

                // print the register and pipeline status
                printStatus((*output));

                output->verbose(
                    CALL_INFO, 0, 0, "pipe-clear: %3s / delay-cleanup: %3s\n", perform_pipeline_clear ? "yes" : "no",
                    perform_delay_cleanup ? "yes" : "no");

                // stop simulation
                output->fatal(
                    CALL_INFO, -2,
                    "Retired instruction address 0x%llx, requested terminate "
                    "on retire this address.\n",
                    pause_on_retire_address);
            }

            if ( UNLIKELY(perform_pipeline_clear) ) {
#ifdef VANADIS_BUILD_DEBUG
                output->verbose(
                    CALL_INFO, 8, 0, "----> perform a pipeline clear thread %" PRIu32 ", reset to address: 0x%llx\n",
                    ins_thread, pipeline_reset_addr);
#endif
                handleMisspeculate(ins_thread, pipeline_reset_addr);

                stat_branch_mispredicts->addData(1);
            }

            delete rob_front;
        }
    }
    else {
        if ( UNLIKELY(INST_SYSCALL == rob_front->getInstFuncType()) ) {
            if ( rob_front->completedIssue() ) {
                // have we been marked front on ROB yet? if yes, then we have issued our
                // syscall
                if ( !rob_front->checkFrontOfROB() ) {
                    VanadisSysCallInstruction* the_syscall_ins = dynamic_cast<VanadisSysCallInstruction*>(rob_front);

                    if ( nullptr == the_syscall_ins ) {
                        output->fatal(
                            CALL_INFO, -1,
                            "Error: SYSCALL cannot be converted to an actual "
                            "sys-call instruction.\n");
                    }

#ifdef VANADIS_BUILD_DEBUG
                    output->verbose(
                        CALL_INFO, 8, 0,
                        "[syscall] -> calling OS handler in decode engine "
                        "(ins-addr: 0x%0llx)...\n",
                        the_syscall_ins->getInstructionAddress());
#endif
                    bool ret = thread_decoders[rob_front->getHWThread()]->getOSHandler()->handleSysCall(the_syscall_ins);

                    // mark as front of ROB now we can proceed
                    rob_front->markFrontOfROB();

                    if ( ret ) {
                        syscallReturn( rob_front->getHWThread() );
                    }
                }

                // We spent this cycle waiting on an issued SYSCALL, it has not resolved
                // at the emulated OS component yet so we have to wait, potentiallty for
                // a lot longer
                stat_syscall_cycles->addData(1);

                return INT_MAX;
            }
        }
        else {
            if ( ! rob_front->checkFrontOfROB() ) { 
                rob_front->markFrontOfROB(); 
            } else {
                // Return 2 because instruction is front of ROB but has not executed and
                // so we cannot make progress any more this cycle
                return 2;
            }
        }
    }

    return 0;
}

bool
VANADIS_COMPONENT::mapInstructiontoFunctionalUnit(
    VanadisInstruction* ins, std::vector<VanadisFunctionalUnit*>& functional_units)
{
    bool allocated = false;

    for ( VanadisFunctionalUnit* next_fu : functional_units ) {
        if ( LIKELY(next_fu->isInstructionSlotFree() )) {
            next_fu->insertInstruction(ins);
            allocated = true;

            if(output->getVerboseLevel() >= 16) {
                output->verbose(CALL_INFO, 16, 0, "------> mapped 0x%llx / %s to func-unit: %" PRIu16 "\n",
                    ins->getInstructionAddress(), ins->getInstCode(), next_fu->getUnitID());
            }

            break;
        }
    }

    return allocated;
}

int
VANADIS_COMPONENT::allocateFunctionalUnit(VanadisInstruction* ins)
{
    bool allocated_fu = false;

    switch ( ins->getInstFuncType() ) {
    case INST_INT_ARITH:
        allocated_fu = mapInstructiontoFunctionalUnit(ins, fu_int_arith);
        break;

    case INST_LOAD:
        if ( !lsq->loadFull() ) {
            stat_loads_issued->addData(1);
            lsq->push((VanadisLoadInstruction*)ins);
            allocated_fu = true;
        }
        break;

    case INST_STORE:
        if ( !lsq->storeFull() ) {
            stat_stores_issued->addData(1);
            lsq->push((VanadisStoreInstruction*)ins);
            allocated_fu = true;
        }
        break;
    
    case INST_BRANCH:
        allocated_fu = mapInstructiontoFunctionalUnit(ins, fu_branch);
        break;

    case INST_NOOP:
        ins->markExecuted();
        allocated_fu = true;
        break;

    case INST_FP_ARITH:
        allocated_fu = mapInstructiontoFunctionalUnit(ins, fu_fp_arith);
        break;

    case INST_INT_DIV:
        allocated_fu = mapInstructiontoFunctionalUnit(ins, fu_int_div);
        break;

    case INST_FP_DIV:
        allocated_fu = mapInstructiontoFunctionalUnit(ins, fu_fp_div);
        break;

    case INST_FENCE:
    {
        VanadisFenceInstruction* fence_ins = dynamic_cast<VanadisFenceInstruction*>(ins);

        if ( nullptr == fence_ins ) {
            output->fatal(
                CALL_INFO, -1,
                "Error: instruction (0x%0llx /thr: %" PRIu32 ") is a fence but not "
                "convertable to a fence instruction.\n",
                ins->getInstructionAddress(), ins->getHWThread());
        }

        lsq->push(fence_ins);
        allocated_fu = true;
    } break;

    case INST_SYSCALL:
        if ( lsq->storeBufferSize() == 0 && lsq->loadSize() == 0 ) { 
            allocated_fu = true;
        }
        break;

    case INST_FAULT:
        ins->markExecuted();
        allocated_fu = true;
        break;

    default:
        output->fatal(CALL_INFO, -1, "Error - no processing for instruction class (%s)\n", ins->getInstCode());
        break;
    }

    return allocated_fu ? 0 : 1;
}

bool
VANADIS_COMPONENT::tick(SST::Cycle_t cycle)
{
    if ( current_cycle >= max_cycle ) {
        output->verbose(CALL_INFO, 1, 0, "Reached maximum cycle %" PRIu64 ". Core stops processing.\n", current_cycle);
        primaryComponentOKToEndSim();
        return true;
    }

    const auto output_verbosity = output->getVerboseLevel();

    stat_cycles->addData(1);
    ins_issued_this_cycle  = 0;
    ins_retired_this_cycle = 0;
    ins_decoded_this_cycle = 0;

    bool should_process = false;
    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        should_process = should_process | halted_masks[i];
    }

#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 2) {
        output->verbose(
            CALL_INFO, 2, VANADIS_DBG_CYCLE_FLG, "============================ Cycle %12" PRIu64 " ============================\n",
            current_cycle);
    }

    if(output_verbosity >= 9) {
        output->verbose(CALL_INFO, 9, 0, "-- Core Status:\n");

        for ( uint32_t i = 0; i < hw_threads; ++i ) {
            
            output->verbose(
                CALL_INFO, 9, 0,
                "---> Thr: %5" PRIu32 " (%s) / ROB-Pend: %" PRIu16 " / IntReg-Free: %" PRIu16 " / FPReg-Free: %" PRIu16
                "\n",
                i, halted_masks[i] ? "halted" : "unhalted", (uint16_t)rob[i]->size(),
                (uint16_t)int_register_stacks[i]->unused(), (uint16_t)fp_register_stacks[i]->unused());
        }

        output->verbose(CALL_INFO, 9, 0, "-- Resetting Zero Registers\n");
    }
#endif

    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        const uint16_t zero_reg = isa_options[i]->getRegisterIgnoreWrites();

        if ( zero_reg < isa_options[i]->countISAIntRegisters() ) {
            VanadisISATable* thr_issue_table = issue_isa_tables[i];
            const uint16_t   zero_phys_reg   = thr_issue_table->getIntPhysReg(zero_reg);
            register_files[i]->setIntReg<uint64_t>(zero_phys_reg, 0);
        }
    }

    // Fetch
    // //////////////////////////////////////////////////////////////////////////
#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 9) {
        output->verbose(
            CALL_INFO, 9, 0,
            "=> Fetch Stage "
            "<==========================================================\n");
    }
#endif
    for ( uint32_t i = 0; i < fetches_per_cycle; ++i ) {
        if ( performFetch(cycle) != 0 ) { break; }
    }

    // Decode
    // //////////////////////////////////////////////////////////////////////////
#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 9) {
        output->verbose(
            CALL_INFO, 9, 0,
            "=> Decode Stage "
            "<==========================================================\n");
    }
#endif
    for ( uint32_t i = 0; i < decodes_per_cycle; ++i ) {
        if ( performDecode(cycle) != 0 ) { break; }
    }

    stat_ins_decoded->addData(ins_decoded_this_cycle);

    // Issue
    // //////////////////////////////////////////////////////////////////////////
#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 9) {
        output->verbose(
            CALL_INFO, 9, 0,
            "=> Issue Stage  "
            "<==========================================================\n");
    }
#endif
    // Clear our temps on a per-thread basis
    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        resetRegisterUseTemps(thread_decoders[i]->countISAIntReg(), thread_decoders[i]->countISAFPReg());
    }

    uint32_t rob_start   = 0;
    bool unallocated_memory_op_seen = false;

    // Attempt to perform issues, cranking through the entire ROB call by call or until we
    // reach the max issues this cycle
    for ( uint32_t i = 0; i < issues_per_cycle; ++i ) {
        if ( performIssue(cycle, rob_start, unallocated_memory_op_seen) != 0 ) { break; }
    }

    // Record how many instructions we issued this cycle
    stat_ins_issued->addData(ins_issued_this_cycle);

    // Execute
    // //////////////////////////////////////////////////////////////////////////
#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 9) {
        output->verbose(
            CALL_INFO, 9, 0,
            "=> Execute Stage "
            "<==========================================================\n");
    }
#endif
    performExecute(cycle);

    bool tick_return = false;

#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 9) {
        output->verbose(
            CALL_INFO, 9, 0,
            "=> Retire Stage "
            "<==========================================================\n");
    }
#endif
    // Retire
    // //////////////////////////////////////////////////////////////////////////
    for ( uint32_t i = 0; i < retires_per_cycle; ++i ) {
        for ( uint32_t j = 0; j < rob.size(); ++j ) {
            const int retire_rc = performRetire(rob[j], cycle);

            if ( retire_rc == INT_MAX ) {
                // we will return true and tell the handler not to clock us until
                // re-register
                tick_return = true;
#ifdef VANADIS_BUILD_DEBUG
                if(output_verbosity >= 8) {
                    output->verbose(
                        CALL_INFO, 8, 0,
                        "--> declocking core, result from retire is SYSCALL "
                        "pending front of ROB\n");
                }
#endif
            }
            else {
                // Signal from retire calls that we can't make progress is non-zero
                if ( retire_rc != 0 ) { break; }
            }
        }
    }

    // Record how many instructions we retired this cycle
    stat_ins_retired->addData(ins_retired_this_cycle);

    uint64_t rob_total_count = 0;
    for ( uint32_t i = 0; i < hw_threads; ++i ) {
        rob_total_count += rob[i]->size();
    }
    stat_rob_entries->addData(rob_total_count);

#ifdef VANADIS_BUILD_DEBUG
    if(output_verbosity >= 2) {
        output->verbose(
            CALL_INFO, 2, VANADIS_DBG_CYCLE_FLG,
            "================================ End of Cycle "
            "==============================\n");
    }
#endif

    current_cycle++;

    uint64_t used_phys_int = 0;
    uint64_t used_phys_fp  = 0;

    for ( uint16_t i = 0; i < hw_threads; ++i ) {
        VanadisRegisterStack* thr_reg_stack = int_register_stacks[i];
        used_phys_int += (thr_reg_stack->capacity() - thr_reg_stack->unused());

        thr_reg_stack = fp_register_stacks[i];
        used_phys_fp += (thr_reg_stack->capacity() - thr_reg_stack->unused());
    }

    stat_int_phys_regs_in_use->addData(used_phys_int);
    stat_fp_phys_regs_in_use->addData(used_phys_fp);

    if ( current_cycle >= max_cycle ) {
        output->verbose(CALL_INFO, 1, 0, "Reached maximum cycle %" PRIu64 ". Core stops processing.\n", current_cycle);
        //primaryComponentOKToEndSim();
        return true;
    }
    else {
        return tick_return;
    }
}

int
VANADIS_COMPONENT::checkInstructionResources(
    VanadisInstruction* ins, VanadisRegisterStack* int_regs, VanadisRegisterStack* fp_regs, VanadisISATable* isa_table)
{
    bool      resources_good   = true;
    const int output_verbosity = output->getVerboseLevel();

    const uint16_t int_reg_in_count = ins->countISAIntRegIn();
    const uint16_t int_reg_out_count = ins->countISAIntRegOut();
    const uint16_t fp_reg_in_count = ins->countISAFPRegIn();
    const uint16_t fp_reg_out_count = ins->countISAFPRegOut();
    
    // We need places to store our output registers
    resources_good &= (int_regs->unused() >= int_reg_out_count) && (fp_regs->unused() >= fp_reg_out_count);

    if ( UNLIKELY(!resources_good )) {
#ifdef VANADIS_BUILD_DEBUG
        output->verbose(
            CALL_INFO, 16, 0,
            "----> insufficient output / req: int: %" PRIu16 " fp: %" PRIu16 " / free: int: %" PRIu16 " fp: %" PRIu16
            "\n",
            (uint16_t)ins->countISAIntRegOut(), (uint16_t)ins->countISAFPRegOut(), (uint16_t)int_regs->unused(),
            (uint16_t)fp_regs->unused());
#endif
        return 1;
    }

    // If there are any pending writes against our reads, we can't issue until
    // they are done
    
    for ( uint16_t i = 0; i < int_reg_in_count; ++i ) {
        const uint16_t ins_isa_reg = ins->getISAIntRegIn(i);
        resources_good &= (!isa_table->pendingIntWrites(ins_isa_reg)) && (!tmp_int_reg_write[ins_isa_reg]);
    }

#ifdef VANADIS_BUILD_DEBUG
    if ( output_verbosity >= 16 ) {
        output->verbose(
            CALL_INFO, 16, 0, "--> Check input integer registers, issue-status: %s\n", (resources_good ? "yes" : "no"));
    }
#endif

    if ( LIKELY(!resources_good )) { return 2; }

    for ( uint16_t i = 0; i < fp_reg_in_count; ++i ) {
        const uint16_t ins_isa_reg = ins->getISAFPRegIn(i);
        resources_good &= (!isa_table->pendingFPWrites(ins_isa_reg)) & (!tmp_fp_reg_write[ins_isa_reg]);
    }

#ifdef VANADIS_BUILD_DEBUG
    if ( output_verbosity >= 16 ) {
        output->verbose(
            CALL_INFO, 16, 0, "--> Check input floating-point registers, issue-status: %s\n",
            (resources_good ? "yes" : "no"));
    }
#endif

    if ( UNLIKELY(!resources_good )) { return 3; }

    for ( uint16_t i = 0; i < int_reg_out_count; ++i ) {
        const uint16_t ins_isa_reg = ins->getISAIntRegOut(i);

        // Check there are no RAW in the pending instruction queue
        resources_good &= (!tmp_not_issued_int_reg_read[ins_isa_reg]) && (!tmp_int_reg_write[ins_isa_reg]);
    }

#ifdef VANADIS_BUILD_DEBUG
    if ( output_verbosity >= 16 ) {
        output->verbose(
            CALL_INFO, 16, 0, "--> Check output integer registers, issue-status: %s\n",
            (resources_good ? "yes" : "no"));
    }
#endif

    if ( LIKELY(!resources_good )) { return 4; }

    for ( uint16_t i = 0; i < fp_reg_out_count; ++i ) {
        const uint16_t ins_isa_reg = ins->getISAFPRegOut(i);

        // Check there are no RAW in the pending instruction queue
        resources_good &= (!tmp_not_issued_fp_reg_read[ins_isa_reg]) && (!tmp_fp_reg_write[ins_isa_reg]);
    }

#ifdef VANADIS_BUILD_DEBUG
    if ( output_verbosity >= 16 ) {
        output->verbose(
            CALL_INFO, 16, 0, "--> Check output floating-point registers, issue-status: %s\n",
            (resources_good ? "yes" : "no"));
    }
#endif

    if ( UNLIKELY(!resources_good )) { return 5; }

    return 0;
}

int
VANADIS_COMPONENT::assignRegistersToInstruction(
    const uint16_t int_reg_count, const uint16_t fp_reg_count, VanadisInstruction* ins, VanadisRegisterStack* int_regs,
    VanadisRegisterStack* fp_regs, VanadisISATable* isa_table)
{

    // PROCESS INPUT REGISTERS
    // ///////////////////////////////////////////////////////

    // Set the current ISA registers required for input
    for ( uint16_t i = 0; i < ins->countISAIntRegIn(); ++i ) {
        if ( ins->getISAIntRegIn(i) >= int_reg_count ) {
            output->fatal(
                CALL_INFO, -1, "Error: ins request in-int-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                ins->getISAIntRegIn(i), int_reg_count);
        }

        const uint16_t isa_reg_in = ins->getISAIntRegIn(i);
        const uint16_t phys_reg_in = isa_table->getIntPhysReg(isa_reg_in);

        if(output->getVerboseLevel() >= 16) {
            output->verbose(CALL_INFO, 16, 0, "-----> creating ins-addr: 0x%llx int reg-in for isa: %" PRIu16 " will mapped to phys: %" PRIu16 "\n",
                ins->getInstructionAddress(), isa_reg_in, phys_reg_in);
        }

        ins->setPhysIntRegIn(i, phys_reg_in);
        isa_table->incIntRead(isa_reg_in);
    }

    for ( uint16_t i = 0; i < ins->countISAFPRegIn(); ++i ) {
        if ( ins->getISAFPRegIn(i) >= fp_reg_count ) {
            output->fatal(
                CALL_INFO, -1, "Error: ins request in-fp-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                ins->getISAFPRegIn(i), fp_reg_count);
        }

        const uint16_t isa_reg_in = ins->getISAFPRegIn(i);
        const uint16_t phys_reg_in = isa_table->getFPPhysReg(isa_reg_in);

        if(output->getVerboseLevel() >= 16) {
            output->verbose(CALL_INFO, 16, 0, "-----> creating ins-addr: 0x%llx fp reg-in for isa: %" PRIu16 " will mapped to phys: %" PRIu16 "\n",
                ins->getInstructionAddress(), isa_reg_in, phys_reg_in);
        }

        ins->setPhysFPRegIn(i, phys_reg_in);
        isa_table->incFPRead(isa_reg_in);
    }

    // PROCESS OUTPUT REGISTERS
    // ///////////////////////////////////////////////////////

    // SYSCALLs have special handling because they request *every* register to
    // lock up the pipeline. We just give them full access to the register file
    // without requiring anything from the register file (otherwise we exhaust
    // registers). This REQUIRES that SYSCALL doesn't muck with the registers
    // itself at execute and relies on the OS handlers, otherwise this is not
    // out-of-order compliant (since mis-predicts would corrupt the register file
    // contents
    if ( UNLIKELY(INST_SYSCALL == ins->getInstFuncType()) ) {
        for ( uint16_t i = 0; i < ins->countISAIntRegOut(); ++i ) {
            if ( UNLIKELY(ins->getISAIntRegOut(i) >= int_reg_count) ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Error: ins request out-int-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                    ins->getISAIntRegOut(i), int_reg_count);
            }

            const uint16_t ins_isa_reg = ins->getISAIntRegOut(i);
            const uint16_t out_reg     = isa_table->getIntPhysReg(ins_isa_reg);

            ins->setPhysIntRegOut(i, out_reg);
            isa_table->incIntWrite(ins_isa_reg);
        }

        // Set current ISA registers required for output
        for ( uint16_t i = 0; i < ins->countISAFPRegOut(); ++i ) {
            if ( UNLIKELY(ins->getISAFPRegOut(i) >= fp_reg_count) ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Error: ins request out-fp-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                    ins->getISAFPRegOut(i), fp_reg_count);
            }

            const uint16_t ins_isa_reg = ins->getISAFPRegOut(i);
            const uint16_t out_reg     = isa_table->getFPPhysReg(ins_isa_reg);

            ins->setPhysFPRegOut(i, out_reg);
            isa_table->incFPWrite(ins_isa_reg);
        }
    }
    else {
        // Set current ISA registers required for output
        for ( uint16_t i = 0; i < ins->countISAIntRegOut(); ++i ) {
            const uint16_t ins_isa_reg    = ins->getISAIntRegOut(i);

            if ( UNLIKELY(ins_isa_reg >= int_reg_count) ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Error: ins request out-int-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                    ins_isa_reg, int_reg_count);
            }

            const uint16_t out_reg        = int_regs->pop();

            if(output->getVerboseLevel() >= 16) {
                output->verbose(CALL_INFO, 16, 0, "-----> creating ins-addr: 0x%llx int reg-out for isa: %" PRIu16 " output will map to phys: %" PRIu16 "\n",
                    ins->getInstructionAddress(), ins_isa_reg, out_reg);
            }

            isa_table->setIntPhysReg(ins_isa_reg, out_reg);
            isa_table->incIntWrite(ins_isa_reg);

            ins->setPhysIntRegOut(i, out_reg);
        }

        // Set current ISA registers required for output
        for ( uint16_t i = 0; i < ins->countISAFPRegOut(); ++i ) {
            const uint16_t ins_isa_reg = ins->getISAFPRegOut(i);

            if ( UNLIKELY(ins_isa_reg >= fp_reg_count) ) {
                output->fatal(
                    CALL_INFO, -1,
                    "Error: ins request out-fp-reg: %" PRIu16 " but ISA has only %" PRIu16 " available\n",
                    ins_isa_reg, fp_reg_count);
            }

            const uint16_t out_reg     = fp_regs->pop();

            if(output->getVerboseLevel() >= 16) {
                output->verbose(CALL_INFO, 16, 0, "-----> creating ins-addr: 0x%llx fp reg-out for isa: %" PRIu16 " output will map to phys: %" PRIu16 "\n",
                    ins->getInstructionAddress(), ins_isa_reg, out_reg);
            }
            
            isa_table->setFPPhysReg(ins_isa_reg, out_reg);
            isa_table->incFPWrite(ins_isa_reg);

            ins->setPhysFPRegOut(i, out_reg);
        }
    }

    return 0;
}

int
VANADIS_COMPONENT::recoverRetiredRegisters(
    VanadisInstruction* ins, VanadisRegisterStack* int_regs, VanadisRegisterStack* fp_regs,
    VanadisISATable* issue_isa_table, VanadisISATable* retire_isa_table)
{
    std::vector<uint16_t> recovered_phys_reg_int;
    std::vector<uint16_t> recovered_phys_reg_fp;

    const uint16_t count_int_reg_in = ins->countISAIntRegIn();
    for ( uint16_t i = 0; i < count_int_reg_in; ++i ) {
        const uint16_t isa_reg = ins->getISAIntRegIn(i);
        issue_isa_table->decIntRead(isa_reg);
    }

    const uint16_t count_fp_reg_in = ins->countISAFPRegIn();
    for ( uint16_t i = 0; i < count_fp_reg_in; ++i ) {
        const uint16_t isa_reg = ins->getISAFPRegIn(i);
        issue_isa_table->decFPRead(isa_reg);
    }

    const uint16_t count_int_reg_out = ins->countISAIntRegOut();
    if ( ins->performIntRegisterRecovery() ) {
        for ( uint16_t i = 0; i < count_int_reg_out; ++i ) {
            const uint16_t isa_reg      = ins->getISAIntRegOut(i);

            // We are about to set a new physical register to hold the ISA value
            // so recover the current holder ready to be issued further up.
            const uint16_t cur_phys_reg = retire_isa_table->getIntPhysReg(isa_reg);

            issue_isa_table->decIntWrite(isa_reg);

            recovered_phys_reg_int.push_back(cur_phys_reg);

            // Set the ISA register in the retirement table to point
            // to the physical register used by this instruction
            retire_isa_table->setIntPhysReg(isa_reg, ins->getPhysIntRegOut(i));
        }
    }
    else {
#ifdef VANADIS_BUILD_DEBUG
        output->verbose(CALL_INFO, 16, 0, "-> instruction bypasses integer register recovery\n");
#endif
        for ( uint16_t i = 0; i < count_int_reg_out; ++i ) {
            const uint16_t isa_reg      = ins->getISAIntRegOut(i);
            const uint16_t cur_phys_reg = retire_isa_table->getIntPhysReg(isa_reg);

            issue_isa_table->decIntWrite(isa_reg);
        }
    }

    const uint16_t count_fp_reg_out = ins->countISAFPRegOut();
    if ( ins->performFPRegisterRecovery() ) {
        for ( uint16_t i = 0; i < count_fp_reg_out; ++i ) {
            const uint16_t isa_reg      = ins->getISAFPRegOut(i);
            const uint16_t cur_phys_reg = retire_isa_table->getFPPhysReg(isa_reg);

            issue_isa_table->decFPWrite(isa_reg);

            recovered_phys_reg_fp.push_back(cur_phys_reg);

            // Set the ISA register in the retirement table to point
            // to the physical register used by this instruction
            retire_isa_table->setFPPhysReg(isa_reg, ins->getPhysFPRegOut(i));
        }
    }
    else {
#ifdef VANADIS_BUILD_DEBUG
        output->verbose(CALL_INFO, 16, 0, "-> instruction bypasses fp register recovery\n");
#endif
        for ( uint16_t i = 0; i < count_fp_reg_out; ++i ) {
            const uint16_t isa_reg      = ins->getISAFPRegOut(i);
            const uint16_t cur_phys_reg = retire_isa_table->getFPPhysReg(isa_reg);

            issue_isa_table->decFPWrite(isa_reg);
        }
    }

#ifdef VANADIS_BUILD_DEBUG
    if(output->getVerboseLevel() >= 16) {
        output->verbose(
            CALL_INFO, 16, 0, "Recovering: %d int-reg and %d fp-reg\n", (int)recovered_phys_reg_int.size(),
            (int)recovered_phys_reg_fp.size());
    }
#endif

    for ( uint16_t next_reg : recovered_phys_reg_int ) {
        int_regs->push(next_reg);
    }

    for ( uint16_t next_reg : recovered_phys_reg_fp ) {
        fp_regs->push(next_reg);
    }

    return 0;
}

void
VANADIS_COMPONENT::setup()
{}

void
VANADIS_COMPONENT::finish()
{}

void
VANADIS_COMPONENT::printStatus(SST::Output& output)
{
    output.verbose(
        CALL_INFO, 0, 0,
        "------------------------------------------------------------------------"
        "----------------------------------------------------\n");
    output.verbose(
        CALL_INFO, 0, 0,
        "Vanadis (Core: %" PRIu16 " / Threads: %" PRIu32 " / cycle: %" PRIu64 " / max-cycle: %" PRIu64 ")\n", core_id,
        hw_threads, current_cycle, max_cycle);
    output.verbose(CALL_INFO, 0, 0, "\n");

    uint32_t thread_num = 0;

    for ( VanadisCircularQueue<VanadisInstruction*>* next_rob : rob ) {
        output.verbose(CALL_INFO, 0, 0, "-> ROB Information for Thread %" PRIu32 "\n", thread_num++);
        for ( size_t i = next_rob->size(); i > 0; i-- ) {
            VanadisInstruction* next_ins = next_rob->peekAt(i - 1);
            output.verbose(
                CALL_INFO, 0, 0,
                "---> rob[%5" PRIu16 "]: addr: 0x%08llx / %10s / spec: %3s / err: "
                "%3s / issued: %3s / front: %3s / exe: %3s\n",
                (uint16_t)(i - 1), next_ins->getInstructionAddress(), next_ins->getInstCode(),
                next_ins->isSpeculated() ? "yes" : "no", next_ins->trapsError() ? "yes" : "no",
                next_ins->completedIssue() ? "yes" : "no", next_ins->checkFrontOfROB() ? "yes" : "no",
                next_ins->completedExecution() ? "yes" : "no");
        }
    }

    output.verbose(CALL_INFO, 0, 0, "\n");
    output.verbose(CALL_INFO, 0, 0, "-> LSQ-Size: %" PRIu32 "\n", (uint32_t)(lsq->storeSize() + lsq->loadSize()));
    lsq->printStatus(output);
    output.verbose(
        CALL_INFO, 0, 0,
        "------------------------------------------------------------------------"
        "----------------------------------------------------\n");
}

void
VANADIS_COMPONENT::init(unsigned int phase)
{
    output->verbose(CALL_INFO, 2, 0, "Start: init-phase: %" PRIu32 "...\n", (uint32_t)phase);
    output->verbose(CALL_INFO, 2, 0, "-> Initializing memory interfaces with this phase...\n");

    lsq->init(phase);
    //	memDataInterface->init( phase );
    memInstInterface->init(phase);

    while (SST::Event* ev = os_link->recvInitData()) {

        assert( 0 );
        VanadisStartThreadReq * req = dynamic_cast<VanadisStartThreadReq*>(ev);
        if (nullptr == req) {
             output->fatal(CALL_INFO, -1, "Error - event cannot be StartThreadReq\n");
        }

        output->verbose(CALL_INFO, 8, 0,
                            "received start thread %d command from the operating system \n",req->getThread());
        startThread(req->getThread(), req->getStackStart(), req->getInstructionPointer());
        delete ev;
    }

    output->verbose(CALL_INFO, 2, 0, "End: init-phase: %" PRIu32 "...\n", (uint32_t)phase);
}

// void VANADIS_COMPONENT::handleIncomingDataCacheEvent( SimpleMem::Request* ev
// ) { 	output->verbose(CALL_INFO, 16, 0, "-> Incoming d-cache event
//(addr=%p)...\n", 		(void*) ev->addr); 	lsq->processIncomingDataCacheEvent(
// output, ev );
// }

void
VANADIS_COMPONENT::handleIncomingInstCacheEvent(StandardMem::Request* ev)
{
#ifdef VANADIS_BUILD_DEBUG
    StandardMem::ReadResp* read_resp = static_cast<StandardMem::ReadResp*>(ev);

    output->verbose(
        CALL_INFO, 16, 0, "-> Incoming i-cache event (addr=0x%" PRIx64 ")...\n", read_resp->pAddr);
#endif
    // Needs to get attached to the decoder
    bool hit = false;

    for ( VanadisDecoder* next_decoder : thread_decoders ) {
        if ( next_decoder->acceptCacheResponse(output, ev) ) {
            hit = true;
            break;
        }
    }

    if ( hit ) { output->verbose(CALL_INFO, 16, 0, "---> Successful hit in hardware-thread decoders.\n"); }

    delete ev;
}

void
VANADIS_COMPONENT::handleMisspeculate(const uint32_t hw_thr, const uint64_t new_ip)
{
#ifdef VANADIS_BUILD_DEBUG
    if(output->getVerboseLevel() >= 16) {
        output->verbose(CALL_INFO, 16, 0, "-> Handle mis-speculation on %" PRIu32 " (new-ip: 0x%llx)...\n", hw_thr, new_ip);
    }
#endif
    clearFuncUnit(hw_thr, fu_int_arith);
    clearFuncUnit(hw_thr, fu_int_div);
    clearFuncUnit(hw_thr, fu_fp_arith);
    clearFuncUnit(hw_thr, fu_fp_div);
    clearFuncUnit(hw_thr, fu_branch);

    lsq->clearLSQByThreadID(hw_thr);
    resetRegisterStacks(hw_thr);
    clearROBMisspeculate(hw_thr);

    // Reset the ISA table to get correct ISA to physical mappings
    issue_isa_tables[hw_thr]->reset(retire_isa_tables[hw_thr]);

    // Notify the decoder we need a clear and reset to new instruction pointer
    thread_decoders[hw_thr]->setInstructionPointerAfterMisspeculate(output, new_ip);

    if(output->getVerboseLevel() >= 16) {
        output->verbose(CALL_INFO, 16, 0, "-> Mis-speculate repair finished.\n");
    }
}

void
VANADIS_COMPONENT::clearFuncUnit(const uint32_t hw_thr, std::vector<VanadisFunctionalUnit*>& unit)
{
    for ( VanadisFunctionalUnit* next_fu : unit ) {
        next_fu->clearByHWThreadID(output, hw_thr);
    }
}

void
VANADIS_COMPONENT::resetRegisterStacks(const uint32_t hw_thr)
{
    const uint16_t int_reg_count = int_register_stacks[hw_thr]->capacity();
#ifdef VANADIS_BUILD_DEBUG
    if(output->getVerboseLevel() >= 16) {
        output->verbose(CALL_INFO, 16, 0, "-> Resetting register stacks on thread %" PRIu32 "...\n", hw_thr);
        output->verbose(CALL_INFO, 16, 0, "---> Reclaiming integer registers...\n");
        output->verbose(
            CALL_INFO, 16, 0, "---> Creating a new int register stack with %" PRIu16 " registers...\n", int_reg_count);
    }
#endif
    VanadisRegisterStack* thr_int_stack = int_register_stacks[hw_thr];
    thr_int_stack->clear();

    for ( uint16_t i = 0; i < int_reg_count; ++i ) {
        if ( !retire_isa_tables[hw_thr]->physIntRegInUse(i) ) { thr_int_stack->push(i); }
    }

    //	delete int_register_stacks[hw_thr];
    //	int_register_stacks[hw_thr] = new_int_stack;
    const uint16_t fp_reg_count = fp_register_stacks[hw_thr]->capacity();
#ifdef VANADIS_BUILD_DEBUG
    if(output->getVerboseLevel() >= 16) {
        output->verbose(
            CALL_INFO, 16, 0, "---> Integer register stack contains %" PRIu32 " registers.\n",
            (uint32_t)thr_int_stack->unused());
        output->verbose(CALL_INFO, 16, 0, "---> Reclaiming floating point registers...\n");

        output->verbose(
            CALL_INFO, 16, 0, "---> Creating a new fp register stack with %" PRIu16 " registers...\n", fp_reg_count);
    }
#endif
    //	VanadisRegisterStack* new_fp_stack = new VanadisRegisterStack(
    // fp_reg_count );
    VanadisRegisterStack* thr_fp_stack = fp_register_stacks[hw_thr];
    thr_fp_stack->clear();

    for ( uint16_t i = 0; i < fp_reg_count; ++i ) {
        if ( !retire_isa_tables[hw_thr]->physFPRegInUse(i) ) { thr_fp_stack->push(i); }
    }

    //	delete fp_register_stacks[hw_thr];
    //	fp_register_stacks[hw_thr] = new_fp_stack;

#ifdef VANADIS_BUILD_DEBUG
    if(output->getVerboseLevel() >= 16) {
        output->verbose(
            CALL_INFO, 16, 0, "---> Floating point stack contains %" PRIu32 " registers.\n",
            (uint32_t)thr_fp_stack->unused());
    }
#endif
}

void
VANADIS_COMPONENT::clearROBMisspeculate(const uint32_t hw_thr)
{
    VanadisCircularQueue<VanadisInstruction*>* thr_rob = rob[hw_thr];
    stat_rob_cleared_entries->addData(thr_rob->size());

    // Delete all the instructions which we aren't going to process
    for ( size_t i = 0; i < thr_rob->size(); ++i ) {
        VanadisInstruction* next_ins = thr_rob->peekAt(i);
        delete next_ins;
    }

    // clear the ROB entries and reset
    thr_rob->clear();
}

void
VANADIS_COMPONENT::syscallReturn(uint32_t thr)
{
    if ( rob[thr]->empty() ) {
        output->fatal(CALL_INFO, -1, "Error - syscall return called on thread: %" PRIu32 " but ROB is empty.\n", thr);
    }

    VanadisInstruction*        rob_front   = rob[thr]->peek();
    VanadisSysCallInstruction* syscall_ins = dynamic_cast<VanadisSysCallInstruction*>(rob_front);

    if ( nullptr == syscall_ins ) {
        output->fatal(
            CALL_INFO, -1,
            "Error - unable to obtain a syscall from the ROB front of "
            "thread %" PRIu32 " (code: %s)\n",
            thr, rob_front->getInstCode());
    }

#ifdef VANADIS_BUILD_DEBUG
    output->verbose(
        CALL_INFO, 16, 0,
        "[syscall-return]: syscall on thread %" PRIu32 " (0x%0llx) is completed, return to processing.\n", thr,
        syscall_ins->getInstructionAddress());
#endif
    syscall_ins->markExecuted();

    // re-register the CPU clock, it will fire on the next cycle
    reregisterClock(cpuClockTC, cpuClockHandler);
}

void VANADIS_COMPONENT::recvOSEvent(SST::Event* ev) {
    output->verbose(CALL_INFO, 8, 0, "-> recv os response\n");

    VanadisSyscallResponse* os_resp = dynamic_cast<VanadisSyscallResponse*>(ev);

    if (nullptr != os_resp) {
        int hw_thr = os_resp->getHWThread();

        output->verbose(CALL_INFO, 8, 0, "hw_thread %d: syscall return-code: %" PRId64 " (success: %3s)\n",
                        hw_thr, os_resp->getReturnCode(), os_resp->isSuccessful() ? "yes" : "no");
        output->verbose(CALL_INFO, 8, 0, "-> issuing call-backs to clear syscall ROB stops...\n");
        
        thread_decoders[hw_thr]->getOSHandler()->recvSyscallResp ( os_resp ); 
        syscallReturn( hw_thr );
    } else {
        VanadisStartThreadReq* os_req = dynamic_cast<VanadisStartThreadReq*>(ev);
        if ( nullptr != os_req ) {
            output->verbose(CALL_INFO, 0, 0,
                            "received start thread %d command from the operating system \n",os_req->getThread());

            int hw_thr = os_req->getThread();

            if ( os_req->getStartArg() ) {

                auto isa_table = retire_isa_tables[hw_thr];
                auto reg_file = register_files[hw_thr];

                auto is = int_register_stacks[hw_thr];

                resetHwThread( hw_thr );

                reregisterClock(cpuClockTC, cpuClockHandler);

#if 0
                reg_file->setIntReg(isa_table->getIntPhysReg(4), os_req->getStartArg());
                reg_file->setIntReg(isa_table->getIntPhysReg(25), os_req->getInstructionPointer());
                reg_file->setIntReg(isa_table->getIntPhysReg(29), os_req->getStackStart());
#endif
printf("%s() %#lx %#lx\n",__func__, os_req->getInstructionPointer(), os_req->getStackStart());

                thread_decoders[hw_thr]->setStackPointer( output, issue_isa_tables[hw_thr], reg_file, os_req->getStackStart() );

                thread_decoders[hw_thr]->setThreadLocalStoragePointer( os_req->getTlsAddr() );

                halted_masks[hw_thr]            = false;
                handleMisspeculate( hw_thr, os_req->getInstructionPointer() );
            } else {
                startThread(hw_thr, os_req->getStackStart(), os_req->getInstructionPointer());
            }
        } else {

           VanadisExitResponse* os_exit = dynamic_cast<VanadisExitResponse*>(ev);

            if ( nullptr != os_exit ) {
                output->verbose(CALL_INFO, 2, 0,
                            "received an exit command from the operating system for hw_thr %d "
                            "(return-code: %" PRId64 " )\n",
                            os_exit->getHWThread(),
                            os_exit->getReturnCode());

                setHalt(os_exit->getHWThread(), os_exit->getReturnCode());
            } else {
                VanadisGetThreadStateReq* os_req = dynamic_cast<VanadisGetThreadStateReq*>(ev);
                if ( nullptr != os_req ) {
                    int hw_thr = os_req->getThread();

                    auto thr_decoder = thread_decoders[hw_thr];
                    auto isa_table = retire_isa_tables[hw_thr];
                    auto reg_file = register_files[hw_thr];
                    uint64_t instPtr = rob[hw_thr]->peek()->getInstructionAddress();
                    uint64_t stackPtr = reg_file->getIntReg<uint64_t>( isa_table->getIntPhysReg( 29 ) );
                    uint64_t tlsPtr = thread_decoders[hw_thr]->getThreadLocalStoragePointer();

                    output->verbose(CALL_INFO, 2, 0,"get thread state, hw_th=%d instPtr=%lx stackPtr=%lx tlsPtr=%lx\n",hw_thr,instPtr,stackPtr,tlsPtr);

                    VanadisGetThreadStateResp* resp = new VanadisGetThreadStateResp( core_id, hw_thr, instPtr, tlsPtr );
                    for ( int i = 0; i < isa_table->getNumIntRegs(); i++ ) {
                        uint64_t val = reg_file->getIntReg<uint64_t>( isa_table->getIntPhysReg( i ) );
                        resp->intRegs.push_back( val );
                    }
                    for ( int i = 0; i < isa_table->getNumFpRegs(); i++ ) {
                        if ( thr_decoder->getFPRegisterMode() == VANADIS_REGISTER_MODE_FP32 ) {
                            uint32_t val = reg_file->getFPReg<uint32_t>( isa_table->getFPPhysReg( i ) );
                            resp->fpRegs.push_back( val );
                        } else {
                            uint64_t val = reg_file->getFPReg<uint64_t>( isa_table->getFPPhysReg( i ) );
                            resp->fpRegs.push_back( val );
                        }
                    }
                    os_link->send( resp );
                } else {
                    VanadisStartThreadFullReq* full_start = dynamic_cast<VanadisStartThreadFullReq*>(ev);
                    if ( nullptr != full_start ) {
                        int hw_thr = full_start->getThread();
                        auto thr_decoder = thread_decoders[hw_thr];
                        auto isa_table = retire_isa_tables[hw_thr];
                        auto reg_file = register_files[hw_thr];

                        output->verbose(CALL_INFO, 0, 0,"start thread full, thread=%d instPtr=%lx tlsPtr=%lx\n",
                                full_start->getThread(), full_start->getInstPtr(), full_start->getTlsPtr() );

                        for ( int i = 0; i < full_start->intRegs.size(); i++ ) {
                            reg_file->setIntReg(isa_table->getIntPhysReg(i), full_start->intRegs[i]);
                        }
                        for ( int i = 0; i < full_start->fpRegs.size(); i++ ) {
                            if ( VANADIS_REGISTER_MODE_FP32 == thr_decoder->getFPRegisterMode() ) {
                                reg_file->setFPReg<uint32_t>(isa_table->getFPPhysReg(i), full_start->fpRegs[i]);
                            } else {
                                reg_file->setFPReg<uint64_t>(isa_table->getFPPhysReg(i), full_start->fpRegs[i]);
                            }
                        }
                        thread_decoders[hw_thr]->setThreadLocalStoragePointer( full_start->getTlsPtr() );
                        halted_masks[hw_thr]            = false;
                        handleMisspeculate( hw_thr, full_start->getInstPtr() );

                    } else {
                        VanadisDumpRegsReq* req = dynamic_cast<VanadisDumpRegsReq*>(ev);
                        if (nullptr != req ) {
                            int hw_thr = req->getThread();
                            auto reg_file = register_files[hw_thr];
                            output->verbose(CALL_INFO, 0, 0,"=======================================================================================\n");
                            output->verbose(CALL_INFO, 0, 0,"Memory Fault: dump registers and exit\n");
                            // print the register and pipeline status
                            output->setVerboseLevel( 16 );
                            printStatus((*output));
#if 0
                            output->verbose(CALL_INFO, 0, 0,"issue isa table\n");
                            issue_isa_tables[hw_thr]->print(output, reg_file, true, false, 0);
                            output->verbose(CALL_INFO, 0, 0," isa table\n");
                            retire_isa_tables[hw_thr]->print(output, reg_file, true, false, 0);
#endif
                            output->verbose(CALL_INFO, 0, 0,"=======================================================================================\n");
                            exit(0);

                        } else {
                            assert(0);
                        }
                    }
                }
            }
        }
        delete ev;
    }
}

void
VANADIS_COMPONENT::resetHwThread(uint32_t thr)
{
    auto decoder = thread_decoders[thr]; 
    auto issue_table = issue_isa_tables[thr];
    auto retire_table = retire_isa_tables[thr];
    auto int_stack = int_register_stacks[thr];
    auto fp_stack = fp_register_stacks[thr];
    auto reg_file = register_files[thr];
    auto thr_rob = rob[thr];

    thr_rob->clear();

#if 0
    output->setVerboseLevel( 16 );
    output->verbose(CALL_INFO, 0, 0,"%s() issue isa table\n",__func__);
    issue_table->print(output, reg_file, true, false, 0);
    output->verbose(CALL_INFO, 0, 0,"%s90 retireisa table\n",__func__);
    retire_table->print(output, reg_file, true, false, 0);
#endif

    decoder->getInstructionLoader()->clearCache();
    
    reg_file->init();
    
    int_stack->reset();
    fp_stack->reset();
    issue_table->init();
    retire_table->init();

    for ( uint16_t j = 0; j < decoder->countISAIntReg(); ++j ) {
        issue_table->setIntPhysReg(j, int_stack->pop());
    }

    for ( uint16_t j = 0; j < decoder->countISAFPReg(); ++j ) {
        issue_table->setFPPhysReg(j, fp_stack->pop());
    }

    retire_table->reset(issue_table);
#if 0
    output->verbose(CALL_INFO, 0, 0,"%s() issue isa table\n",__func__);
    issue_table->print(output, reg_file, true, false, 0);
    output->verbose(CALL_INFO, 0, 0,"%s90 retireisa table\n",__func__);
    retire_table->print(output, reg_file, true, false, 0);
    output->setVerboseLevel( 0 );
#endif
}
