/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "../periphery/VPM.h"
#include "Intrinsics.h"
#include "Comparisons.h"
#include "Operators.h"
#include "Images.h"
#include "log.h"
#include "../intermediate/Helper.h"
#include <map>
#include <cmath>
#include <stdbool.h>
#include <vector>

using namespace vc4c;
using namespace vc4c::intermediate;

enum class IntrinsicType
{
    ADD_ALU,
    MUL_ALU,
    SFU,
    VALUE_READ,
    NOOP,
    SEMAPHORE_INCREMENT,
    SEMAPHORE_DECREMENT,
    MUTEX_LOCK,
    MUTEX_UNLOCK,
    DMA_READ,
    DMA_WRITE,
	DMA_COPY,
	VECTOR_ROTATE
};

//The function to apply for pre-calculation
using UnaryInstruction = std::function<Optional<Value>(const Value&)>;
static const UnaryInstruction NO_OP = [](const Value& val){return NO_VALUE;};
//The function to apply for pre-calculation
using BinaryInstruction = std::function<Optional<Value>(const Value&, const Value&)>;
static const BinaryInstruction NO_OP2 = [](const Value& val0, const Value& val1){return NO_VALUE;};

//see VC4CLStdLib (_intrinsics.h)
static constexpr unsigned char VC4CL_UNSIGNED {1};

struct Intrinsic
{
    const IntrinsicType type;
	const char* opCode;
    const Value val;
    const Optional<UnaryInstruction> unaryInstr;
    const Optional<BinaryInstruction> binaryInstr;
    const bool withSignFlag;
    
    Intrinsic(const IntrinsicType type, const Value& val, const UnaryInstruction unary, bool withSignFlag = false) : type(type), opCode(""), val(val), unaryInstr(unary), withSignFlag(withSignFlag)
    {
        
    }
    
    Intrinsic(const IntrinsicType type, const char* opcode, const UnaryInstruction unary, bool withSignFlag = false) : type(type), opCode(opcode), val(UNDEFINED_VALUE), binaryInstr(NO_OP2), withSignFlag(withSignFlag)
    {
        
    }
    
    Intrinsic(const IntrinsicType type, const char* opcode, const BinaryInstruction binary, bool withSignFlag = false) : type(type), opCode(opcode), val(UNDEFINED_VALUE), binaryInstr(binary), withSignFlag(withSignFlag)
	{

	}

    Intrinsic(const IntrinsicType type, const Value& val) : type(type), opCode(""), val(val), withSignFlag(false)
    {
        
    }
};

const static std::map<std::string, Intrinsic> nonaryInstrinsics = {
    {"vc4cl_mutex_lock", Intrinsic{IntrinsicType::MUTEX_LOCK, NOP_REGISTER}},
    {"vc4cl_mutex_unlock", Intrinsic{IntrinsicType::MUTEX_UNLOCK, NOP_REGISTER}},
	{"vc4cl_element_number", Intrinsic{IntrinsicType::VALUE_READ, ELEMENT_NUMBER_REGISTER}},
	{"vc4cl_qpu_number", Intrinsic{IntrinsicType::VALUE_READ, Value(REG_QPU_NUMBER, TYPE_INT8)}}
};

const static std::map<std::string, Intrinsic> unaryIntrinsicMapping = {
    {"vc4cl_ftoi", Intrinsic{IntrinsicType::ADD_ALU, "ftoi", [](const Value& val){return Value(Literal(static_cast<long>(std::round(val.literal.real))), TYPE_INT32);}}},
    {"vc4cl_itof", Intrinsic{IntrinsicType::ADD_ALU, "itof", [](const Value& val){return Value(Literal(static_cast<double>(val.literal.integer)), TYPE_FLOAT);}}},
    {"vc4cl_clz", Intrinsic{IntrinsicType::ADD_ALU, "clz", NO_OP}},
    {"vc4cl_sfu_rsqrt", Intrinsic{IntrinsicType::SFU, Value(REG_SFU_RECIP_SQRT, TYPE_FLOAT), [](const Value& val){return Value(Literal(1.0 / std::sqrt(val.literal.real)), TYPE_FLOAT);}}},
    {"vc4cl_sfu_exp2", Intrinsic{IntrinsicType::SFU, Value(REG_SFU_EXP2, TYPE_FLOAT), [](const Value& val){return Value(Literal(std::exp2(val.literal.real)), TYPE_FLOAT);}}},
    {"vc4cl_sfu_log2", Intrinsic{IntrinsicType::SFU, Value(REG_SFU_LOG2, TYPE_FLOAT), [](const Value& val){return Value(Literal(std::log2(val.literal.real)), TYPE_FLOAT);}}},
    {"vc4cl_sfu_recip", Intrinsic{IntrinsicType::SFU, Value(REG_SFU_RECIP, TYPE_FLOAT), [](const Value& val){return Value(Literal(1.0 / val.literal.real), TYPE_FLOAT);}}},
    {"vc4cl_semaphore_increment", Intrinsic{IntrinsicType::SEMAPHORE_INCREMENT, NOP_REGISTER}},
    {"vc4cl_semaphore_decrement", Intrinsic{IntrinsicType::SEMAPHORE_DECREMENT, NOP_REGISTER}},
    {"vc4cl_dma_read", Intrinsic{IntrinsicType::DMA_READ, NOP_REGISTER}}
};

