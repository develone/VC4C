/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Optimizer.h"
#include "Inliner.h"
#include "LiteralValues.h"
#include "Eliminator.h"
#include "Reordering.h"
#include "Combiner.h"
#include "MemoryAccess.h"
#include "../intrinsics/Intrinsics.h"
#include "../Profiler.h"
#include "../BackgroundWorker.h"
#include "log.h"

using namespace vc4c;
using namespace vc4c::optimizations;

OptimizationPass::OptimizationPass(const std::string& name, const Pass pass, const std::size_t index) : name(name), index(index), pass(pass)
{
}

bool OptimizationPass::operator <(const OptimizationPass& other) const
{
	return index < other.index;
}

void OptimizationPass::operator ()(const Module& module, Method& method, const Configuration& config) const
{
	pass(module, method, config);
}

bool OptimizationPass::operator ==(const OptimizationPass& other) const
{
	return name.compare(other.name) == 0 && index == other.index;
}

OptimizationStep::OptimizationStep(const std::string& name, const Step step, const std::size_t index) : name(name), index(index), step(step)
{
}

bool OptimizationStep::operator <(const OptimizationStep& other) const
{
	return index < other.index;
}

InstructionWalker OptimizationStep::operator ()(const Module& module, Method& method, InstructionWalker it, const Configuration& config) const
{
	return step(module, method, it, config);
}

bool OptimizationStep::operator ==(const OptimizationStep& other) const
{
	return name.compare(other.name) == 0 && index == other.index;
}

static InstructionWalker checkMethodCalls(const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
	if(it.has<intermediate::MethodCall>())
	{
		throw CompilationError(CompilationStep::OPTIMIZER, "There should be no more function-calls", it->to_string());
	}
	return it;
}

static const std::set<OptimizationStep> SINGLE_STEPS = {
		//replaces all remaining returns with jumps to the end of the kernel-function
		OptimizationStep("EliminateReturns", eliminateReturn, 0),
		//combined successive branches to the same label (e.g. end of switch-case)
		OptimizationStep("CombineDuplicateBranches", combineDuplicateBranches, 10),
		//eliminates useless branches (e.g. jumps to the next instruction)
		OptimizationStep("EliminateUselessBranch", eliminateUselessBranch, 20),
		//intrinsifies calls to built-ins and unsupported operations
		OptimizationStep("IntrinsifyBuiltin", intrinsify, 30),
		//moves vector-containers to locals and re-directs all uses to the local
		OptimizationStep("HandleLiteralVector", handleContainer, 40),
		//maps access to global data to the offset in the code
		OptimizationStep("MapGlobalDataToAddress", accessGlobalData, 50),
		//calculates constant values where applicable
		//TODO sometimes reverts immediate-loads which are then again converted to immediate-loads??
		OptimizationStep("CalculateConstantValue", calculateConstantInstruction, 60),
		//eliminates/rewrites useless instructions (e.g. y = x + 0 -> y = x)
		OptimizationStep("EliminateUselessInstruction", eliminateUselessInstruction, 70),
		//extracts immediate values into loads
		OptimizationStep("LoadImmediateValues", handleImmediate, 80),
		//prevents register-conflicts by moving long-living locals into temporaries before being used together with literal values
		OptimizationStep("HandleUseWithImmediateValues", handleUseWithImmediate, 90),
		//moves all sources of vector-rotations to accumulators (if too large usage-range)
		OptimizationStep("MoveRotationSourcesToAccs", moveRotationSourcesToAccumulators, 100),
		//simple fail-fast for not inlined or intrinsified method-calls
		OptimizationStep("CheckMethodCalls", checkMethodCalls, 110),
		//combine consecutive instructions writing the same local with a value and zero depending on some flags
		OptimizationStep("CombineSelectionWithZero", combineSelectionWithZero, 120)
};

static void runSingleSteps(const Module& module, Method& method, const Configuration& config)
{
	auto& s = (logging::debug() << "Running steps: ");
	for(const OptimizationStep& step : SINGLE_STEPS)
		s << step.name << ", ";
	s << logging::endl;

//	//FOR VECTOR:
//
//	//since an optimization-step can be run on the result of the previous step,
//	//we can't just pass the resulting iterator (pointing behind the optimization result) into the next optimization-step
//	//using the index makes sure, that any added instruction is processed by the following optimization-steps
//	//for removed instructions, the next optimization-step automatically handles the next instruction (which moves to the index of the removed one)
//	for(std::size_t i = 0; i < method.readInstructions().size(); ++i)
//	{
//		for(const OptimizationStep& step : SINGLE_STEPS)
//		{
//			auto it = method.modifyInstructions();
//			//this is actually constant time, something like "it = it + i"
//			std::advance(it, i);
//			PROFILE_START_DYNAMIC(step.name);
//			step(module, method, it, config);
//			PROFILE_END_DYNAMIC(step.name);
//		}
//	}
	//FOR LIST:

	//since an optimization-step can be run on the result of the previous step,
	//we can't just pass the resulting iterator (pointing behind the optimization result) into the next optimization-step
	//but since lists do not reallocate elements at inserting/removing, we can re-use the previous iterator
	auto it = method.walkAllInstructions();
	//this construct with previous iterator is required, because the iterator could be invalidated (if the underlying node is removed)
	auto prevIt = it;
	while(!it.isEndOfMethod())
	{
		for(const OptimizationStep& step : SINGLE_STEPS)
		{
			PROFILE_START_DYNAMIC(step.name);
			auto newIt = step(module, method, it, config);
			//we can't just test newIt == it here, since if we replace the content of the iterator instead of deleting it, the iterators are still the same, even if we emplace instructions before
			if(newIt.copy().previousInMethod() != prevIt || newIt != it)
				it = prevIt;
			PROFILE_END_DYNAMIC(step.name);
		}
		it.nextInMethod();
		prevIt = it.copy().previousInMethod();
	}
}

