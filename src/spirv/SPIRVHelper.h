/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */


#ifndef SPIRVHELPER_H
#define SPIRVHELPER_H
#ifdef SPIRV_HEADER

#include SPIRV_OPENCL_HEADER
#include SPIRV_HEADER
#include SPIRV_PARSER_HEADER

#include "log.h"
#include "../Locals.h"

namespace vc4c
{
	namespace spirv2qasm
	{
		std::string getOpenCLMethodName(const uint32_t instructionID);
		std::string getErrorMessage(spv_result_t error);
		spv_result_t checkCapability(const SpvCapability cap);
		Optional<uint32_t> getDecoration(const std::vector<std::pair<SpvDecoration, uint32_t>>& decorations, const SpvDecoration deco);
		void setParameterDecorations(Parameter& param, const std::vector<std::pair<SpvDecoration, uint32_t>>& decorations);
		DataType getIntegerType(const uint32_t bitWidth, const uint32_t signedness);
		AddressSpace toAddressSpace(const SpvStorageClass storageClass);
	}
}



#endif
#endif /* SPIRVHELPER_H */