const static std::map<std::string, Intrinsic> binaryIntrinsicMapping = {
    {"vc4cl_fmax", Intrinsic{IntrinsicType::ADD_ALU, "fmax", [](const Value& val0, const Value& val1){return Value(Literal(std::max(val0.literal.real, val1.literal.real)), TYPE_FLOAT);}}},
    {"vc4cl_fmin", Intrinsic{IntrinsicType::ADD_ALU, "fmin", [](const Value& val0, const Value& val1){return Value(Literal(std::min(val0.literal.real, val1.literal.real)), TYPE_FLOAT);}}},
    {"vc4cl_fmaxabs", Intrinsic{IntrinsicType::ADD_ALU, "fmaxabs", [](const Value& val0, const Value& val1){return Value(Literal(std::max(std::abs(val0.literal.real), std::abs(val1.literal.real))), TYPE_FLOAT);}}},
    {"vc4cl_fminabs", Intrinsic{IntrinsicType::ADD_ALU, "fminabs", [](const Value& val0, const Value& val1){return Value(Literal(std::min(std::abs(val0.literal.real), std::abs(val1.literal.real))), TYPE_FLOAT);}}},
	//FIXME sign / no-sign!!
    {"vc4cl_shr", Intrinsic{IntrinsicType::ADD_ALU, "shr", [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer >> val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_asr", Intrinsic{IntrinsicType::ADD_ALU, "asr", [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer >> val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_ror", Intrinsic{IntrinsicType::ADD_ALU, "ror", NO_OP2}},
    {"vc4cl_shl", Intrinsic{IntrinsicType::ADD_ALU, "shl", [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer << val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_min", Intrinsic{IntrinsicType::ADD_ALU, "min", [](const Value& val0, const Value& val1){return Value(Literal(std::min(val0.literal.integer, val1.literal.integer)), val0.type.getUnionType(val1.type));}, true}},
    {"vc4cl_max", Intrinsic{IntrinsicType::ADD_ALU, "max", [](const Value& val0, const Value& val1){return Value(Literal(std::max(val0.literal.integer, val1.literal.integer)), val0.type.getUnionType(val1.type));}, true}},
    {"vc4cl_and", Intrinsic{IntrinsicType::ADD_ALU, "and", [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer & val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_mul24", Intrinsic{IntrinsicType::MUL_ALU, "mul24", [](const Value& val0, const Value& val1){return Value(Literal((val0.literal.integer & 0xFFFFFFL) * (val1.literal.integer & 0xFFFFFFL)), val0.type.getUnionType(val1.type));}, true}},
    {"vc4cl_dma_write", Intrinsic{IntrinsicType::DMA_WRITE, NOP_REGISTER}},
	{"vc4cl_vector_rotate", Intrinsic{IntrinsicType::VECTOR_ROTATE, NOP_REGISTER}}
};

const static std::map<std::string, Intrinsic> ternaryIntrinsicMapping = {
	{"vc4cl_dma_copy", Intrinsic{IntrinsicType::DMA_COPY, NOP_REGISTER}}
};

const static std::map<std::string, std::pair<Intrinsic, Optional<Value>>> typeCastIntrinsics = {
    {"vc4cl_bitcast_uchar", {Intrinsic{IntrinsicType::ADD_ALU, "and", [](const Value& val){return Value(Literal(val.literal.integer & 0xFF), TYPE_INT8);}, true}, Value(Literal(0xFFUL), TYPE_INT8)}},
    {"vc4cl_bitcast_char", {Intrinsic{IntrinsicType::ADD_ALU, "and", [](const Value& val){return Value(Literal(val.literal.integer & 0xFF), TYPE_INT8);}}, Value(Literal(0xFFUL), TYPE_INT8)}},
    {"vc4cl_bitcast_ushort", {Intrinsic{IntrinsicType::ADD_ALU, "and", [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFF), TYPE_INT16);}, true}, Value(Literal(0xFFFFUL), TYPE_INT16)}},
    {"vc4cl_bitcast_short", {Intrinsic{IntrinsicType::ADD_ALU, "and", [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFF), TYPE_INT16);}}, Value(Literal(0xFFFFUL), TYPE_INT16)}},
    {"vc4cl_bitcast_uint", {Intrinsic{IntrinsicType::ADD_ALU, "mov", [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFFFFFFUL), TYPE_INT32);}, true}, NO_VALUE}},
    {"vc4cl_bitcast_int", {Intrinsic{IntrinsicType::ADD_ALU, "mov", [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFFFFFFUL), TYPE_INT32);}}, NO_VALUE}},
    {"vc4cl_bitcast_float", {Intrinsic{IntrinsicType::ADD_ALU, "mov", [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFFFFFFUL), TYPE_INT32);}}, NO_VALUE}}
};

static InstructionWalker intrinsifyNoArgs(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() > 1 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : nonaryInstrinsics)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
            logging::debug() << "Intrinsifying method-call without arguments to " << callSite->methodName << logging::endl;
            if(pair.second.type == IntrinsicType::VALUE_READ)
            {
                it.reset(new MoveOperation(callSite->getOutput(), pair.second.val));
            }
            else if(pair.second.type == IntrinsicType::MUTEX_LOCK)
            {
                it.reset(new MoveOperation(NOP_REGISTER, MUTEX_REGISTER));
            }
            else if(pair.second.type == IntrinsicType::MUTEX_UNLOCK)
            {
                it.reset(new MoveOperation(MUTEX_REGISTER, BOOL_TRUE));
            }
            else
            {
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled no-arg intrisics", pair.first);
            }
            return it;
        }
    }
    return it;
}

