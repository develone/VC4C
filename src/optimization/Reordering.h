/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef REORDERING_H
#define REORDERING_H

#include <config.h>
#include "../intermediate/IntermediateInstruction.h"
#include "../Module.h"
#include "../InstructionWalker.h"

#include <list>

namespace vc4c
{

	namespace optimizations
	{
		/*
		 * Moves an instruction up (to the front, dest) in the sequence of instructions by swapping all instructions in between.
		 * We can't just simply insert the instruction at the original destination, since the iterators might get invalidated!
		 */
		InstructionWalker moveInstructionUp(InstructionWalker dest, InstructionWalker it);

		void splitReadAfterWrites(const Module& module, Method& method, const Configuration& config);

		void reorderWithinBasicBlocks(const Module& module, Method& method, const Configuration& config);

		InstructionWalker moveRotationSourcesToAccumulators(const Module& module, Method& method, InstructionWalker it, const Configuration& config);
	}
}

#endif /* REORDERING_H */