const OptimizationPass optimizations::RUN_SINGLE_STEPS = OptimizationPass("SingleSteps", runSingleSteps, 20);
const OptimizationPass optimizations::SPILL_LOCALS = OptimizationPass("SpillLocals", spillLocals, 70);
const OptimizationPass optimizations::COMBINE_VPM_SETUP = OptimizationPass("CombineVPMAccess", combineVPMAccess, 80);
const OptimizationPass optimizations::COMBINE_LITERAL_LOADS = OptimizationPass("CombineLiteralLoads", combineLoadingLiterals, 90);
const OptimizationPass optimizations::ELIMINATE = OptimizationPass("EliminateDeadStores", eliminateDeadStore, 100);
const OptimizationPass optimizations::SPLIT_READ_WRITES = OptimizationPass("SplitReadAfterWrites", splitReadAfterWrites, 110);
const OptimizationPass optimizations::REORDER = OptimizationPass("ReorderInstructions", reorderWithinBasicBlocks, 120);
const OptimizationPass optimizations::COMBINE = OptimizationPass("CombineALUIinstructions", combineOperations, 130);
const OptimizationPass optimizations::UNROLL_WORK_GROUPS = OptimizationPass("UnrollWorkGroups", unrollWorkGroups, 140);

const std::set<OptimizationPass> optimizations::DEFAULT_PASSES = {
		RUN_SINGLE_STEPS, /* SPILL_LOCALS, */ COMBINE_VPM_SETUP, COMBINE_LITERAL_LOADS, ELIMINATE, SPLIT_READ_WRITES, REORDER, COMBINE, UNROLL_WORK_GROUPS
};

Optimizer::Optimizer(const Configuration& config, const std::set<OptimizationPass>& passes) : config(config), passes(passes)
{
}

Optimizer::~Optimizer()
{
}

static void runOptimizationPasses(const Module& module, Method& method, const Configuration& config, const std::set<OptimizationPass>& passes)
{
    logging::debug() << "-----" << logging::endl;
    logging::info() << "Running optimization passes for: " << method.name << logging::endl;
    std::size_t numInstructions = method.countInstructions();
    
    for(const OptimizationPass& pass : passes)
    {
        logging::debug() << logging::endl;
        logging::debug() << "Running pass: " << pass.name << logging::endl;
        PROFILE_COUNTER(pass.index * 100, pass.name + " (before)", method.countInstructions());
        PROFILE_START_DYNAMIC(pass.name);
        pass(module, method, config);
        PROFILE_END_DYNAMIC(pass.name);
        PROFILE_COUNTER_WITH_PREV((pass.index + 1) * 100, pass.name + " (after)", method.countInstructions(), pass.index * 100);
    }
    logging::info() << logging::endl;
    if (numInstructions != method.countInstructions()) {
        logging::info() << "Optimizations done, changed number of instructions from " << numInstructions << " to " << method.countInstructions() << logging::endl;
    }
    else {
        logging::info() << "Optimizations done" << logging::endl;
    }
    logging::debug() << "-----" << logging::endl;
    method.dumpInstructions();
}

void Optimizer::optimize(Module& module) const
{
	std::vector<threading::BackgroundWorker> workers;
	workers.reserve(module.getKernels().size());
	for(auto& method : module.methods)
	{
		//PHI-nodes need to be eliminated before inlining functions
		//since otherwise the phi-node is mapped to the initial label, not to the last label added by the functions (the real end of the original, but split up block)
		PROFILE_COUNTER(90, "Eliminate Phi-nodes (before)", method->countInstructions());
		eliminatePhiNodes(module, *method.get(), config);
		PROFILE_COUNTER_WITH_PREV(95, "Eliminate Phi-nodes (after)", method->countInstructions(), 90);
	}
	for(Method* kernelFunc : module.getKernels())
	{
		Method& kernel = *kernelFunc;

		PROFILE_COUNTER(100, "Inline (before)", kernel.countInstructions());
		inlineMethods(module, kernel, config);
		PROFILE_COUNTER_WITH_PREV(110, "Inline (after)", kernel.countInstructions(), 100);
	}
	for(Method* kernelFunc : module.getKernels())
	{
		auto f = [kernelFunc, &module, this]() -> void {
			runOptimizationPasses(module, *kernelFunc, config, passes);
		};
		workers.emplace(workers.end(), f, "Optimizer")->operator ()();
	}
	threading::BackgroundWorker::waitForAll(workers);
}

void Optimizer::addPass(const OptimizationPass& pass)
{
	passes.insert(pass);
}

void Optimizer::removePass(const OptimizationPass& pass)
{
	passes.erase(pass);
}