static InstructionWalker intrinsifyUnary(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() == 0 || callSite->getArguments().size() > 2 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : unaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0).get().hasType(ValueType::LITERAL) && pair.second.unaryInstr && pair.second.unaryInstr.get()(callSite->getArgument(0)).hasValue)
        	{
        		logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
        		it.reset(new MoveOperation(callSite->getOutput(), pair.second.unaryInstr.get()(callSite->getArgument(0)), callSite->conditional, callSite->setFlags));
        	}
        	else if(pair.second.type == IntrinsicType::SFU)
            {
				logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to SFU call" << logging::endl;
				it = insertSFUCall(pair.second.val.reg, it, callSite->getArgument(0), callSite->conditional, callSite->setFlags);
				//3. write result to output (from r4)
				it.reset(new MoveOperation(callSite->getOutput(), Value(REG_SFU_OUT, callSite->getOutput().get().type), COND_ALWAYS));
            }
            else if(pair.second.type == IntrinsicType::ADD_ALU || pair.second.type == IntrinsicType::MUL_ALU)
            {
                logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to operation " << pair.second.opCode << logging::endl;
                it.reset(new Operation(pair.second.opCode, callSite->getOutput(), callSite->getArgument(0), COND_ALWAYS));
            }
            else if(pair.second.type == IntrinsicType::NOOP)
            {
                logging::debug() << "Skipping no-op " << callSite->to_string() << logging::endl;
                it.erase();
                //so next instruction is not skipped
                it.previousInBlock();
            }
            else if(pair.second.type == IntrinsicType::DMA_READ)
            {
                logging::debug() << "Intrinsifying memory read " << callSite->to_string() << logging::endl;
                it = periphery::insertReadDMA(it, callSite->getOutput(), callSite->getArgument(0), false);
                it.erase();
                //so next instruction is not skipped
                it.previousInBlock();
            }
            else if(pair.second.type == IntrinsicType::SEMAPHORE_INCREMENT)
            {
            	if(!callSite->getArgument(0).get().hasType(ValueType::LITERAL))
            		throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be a compile-time constant", callSite->to_string());
            	if(callSite->getArgument(0).get().literal.integer < 0 || callSite->getArgument(0).get().literal.integer >= 16)
            		throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be between 0 and 15", callSite->to_string());
            	logging::debug() << "Intrinsifying semaphore increment with instruction" << logging::endl;
            	it.reset(new SemaphoreAdjustment(static_cast<Semaphore>(callSite->getArgument(0).get().literal.integer), true, callSite->conditional, callSite->setFlags));
            }
            else if(pair.second.type == IntrinsicType::SEMAPHORE_DECREMENT)
			{
            	if(!callSite->getArgument(0).get().hasType(ValueType::LITERAL))
					throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be a compile-time constant", callSite->to_string());
				if(callSite->getArgument(0).get().literal.integer < 0 || callSite->getArgument(0).get().literal.integer >= 16)
					throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be between 0 and 15", callSite->to_string());
				logging::debug() << "Intrinsifying semaphore decrement with instruction" << logging::endl;
				it.reset(new SemaphoreAdjustment(static_cast<Semaphore>(callSite->getArgument(0).get().literal.integer), false, callSite->conditional, callSite->setFlags));
			}
            else
            {
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled unary intrisics", pair.first);
            }
        	const bool isUnsigned = pair.second.withSignFlag && callSite->getArgument(2).hasValue && callSite->getArgument(2).get().literal.integer == VC4CL_UNSIGNED;
        	if(isUnsigned)
        		it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);
            return it;
        }
    }
    for(const auto& pair : typeCastIntrinsics)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0).get().hasType(ValueType::LITERAL) && pair.second.first.unaryInstr && pair.second.first.unaryInstr.get()(callSite->getArgument(0)).hasValue)
			{
				logging::debug() << "Intrinsifying type-cast '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput(), pair.second.first.unaryInstr.get()(callSite->getArgument(0)), callSite->conditional, callSite->setFlags));
			}
        	else if(!pair.second.second.hasValue)	//there is no value to apply -> simple move
        	{
        		logging::debug() << "Intrinsifying '" << callSite->to_string() << "' to simple move" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput(), callSite->getArgument(0)));
        	}
        	else if(pair.second.first.type == IntrinsicType::ADD_ALU || pair.second.first.type == IntrinsicType::MUL_ALU)
            {
        		//TODO could use pack-mode here, but only for UNSIGNED values!!
				logging::debug() << "Intrinsifying '" << callSite->to_string() << "' to operation " << pair.second.first.opCode << " with constant " << pair.second.second.to_string() << logging::endl;
                it.reset(new Operation(pair.second.first.opCode, callSite->getOutput(), callSite->getArgument(0), pair.second.second, COND_ALWAYS));
            }
            else
            {
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled type-cast intrisics", pair.first);
            }
			if(pair.second.first.withSignFlag)
				it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);
            return it;
        }
    }
    return it;
}

static InstructionWalker intrinsifyBinary(InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() < 2 || callSite->getArguments().size() > 3 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : binaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0).get().hasType(ValueType::LITERAL) && callSite->getArgument(1).get().hasType(ValueType::LITERAL) && pair.second.binaryInstr && pair.second.binaryInstr.get()(callSite->getArgument(0), callSite->getArgument(1)).hasValue)
			{
				logging::debug() << "Intrinsifying binary '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput(), pair.second.binaryInstr.get()(callSite->getArgument(0), callSite->getArgument(1)), callSite->conditional, callSite->setFlags));
			}
        	else if(pair.second.type == IntrinsicType::ADD_ALU || pair.second.type == IntrinsicType::MUL_ALU)
            {
                logging::debug() << "Intrinsifying binary '" << callSite->to_string() << "' to operation " << pair.second.opCode << logging::endl;
                it.reset(new Operation(pair.second.opCode, callSite->getOutput(), callSite->getArgument(0), callSite->getArgument(1), COND_ALWAYS));
            }
            else if(pair.second.type == IntrinsicType::DMA_WRITE)
            {
                logging::debug() << "Intrinsifying memory write " << callSite->to_string() << logging::endl;
                it = periphery::insertWriteDMA(it, callSite->getArgument(1), callSite->getArgument(0), false);
                it.erase();
                //so next instruction is not skipped
                it.previousInBlock();
            }
            else if(pair.second.type == IntrinsicType::VECTOR_ROTATE)
            {
            	logging::debug() << "Intrinsifying vector rotation " << callSite->to_string() << logging::endl;
            	it = insertVectorRotation(it, callSite->getArgument(0), callSite->getArgument(1), callSite->getOutput(), Direction::UP);
            	it.erase();
				//so next instruction is not skipped
            	it.previousInBlock();
            }
            else
            {
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled binary intrisics", pair.first);
            }
        	const bool isUnsigned = pair.second.withSignFlag && callSite->getArgument(3).hasValue && callSite->getArgument(3).get().literal.integer == VC4CL_UNSIGNED;
        	if(isUnsigned)
				it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);
            return it;
        }
    }
    return it;
}

static InstructionWalker intrinsifyTernary(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() < 3 || callSite->getArguments().size() > 4 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : ternaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
            if(pair.second.type == IntrinsicType::ADD_ALU || pair.second.type == IntrinsicType::MUL_ALU)
            {
                logging::debug() << "Intrinsifying ternary '" << callSite->to_string() << "' to operation " << pair.second.opCode << logging::endl;
                it.emplace(new Operation(pair.second.opCode, callSite->getOutput(), callSite->getArgument(0), callSite->getArgument(1)));
            }
            else if(pair.second.type == IntrinsicType::DMA_COPY)
            {
			   logging::debug() << "Intrinsifying ternary '" << callSite->to_string() << "' to DMA copy operation " << logging::endl;
			   const DataType type = callSite->getArgument(0).get().type.getElementType();
			   //TODO number of elements!
			   it = method.vpm->insertReadRAM(it, callSite->getArgument(1), type, false);
			   it = method.vpm->insertWriteRAM(it, callSite->getArgument(0), type, false);
			   it.erase();
			   //so next instruction is not skipped
			   it.previousInBlock();
            }
            else
            {
                throw CompilationError(CompilationStep::OPTIMIZER, "Unhandled ternary intrisics", pair.first);
            }
            const bool isUnsigned = pair.second.withSignFlag && callSite->getArgument(4).hasValue && callSite->getArgument(4).get().literal.integer == VC4CL_UNSIGNED;
            if(isUnsigned)
            	it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);
            return it;
        }
    }
    return it;
}

static void swapComparisons(const std::string& opCode, Comparison* comp)
{
    Value tmp = comp->getFirstArg();
    comp->setArgument(0, comp->getSecondArg());
    comp->setArgument(1, tmp);
    comp->opCode = opCode;
}

static InstructionWalker intrinsifyComparison(Method& method, InstructionWalker it)
{
    Comparison* comp = it.get<Comparison>();
    if(comp == nullptr)
    {
        return it;
    }
    logging::debug() << "Intrinsifying comparison '" << comp->opCode << "' to arithmetic operations" << logging::endl;
    bool isFloating = comp->getFirstArg().type.isFloatingType();
    if(!isFloating)
    {
        //simplification, make a R b -> b R' a
        if(COMP_UNSIGNED_GE.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_UNSIGNED_LE, comp);
        }
        else if(COMP_UNSIGNED_GT.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_UNSIGNED_LT, comp);
        }
        else if(COMP_SIGNED_GE.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_SIGNED_LE, comp);
        }
        else if(COMP_SIGNED_GT.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_SIGNED_LT, comp);
        }
            
        it = intrinsifyIntegerRelation(method, it, comp);
    }
    else
    {
        //simplification, make a R b -> b R' a
        if(COMP_ORDERED_GE.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_ORDERED_LE, comp);
        }
        else if(COMP_ORDERED_GT.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_ORDERED_LT, comp);
        }
        else if(COMP_UNORDERED_GE.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_UNORDERED_LE, comp);
        }
        else if(COMP_UNORDERED_GT.compare(comp->opCode) == 0)
        {
            swapComparisons(COMP_UNORDERED_LT, comp);
        }
        
        it = intrinsifyFloatingRelation(method, it, comp);
    }
    
    return it;
}

static bool isPowerTwo(long val)
{
    //https://en.wikipedia.org/wiki/Power_of_two#Fast_algorithm_to_check_if_a_positive_number_is_a_power_of_two
    return val > 0 && (val & (val - 1)) == 0;
}

static InstructionWalker intrinsifyArithmetic(Method& method, InstructionWalker it, const MathType& mathType)
{
    Operation* op = it.get<Operation>();
    if(op == nullptr)
    {
        return it;
    }
    const Value& arg0 = op->getFirstArg();
    const Value& arg1 = op->getSecondArg().orElse(UNDEFINED_VALUE);
    const bool saturateResult = has_flag(op->decoration, InstructionDecorations::SATURATED_CONVERSION);
    //integer multiplication
    if(op->opCode.compare("mul") == 0)
    {
        //a * b = b * a
        //a * 2^n = a << n
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for multiplication with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal(arg0.literal.integer * arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg0.hasType(ValueType::LITERAL) && isPowerTwo(arg0.literal.integer))
        {
            logging::debug() << "Intrinsifying multiplication with left-shift" << logging::endl;
            op->opCode = "shl";
            op->setArgument(0, arg1);
            op->setArgument(1, Value(Literal(static_cast<long>(std::log2(arg0.literal.integer))), arg0.type));
        }
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying multiplication with left-shift" << logging::endl;
            op->opCode = "shl";
            op->setArgument(1, Value(Literal(static_cast<long>(std::log2(arg1.literal.integer))), arg1.type));
        }
        else if(std::max(arg0.type.getScalarBitCount(), arg1.type.getScalarBitCount()) <= 24)
        {
            logging::debug() << "Intrinsifying multiplication of small integers to mul24" << logging::endl;
            op->opCode = "mul24";
        }
        else
        {
            it = intrinsifySignedIntegerMultiplication(method, it, *op);
        }
    }
    //unsigned division
    else if(op->opCode.compare("udiv") == 0)
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal((long)(unsigned long)(arg0.literal.integer / arg1.literal.integer)), arg0.type), op->conditional, op->setFlags));
        }
        //a / 2^n = a >> n
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying division with right-shift" << logging::endl;
            op->opCode = "shr";
            op->setArgument(1, Value(Literal(static_cast<long>(std::log2(arg1.literal.integer))), arg1.type));
        }
//        else if(arg1.hasType(ValueType::LITERAL))
//        {
//            //TODO replace with multiplication and shift (only if no overflow!)
//            //http://forums.parallax.com/discussion/114807/fast-faster-fastest-code-integer-division
//        }
        else
        {
            it = intrinsifyUnsignedIntegerDivision(method, it, *op);
        }
    }
    //signed division
    else if(op->opCode.compare("sdiv") == 0)
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal(arg0.literal.integer / arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        //a / 2^n = a >> n
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying signed division with arithmetic right-shift" << logging::endl;
            op->opCode = "asr";
            op->setArgument(1, Value(Literal(static_cast<long>(std::log2(arg1.literal.integer))), arg1.type));
        }
        else
        {
            it = intrinsifySignedIntegerDivision(method, it, *op);
        }
    }
    //unsigned modulo
    //LLVM IR calls it urem, SPIR-V umod
    else if(op->opCode.compare("urem") == 0 || op->opCode.compare("umod") == 0)
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for modulo with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal((long)(unsigned long)(arg0.literal.integer % arg1.literal.integer)), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying unsigned modulo by power of two" << logging::endl;
            op->opCode = "and";
            op->setArgument(1, Value(Literal(arg1.literal.integer - 1), arg1.type));
        }
        else
        {
            it = intrinsifyUnsignedIntegerDivision(method, it, *op, true);
        }
    }
    //signed modulo
    else if(op->opCode.compare("srem") == 0)
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed modulo with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal(arg0.literal.integer % arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        else
        {
            it = intrinsifySignedIntegerDivision(method, it, *op, true);
        }
    }
    //floating division
    else if(op->opCode.compare("fdiv") == 0)
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput().get().local, arg0.type), Value(Literal(arg0.literal.real / arg1.literal.real), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Intrinsifying floating division with multiplication of constant inverse" << logging::endl;
            op->opCode = "fmul";
            op->setArgument(1, Value(Literal(1.0f / arg1.literal.real), arg1.type));
        }
        else if(has_flag(op->decoration, InstructionDecorations::ALLOW_RECIP) || has_flag(op->decoration, InstructionDecorations::FAST_MATH))
        {
            logging::debug() << "Intrinsifying floating division with multiplication of reciprocal" << logging::endl;
            it = insertSFUCall(REG_SFU_RECIP, it, arg1, op->conditional);
            it.nextInBlock();
            op->opCode = "fmul";
            op->setArgument(1, Value(REG_SFU_OUT, op->getFirstArg().type));
        }
        else
        {
            logging::debug() << "Intrinsifying floating division with multiplication of inverse" << logging::endl;
            it = intrinsifyFloatingDivision(method, it, *op);
        }
    }
    //truncate bits
    else if(op->opCode.compare("trunc") == 0)
    {
    	if(saturateResult)
    	{
    		//let pack-mode handle saturation
    		logging::debug() << "Intrinsifying saturated truncate with move and pack-mode" << logging::endl;
    		it = insertSaturation(it, method, op->getFirstArg(), op->getOutput(), !has_flag(op->decoration, InstructionDecorations::UNSIGNED_RESULT));
    		it.nextInBlock();
    		it.erase();
    	}
        //if orig = i64, dest = i32 -> move
    	else if(op->getFirstArg().type.getScalarBitCount() > 32 && op->getOutput().get().type.getScalarBitCount() == 32)
        {
            //do nothing, is just a move, since we truncate the 64-bit integers anyway
            logging::debug() << "Intrinsifying truncate from unsupported type with move" << logging::endl;
            it.reset((new MoveOperation(op->getOutput(), op->getFirstArg(), op->conditional, op->setFlags))->copyExtrasFrom(op));
        }
        //if dest < i32 -> orig & dest-bits or pack-code
        else if(op->getOutput().get().type.getScalarBitCount() < 32)
        {
            logging::debug() << "Intrinsifying truncate with and" << logging::endl;
            op->opCode = "and";
            op->setArgument(1, Value(Literal(op->getOutput().get().type.getScalarWidthMask()), TYPE_INT32));
        }
    }
    else if(op->opCode.compare("fptrunc") == 0)
    {
    	if(saturateResult)
    	{
    		//let pack-mode handle saturation
    		it = insertSaturation(it, method, op->getFirstArg(), op->getOutput(), !has_flag(op->decoration, InstructionDecorations::UNSIGNED_RESULT));
			it.nextInBlock();
			it.erase();
    	}
        //if orig = i64, dest = i32 -> move
    	else if(op->getFirstArg().type.getScalarBitCount() >= 32 && op->getOutput().get().type.getScalarBitCount() == 32)
        {
            logging::debug() << "Intrinsifying fptrunc with move" << logging::endl;
            it.reset((new MoveOperation(op->getOutput(), op->getFirstArg(), op->conditional, op->setFlags))->copyExtrasFrom(op));
        }
        else if(op->getFirstArg().type.getScalarBitCount() < 32)
            throw CompilationError(CompilationStep::OPTIMIZER, "Unsupported floating-point type", op->getFirstArg().type.to_string());
        else
            throw CompilationError(CompilationStep::OPTIMIZER, "Unsupported floating-point type", op->getOutput().get().type.to_string());
    }
    //arithmetic shift right
    else if(op->opCode.compare("ashr") == 0)
    {
        //just surgical modification
        op->opCode = "asr";
    }
    else if(op->opCode.compare("lshr") == 0)
    {
        //TODO only if type <= i32 and/or offset <= 32
        //just surgical modification
        op->opCode = "shr";
    }
    //integer to float
    else if(op->opCode.compare("sitofp") == 0)
    {
    	//for non 32-bit types, need to sign-extend
    	Value tmp = op->getFirstArg();
    	if(op->getFirstArg().type.getScalarBitCount() < 32)
    	{
    		tmp = method.addNewLocal(TYPE_INT32, "%sitofp");
    		it = insertSignExtension(it, method, op->getFirstArg(), tmp, op->conditional);
    		it.nextInBlock();
    	}
        //just surgical modification
        op->opCode = "itof";
        if(tmp != op->getFirstArg())
        	op->setArgument(0, tmp);
    }
    else if(op->opCode.compare("uitofp") == 0)
    {
    	const Value tmp = method.addNewLocal(op->getOutput().get().type, "%uitofp");
        if(op->getFirstArg().type.getScalarBitCount() < 32)
        {
            //make sure, leading bits are zeroes
            const long mask = op->getFirstArg().type.getScalarWidthMask();
            it.emplace(new Operation("and", tmp, op->getFirstArg(), Value(Literal(mask), TYPE_INT32), op->conditional));
            it.nextInBlock();
            op->setArgument(0, tmp);
            op->opCode = "itof";
        }
        else if(op->getFirstArg().type.getScalarBitCount() > 32)
        {
            throw CompilationError(CompilationStep::OPTIMIZER, "Can't convert long to floating value, since long is not supported!");
        }
        else    //32-bits
        {
            //itofp + if MSB set add 2^31(f)
//            it.emplace(new Operation("and", REG_NOP, Value(Literal(0x80000000UL), TYPE_INT32), op->getFirstArg(), op->conditional, SetFlag::SET_FLAGS));
//            ++it;
//            it.emplace((new Operation("itof", tmp, op->getFirstArg(), op->conditional))->setDecorations(op->decoration));
//            ++it;
//            it.reset(new Operation("fadd", op->getOutput(), tmp, Value(Literal(std::pow(2, 31)), TYPE_FLOAT), COND_ZERO_CLEAR));
        	//TODO this passed OpenCL-CTS parameter_types, but what of large values (MSB set)??
        	op->opCode = "itof";
        }
    }
    //float to integer
    else if(op->opCode.compare("fptosi") == 0)
    {
        //just surgical modification
    	//TODO truncate to type?
        op->opCode = "ftoi";
    }
    //float to unsigned integer
    else if(op->opCode.compare("fptoui") == 0)
    {
        //TODO special treatment??
    	//TODO truncate to type?
        op->opCode = "ftoi";
        op->decoration = add_flag(op->decoration, InstructionDecorations::UNSIGNED_RESULT);
    }
    //sign extension
    else if(op->opCode.compare("sext") == 0)
    {
        logging::debug() << "Intrinsifying sign extension with shifting" << logging::endl;
        it = insertSignExtension(it, method, op->getFirstArg(), op->getOutput(), op->conditional, op->setFlags);
        //remove 'sext'
        it.erase();
        //so next instruction is not skipped
        it.previousInBlock();
    }
    //zero extension
    else if(op->opCode.compare("zext") == 0)
    {
        logging::debug() << "Intrinsifying zero extension with and" << logging::endl;
        it = insertZeroExtension(it, method, op->getFirstArg(), op->getOutput(), op->conditional, op->setFlags);
        //remove 'zext'
        it.erase();
        //so next instruction is not skipped
        it.previousInBlock();
    }
    return it;
}

static InstructionWalker intrinsifyReadWorkGroupInfo(Method& method, InstructionWalker it, const Value& arg, const std::vector<std::string>& locals, const Value& defaultValue, const InstructionDecorations decoration)
{
	if(arg.hasType(ValueType::LITERAL))
	{
		Value src = UNDEFINED_VALUE;
		switch(arg.literal.integer)
		{
			case 0:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(0))->createReference();
				break;
			case 1:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(1))->createReference();
				break;
			case 2:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(2))->createReference();
				break;
			default:
				src = defaultValue;
		}
		return it.reset((new MoveOperation(it->getOutput(), src))->copyExtrasFrom(it.get()));
	}
	//dim == 0 -> return first value
	it.emplace((new Operation("xor", NOP_REGISTER, arg, INT_ZERO))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	it.emplace(new MoveOperation(it->getOutput(), method.findOrCreateLocal(TYPE_INT32, locals.at(0))->createReference(), COND_ZERO_SET));
	it.nextInBlock();
	//dim == 1 -> return second value
	it.emplace((new Operation("xor", NOP_REGISTER, arg, INT_ONE))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	it.emplace(new MoveOperation(it->getOutput(), method.findOrCreateLocal(TYPE_INT32, locals.at(1))->createReference(), COND_ZERO_SET));
	it.nextInBlock();
	//dim == 2 -> return third value
	it.emplace((new Operation("xor", NOP_REGISTER, arg, Value(Literal(2L), TYPE_INT32)))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	it.emplace(new MoveOperation(it->getOutput(), method.findOrCreateLocal(TYPE_INT32, locals.at(2))->createReference(), COND_ZERO_SET));
	it.nextInBlock();
	//otherwise (dim > 2 -> 2 - dim < 0) return default value
	it.emplace((new Operation("sub", NOP_REGISTER, Value(Literal(2L), TYPE_INT32), arg))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	return it.reset(new MoveOperation(it->getOutput(), defaultValue, COND_NEGATIVE_SET));
}

static InstructionWalker intrinsifyReadWorkItemInfo(Method& method, InstructionWalker it, const Value& arg, const std::string& local, const InstructionDecorations decoration)
{
	/*
	 * work-item infos (id, size) are stored within a single UNIFORM:
	 * high <-> low byte
	 * 00 | 3.dim | 2.dim | 1.dim
	 * -> res = (UNIFORM >> (dim * 8)) & 0xFF
	 */
	const Local* itemInfo = method.findOrCreateLocal(TYPE_INT32, local);
	Value tmp0(UNDEFINED_VALUE);
	//TODO this pre-calculation causes invalid values for get_global_id(), but why?? (see ./testing/test_work_item.cl)
//	if(arg.hasType(ValueType::LITERAL))
//	{
//		tmp0 = Value(Literal(arg.literal.integer * 8L), TYPE_INT8);
//	}
//	else
	{
		tmp0 = method.addNewLocal(TYPE_INT32, "%local_info");
		it.emplace(new Operation("mul24", tmp0, arg, Value(Literal(8L), TYPE_INT32)));
		it.nextInBlock();
	}
	const Value tmp1 = method.addNewLocal(TYPE_INT32, "%local_info");
	it.emplace(new Operation("shr", tmp1, itemInfo->createReference(), tmp0));
	it.nextInBlock();
	return it.reset((new Operation("and", it->getOutput(), tmp1, Value(Literal(0xFFL), TYPE_INT8)))->copyExtrasFrom(it.get()));
}

static InstructionWalker intrinsifyWorkItemFunctions(Method& method, InstructionWalker it)
{
	MethodCall* callSite = it.get<MethodCall>();
	if(callSite == nullptr)
		return it;
	if(callSite->getArguments().size() > 1)
		return it;

	if(callSite->methodName.compare("vc4cl_work_dimensions") == 0 && callSite->getArguments().size() == 0)
	{
		logging::debug() << "Intrinsifying reading of work-item dimensions" << logging::endl;
		//setting the type to int8 allows us to optimize e.g. multiplications with work-item values
		Value out = callSite->getOutput();
		out.type = TYPE_INT8;
		return it.reset((new MoveOperation(out, method.findOrCreateLocal(TYPE_INT32, Method::WORK_DIMENSIONS)->createReference()))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_WORK_DIMENSIONS)));
	}
	if(callSite->methodName.compare("vc4cl_num_groups") == 0 && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the number of work-groups" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::NUM_GROUPS_X, Method::NUM_GROUPS_Y, Method::NUM_GROUPS_Z}, INT_ONE, InstructionDecorations::BUILTIN_NUM_GROUPS);
	}
	if(callSite->methodName.compare("vc4cl_group_id") == 0 && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the work-group ids" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::GROUP_ID_X, Method::GROUP_ID_Y, Method::GROUP_ID_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GROUP_ID);
	}
	if(callSite->methodName.compare("vc4cl_global_offset") == 0 && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the global offsets" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::GLOBAL_OFFSET_X, Method::GLOBAL_OFFSET_Y, Method::GLOBAL_OFFSET_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GLOBAL_OFFSET);
	}
	if(callSite->methodName.compare("vc4cl_local_size") == 0 && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of local work-item sizes" << logging::endl;
		//TODO needs to have a size of 1 for all higher dimensions (instead of currently implicit 0)
		return intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
	}
	if(callSite->methodName.compare("vc4cl_local_id") == 0 && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of local work-item ids" << logging::endl;
		return intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0), Method::LOCAL_IDS, InstructionDecorations::BUILTIN_LOCAL_ID);
	}
	if(callSite->methodName.compare("vc4cl_global_size") == 0 && callSite->getArguments().size() == 1)
	{
		//global_size(dim) = local_size(dim) * num_groups(dim)
		logging::debug() << "Intrinsifying reading of global work-item sizes" << logging::endl;

		const Value tmpLocalSize = method.addNewLocal(TYPE_INT8, "%local_size");
		const Value tmpNumGroups = method.addNewLocal(TYPE_INT32, "%num_groups");
		//emplace dummy instructions to be replaced
		it.emplace(new MoveOperation(tmpLocalSize, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpNumGroups, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::NUM_GROUPS_X, Method::NUM_GROUPS_Y, Method::NUM_GROUPS_Z}, INT_ONE, InstructionDecorations::BUILTIN_NUM_GROUPS);
		it.nextInBlock();
		return it.reset((new Operation("mul24", callSite->getOutput(), tmpLocalSize, tmpNumGroups))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_GLOBAL_SIZE)));
	}
	if(callSite->methodName.compare("vc4cl_global_id") == 0 && callSite->getArguments().size() == 1)
	{
		//global_id(dim) = global_offset(dim) + (group_id(dim) * local_size(dim) + local_id(dim)
		logging::debug() << "Intrinsifying reading of global work-item ids" << logging::endl;

		const Value tmpGroupID = method.addNewLocal(TYPE_INT32, "%group_id");
		const Value tmpLocalSize = method.addNewLocal(TYPE_INT8, "%local_size");
		const Value tmpGlobalOffset = method.addNewLocal(TYPE_INT32, "%global_offset");
		const Value tmpLocalID = method.addNewLocal(TYPE_INT8, "%local_id");
		const Value tmpRes0 = method.addNewLocal(TYPE_INT32, "%global_id");
		const Value tmpRes1 = method.addNewLocal(TYPE_INT32, "%global_id");
		//emplace dummy instructions to be replaced
		it.emplace(new MoveOperation(tmpGroupID, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::GROUP_ID_X, Method::GROUP_ID_Y, Method::GROUP_ID_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GROUP_ID);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpLocalSize, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpGlobalOffset, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0), {Method::GLOBAL_OFFSET_X, Method::GLOBAL_OFFSET_Y, Method::GLOBAL_OFFSET_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GLOBAL_OFFSET);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpLocalID, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0), Method::LOCAL_IDS, InstructionDecorations::BUILTIN_LOCAL_ID);
		it.nextInBlock();
		it.emplace(new Operation("mul24", tmpRes0, tmpGroupID, tmpLocalSize));
		it.nextInBlock();
		it.emplace(new Operation("add", tmpRes1, tmpGlobalOffset, tmpRes0));
		it.nextInBlock();
		return it.reset((new Operation("add", callSite->getOutput(), tmpRes1, tmpLocalID))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_GLOBAL_ID)));
	}
	return it;
}

static void setDecorationToBranches(Local* condition)
{
	condition->forUsers(LocalUser::Type::READER, [](const LocalUser* user) -> void
	{
		if(dynamic_cast<const Branch*>(user) != nullptr)
		{
			Branch* branch = dynamic_cast<Branch*>(const_cast<LocalUser*>(user));
			branch->setDecorations(InstructionDecorations::BRANCH_ON_ALL_ELEMENTS);
		}
	});
}

static InstructionWalker intrinsifySpecial(Method& method, InstructionWalker it)
{
	MethodCall* callSite = it.get<MethodCall>();
	if(callSite == nullptr)
		return it;
	if(callSite->getArguments().size() == 1)
	{
		if(callSite->methodName.find("vc4cl_any_non_zero") != std::string::npos)
		{
			logging::debug() << "Intrinsifying conditional branch on ALL SIMD-elements" << logging::endl;
			setDecorationToBranches(it->getOutput().get().local);
			return it.reset((new MoveOperation(it->getOutput(), it->getArgument(0)))->copyExtrasFrom(callSite));
		}
		if(callSite->methodName.find("vc4cl_all_non_zero") != std::string::npos)
		{
			logging::debug() << "Intrinsifying conditional branch on ALL SIMD-elements" << logging::endl;
			setDecorationToBranches(it->getOutput().get().local);
			return it.reset((new MoveOperation(it->getOutput(), it->getArgument(0)))->copyExtrasFrom(callSite));
		}
	}
	return it;
}

InstructionWalker optimizations::intrinsify(const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
	if(!it.has<Operation>() && !it.has<MethodCall>())
		//fail fast
		return it;
	auto newIt = intrinsifyComparison(method, it);
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyWorkItemFunctions(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyNoArgs(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyUnary(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyBinary(it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyTernary(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyArithmetic(method, it, config.mathType);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyImageFunction(it, method);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifySpecial(method, it);
	}
	return newIt;
}
