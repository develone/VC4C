/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "IntermediateInstruction.h"
#include "../asm/ALUInstruction.h"
#include "expr.h"
#include "log.h"

#include <cmath>

using namespace vc4c;
using namespace vc4c::intermediate;

Operation::Operation(const std::string& opCode, const Value& dest, const Value& arg0, const ConditionCode cond, const SetFlag setFlags) :
IntermediateInstruction(dest, cond, setFlags), opCode(opCode), parent(nullptr)
{
	setArgument(0, arg0);
}

Operation::Operation(const std::string& opCode, const Value& dest, const Value& arg0, const Value& arg1, const ConditionCode cond, const SetFlag setFlags) :
IntermediateInstruction(dest, cond, setFlags), opCode(opCode), parent(nullptr)
{
	setArgument(0, arg0);
	setArgument(1, arg1);
}

Operation::~Operation()
{

}

std::string Operation::to_string() const
{
    return (getOutput().get().to_string(true) + " = ") + (opCode + " ") + getFirstArg().to_string() + (getSecondArg() ? std::string(", ") + getSecondArg().to_string() : "") + createAdditionalInfoString();
}

IntermediateInstruction* Operation::copyFor(Method& method, const std::string& localPrefix) const
{
    if (!getSecondArg())
        return (new Operation(opCode, renameValue(method, getOutput(), localPrefix), renameValue(method, getFirstArg(), localPrefix), conditional, setFlags))->copyExtrasFrom(this);
    return (new Operation(opCode, renameValue(method, getOutput(), localPrefix), renameValue(method, getFirstArg(), localPrefix), renameValue(method, getSecondArg(), localPrefix), conditional, setFlags))->copyExtrasFrom(this);
}

static InputMutex getInputMux(const Register& reg, const bool isExplicitRegister, const Optional<SmallImmediate>& immediate, const bool isABlocked = false, const bool isBBlocked = false)
{
	if(immediate.hasValue)
		return InputMutex::REGB;
    if (!isExplicitRegister && reg.isAccumulator()) {
        return static_cast<InputMutex> (reg.getAccumulatorNumber());
    }
    if (isExplicitRegister && reg.isAccumulator() && (reg.getAccumulatorNumber() == 5 || reg.getAccumulatorNumber() == 4)) {
        //special case: is handled as register, but need to access the accumulator
        //reason: the value is actually read from the accumulator, not from the register with the same index
        return static_cast<InputMutex> (reg.getAccumulatorNumber());
    }
    if (reg.file == RegisterFile::PHYSICAL_A)
        return InputMutex::REGA;
    if (reg.file == RegisterFile::PHYSICAL_B)
        return InputMutex::REGB;
    if (!isABlocked)
        return InputMutex::REGA;
    if (!isBBlocked)
        return InputMutex::REGB;
    throw CompilationError(CompilationStep::CODE_GENERATION, "Invalid input getSource()", reg.to_string(true, true));
}

static Register getRegister(const RegisterFile file, const Register reg0, const Register reg1, const Operation* op)
{
    if (reg0.file == file && reg1.file == file) {
        if (reg0.isAccumulator())
            return reg1;
        if (reg1.isAccumulator())
            return reg0;
        if (reg0 == reg1)
            return reg0;
        if(reg0.num == REG_NOP.num)
        	return reg1;
        if(reg1.num == REG_NOP.num)
        	return reg0;
        logging::error() << "Error in assigning registers for " << op->to_string() << logging::endl;
        throw CompilationError(CompilationStep::CODE_GENERATION, "Can't access two registers from the same file", (reg0.to_string(true, true) + " and ") + reg1.to_string(true, true));
    }
    else if (reg0.file == file)
        return reg0;
    else if (reg1.file == file)
        return reg1;
    else if (has_flag(reg0.file, RegisterFile::PHYSICAL_ANY))
        return reg0;
    else if (has_flag(reg1.file, RegisterFile::PHYSICAL_ANY))
        return reg1;
    return REG_NOP;
}

std::pair<Register, Optional<SmallImmediate>> intermediate::getInputValue(const Value& val, const FastMap<const Local*, Register>& registerMapping)
{
    if (val.hasType(ValueType::REGISTER))
        return std::make_pair(val.reg, Optional<SmallImmediate>(false, 0));
    if (val.hasType(ValueType::LOCAL)) {
        const Register reg = registerMapping.at(val.local);
        return std::make_pair(reg, Optional<SmallImmediate>(false, 0));
    }
    if(val.hasType(ValueType::SMALL_IMMEDIATE))
    	return std::make_pair(Register{RegisterFile::PHYSICAL_B, val.immediate.value}, val.immediate);
//    if (val.hasType(ValueType::LITERAL))
//        return std::make_pair(Register{RegisterFile::PHYSICAL_B, val.literal.toImmediate()}, static_cast<SmallImmediate> (val.literal.toImmediate()));
    throw CompilationError(CompilationStep::CODE_GENERATION, "Unhandled value", val.to_string(false));
}

qpu_asm::Instruction* Operation::convertToAsm(const FastMap<const Local*, Register>& registerMapping, const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    std::pair<OpAdd, OpMul> opCodes = toOpCode(opCode);
    const Register outReg = getOutput().get().hasType(ValueType::LOCAL) ? registerMapping.at(getOutput().get().local) : getOutput().get().reg;

    auto input0 = getInputValue(getFirstArg(), registerMapping);
    const InputMutex inMux0 = getInputMux(input0.first, getFirstArg().hasType(ValueType::REGISTER), input0.second);
    if (!input0.second) {
        if (input0.first.isAccumulator() && inMux0 != InputMutex::REGA && inMux0 != InputMutex::REGB)
            input0.first.num = REG_NOP.num; //Cosmetics, so vc4asm does not print "maybe reading reg xx"
        else if (has_flag(input0.first.file, RegisterFile::PHYSICAL_ANY))
            input0.first.file = RegisterFile::PHYSICAL_A;
    }
    const bool writeSwap = (opCodes.first != OPADD_NOP && outReg.file == RegisterFile::PHYSICAL_B) ||
            (opCodes.second != OPMUL_NOP && outReg.file == RegisterFile::PHYSICAL_A);

    const bool isAddALU = opCodes.first != OPADD_NOP;
    const bool isMulALU = opCodes.second != OPMUL_NOP;
    const WriteSwap swap = writeSwap ? WriteSwap::SWAP : WriteSwap::DONT_SWAP;

    if (getSecondArg()) {
        auto input1 = getInputValue(getSecondArg(), registerMapping);
        InputMutex inMux1 = getInputMux(input1.first, getSecondArg().get().hasType(ValueType::REGISTER), input1.second, input0.first.file == RegisterFile::PHYSICAL_A, input0.first.file == RegisterFile::PHYSICAL_B);

        //one of the values is a literal immediate
        if (input0.second || input1.second) {

            SmallImmediate imm = input0.second ? input0.second : input1.second;
            Register regA = input0.second ? input1.first : input0.first;
            if (input0.second && input1.second) {
                //both operands have literal
                if (input0.second.get() != input1.second.get())
                    throw CompilationError(CompilationStep::CODE_GENERATION, "Can't perform operation with two distinguish literals", to_string());
                regA = REG_NOP;
            }
            else if(input0.second.hasValue && input1.first.file == RegisterFile::ACCUMULATOR)
            	//don't read register overlapped by accumulator, e.g. UNIFORM
            	regA = REG_NOP;
            else if(input1.second.hasValue && input0.first.file == RegisterFile::ACCUMULATOR)
            	regA = REG_NOP;
            if(firesSignal())
            {
            	throw CompilationError(CompilationStep::CODE_GENERATION, "Signal is discarded, since the operation takes an immediate", toString(signal));
            }

            if (isAddALU) {
                return new qpu_asm::ALUInstruction(unpackMode, packMode, conditional, COND_NEVER, setFlags, swap,
                                          outReg.num, REG_NOP.num, OPMUL_NOP, opCodes.first, regA.num, imm,
                                          inMux0, inMux1, MUTEX_NONE, MUTEX_NONE);
            }
            else if (isMulALU) {
                return new qpu_asm::ALUInstruction(unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
                                          REG_NOP.num, outReg.num, opCodes.second, OPADD_NOP, regA.num, imm,
                                          MUTEX_NONE, MUTEX_NONE, inMux0, inMux1);
            }
            throw CompilationError(CompilationStep::CODE_GENERATION, "No instruction set", to_string());
        }
            //both are registers
        else {

            if (input1.first.isAccumulator() && inMux1 != InputMutex::REGA && inMux1 != InputMutex::REGB)
                input1.first.num = REG_NOP.num; //Cosmetics, so vc4asm does not print "maybe reading reg xx"
            else if (has_flag(input1.first.file, RegisterFile::PHYSICAL_ANY))
            {
            	if(getInputValue(getFirstArg(), registerMapping).first == getInputValue(getSecondArg(), registerMapping).first)
            	{
            		//same inputs - this allows e.g. vc4asm to recognize "mov x, uniform" correctly
            		input1.first.file = input0.first.file;
            		inMux1 = inMux0;
            	}
            	else
            		input1.first.file = RegisterFile::PHYSICAL_B;
            }
            if (isAddALU) {
                return new qpu_asm::ALUInstruction(signal, unpackMode, packMode, conditional, COND_NEVER, setFlags, swap,
                                          outReg.num, REG_NOP.num, OPMUL_NOP, opCodes.first,
                                          getRegister(RegisterFile::PHYSICAL_A, input0.first, input1.first, this).num,
                                          getRegister(RegisterFile::PHYSICAL_B, input0.first, input1.first, this).num,
                                          inMux0, inMux1, MUTEX_NONE, MUTEX_NONE);
            }
            else if (isMulALU) {
                return new qpu_asm::ALUInstruction(signal, unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
                                          REG_NOP.num, outReg.num, opCodes.second, OPADD_NOP,
                                          getRegister(RegisterFile::PHYSICAL_A, input0.first, input1.first, this).num,
                                          getRegister(RegisterFile::PHYSICAL_B, input0.first, input1.first, this).num,
                                          MUTEX_NONE, MUTEX_NONE, inMux0, inMux1);
            }
            throw CompilationError(CompilationStep::CODE_GENERATION, "No instruction set", to_string());
        }
    }
        //only 1 input
    else {

        if(input0.second.hasValue)
        {
        	// unary operation with immediate
        	const SmallImmediate imm = input0.second;
        	if(firesSignal())
			{
				throw CompilationError(CompilationStep::CODE_GENERATION, "Signal is discarded, since the operation takes an immediate", toString(signal));
			}

        	if(isAddALU)
        	{
        		return new qpu_asm::ALUInstruction(unpackMode, packMode, conditional, COND_NEVER, setFlags, swap,
        				outReg.num, REG_NOP.num, OPMUL_NOP, opCodes.first, REG_NOP.num, imm, inMux0, MUTEX_NONE, MUTEX_NONE, MUTEX_NONE);
        	}
        	else if(isMulALU)
        	{
        		return new qpu_asm::ALUInstruction(unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
						REG_NOP.num, outReg.num, opCodes.second, OPADD_NOP, REG_NOP.num, imm, MUTEX_NONE, MUTEX_NONE, inMux0, MUTEX_NONE);
        	}
        }
        else
        {
			if (isAddALU) {
				return new qpu_asm::ALUInstruction(signal, unpackMode, packMode, conditional, COND_NEVER, setFlags, swap,
										  outReg.num, REG_NOP.num, OPMUL_NOP, opCodes.first,
										  getRegister(RegisterFile::PHYSICAL_A, input0.first, REG_NOP, this).num,
										  getRegister(RegisterFile::PHYSICAL_B, input0.first, REG_NOP, this).num,
										  inMux0, MUTEX_NONE, MUTEX_NONE, MUTEX_NONE);
			}
			else if (isMulALU) {
				return new qpu_asm::ALUInstruction(signal, unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
										  REG_NOP.num, outReg.num, opCodes.second, OPADD_NOP,
										  getRegister(RegisterFile::PHYSICAL_A, input0.first, REG_NOP, this).num,
										  getRegister(RegisterFile::PHYSICAL_B, input0.first, REG_NOP, this).num,
										  MUTEX_NONE, MUTEX_NONE, inMux0, MUTEX_NONE);
			}
        }
        throw CompilationError(CompilationStep::CODE_GENERATION, "No instruction set", to_string());
    }
}

bool Operation::mapsToASMInstruction() const
{
	auto pair = toOpCode(opCode);
	unsigned numOperands = 1;
	if(pair.first != OPADD_NOP)
		numOperands = pair.first.numOperands;
	else
		numOperands = pair.second.numOperands;

	//this instruction is not mapped to an ASM instruction, if either of the operands is undefined, since then the result is undefined too
	if(getFirstArg().isUndefined())
		return false;
	if(numOperands > 1 && getSecondArg().get().isUndefined())
		return false;
	return true;
}

const Value Operation::getFirstArg() const
{
	return getArgument(0);
}

const Optional<Value> Operation::getSecondArg() const
{
	return getArgument(1);
}

Optional<Value> Operation::precalculate() const
{
	Optional<Value> arg0 = getPrecalculatedValueForArg(0);
	if(!arg0.hasValue)
		return NO_VALUE;
	Optional<Value> arg1 = NO_VALUE;
    if (getSecondArg().hasValue)
    {
    	arg1 = getPrecalculatedValueForArg(1);
    	if(!arg1.hasValue)
    		return NO_VALUE;
    }
    std::pair<OpAdd, OpMul> opCodes = toOpCode(opCode);
    const Literal first = arg0.get().literal;
    const Literal second = arg1.hasValue ? arg1.get().literal : Literal(0L);
    bool firstInt = getFirstArg().literal.type == LiteralType::BOOL || getFirstArg().literal.type == LiteralType::INTEGER;
    bool secondInt = getSecondArg() && (getSecondArg().get().literal.type == LiteralType::BOOL || getSecondArg().get().literal.type == LiteralType::INTEGER);
    switch (opCodes.first.opCode) {
    case OPADD_ADD.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer + second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_AND.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer & second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_ASR.opCode:
        return NO_VALUE;
    case OPADD_CLZ.opCode:
        return NO_VALUE;
    case OPADD_FADD.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(first.real + second.real), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FMAX.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(std::max(first.real, second.real)), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FMAXABS.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(std::max(std::abs(first.real), std::abs(second.real))), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FMIN.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(std::min(first.real, second.real)), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FMINABS.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(std::min(std::abs(first.real), std::abs(second.real))), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FSUB.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(first.real - second.real), getFirstArg().type));
        return NO_VALUE;
    case OPADD_FTOI.opCode:
        if (!firstInt)
            return packMode.pack(Value(Literal((long) first.real), getFirstArg().type));
        return NO_VALUE;
    case OPADD_ITOF.opCode:
        if (firstInt)
            return packMode.pack(Value(Literal((double) first.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_MAX.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(std::max(first.integer, second.integer)), getFirstArg().type));
        return NO_VALUE;
    case OPADD_MIN.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(std::min(first.integer, second.integer)), getFirstArg().type));
        return NO_VALUE;
    case OPADD_NOT.opCode:
        if (firstInt)
            return packMode.pack(Value(Literal(~first.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_OR.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer | second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_ROR.opCode:
        return NO_VALUE;
    case OPADD_SHL.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer << second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_SHR.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer >> second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_SUB.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer - second.integer), getFirstArg().type));
        return NO_VALUE;
    case OPADD_V8ADDS.opCode:
        return NO_VALUE;
    case OPADD_V8SUBS.opCode:
        return NO_VALUE;
    case OPADD_XOR.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal(first.integer ^ second.integer), getFirstArg().type));
        return NO_VALUE;
    }
    switch (opCodes.second.opCode) {
    case OPMUL_FMUL.opCode:
        if (!firstInt && !secondInt)
            return packMode.pack(Value(Literal(first.real * second.real), getFirstArg().type));
        return NO_VALUE;
    case OPMUL_MUL24.opCode:
        if (firstInt && secondInt)
            return packMode.pack(Value(Literal((first.integer & 0xFFFFFF) * (second.integer & 0xFFFFFF)), getFirstArg().type));
        return NO_VALUE;
    case OPMUL_V8ADDS.opCode:
        return NO_VALUE;
    case OPMUL_V8MAX.opCode:
        return NO_VALUE;
    case OPMUL_V8MIN.opCode:
        return NO_VALUE;
    case OPMUL_V8MULD.opCode:
        return NO_VALUE;
    case OPMUL_V8SUBS.opCode:
        return NO_VALUE;
    }
    return NO_VALUE;
}

MoveOperation::MoveOperation(const Value& dest, const Value& arg, const ConditionCode cond, const SetFlag setFlags) :
IntermediateInstruction({true, dest}, cond, setFlags)
{
	setArgument(0, arg);
}

MoveOperation::~MoveOperation()
{

}

std::string MoveOperation::to_string() const
{
    return (getOutput().get().to_string(true) + " = ") +getSource().to_string() + createAdditionalInfoString();
}

IntermediateInstruction* MoveOperation::copyFor(Method& method, const std::string& localPrefix) const
{
    return (new MoveOperation(renameValue(method, getOutput(), localPrefix), renameValue(method, getSource(), localPrefix), conditional, setFlags))->copyExtrasFrom(this);
}

qpu_asm::Instruction* MoveOperation::convertToAsm(const FastMap<const Local*, Register>& registerMapping, const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    //simply call this method for intermediate-operation 
    //TODO y = (f)add x 0 or y = (f)mul x 1 or y = x and/or x
    Operation op("or", getOutput(), getSource(), getSource(), conditional, setFlags);
    op.packMode = packMode;
    op.signal = signal;
    op.unpackMode = unpackMode;
    return op.convertToAsm(registerMapping, labelMapping, instructionIndex);
}

Operation* MoveOperation::combineWith(const std::string& otherOpCode) const
{
    const auto opCodes = toOpCode(otherOpCode);
    Operation* op = nullptr;
    if (opCodes.second != OPMUL_NOP) {
        //use ADD ALU
        op = new Operation("or", getOutput(), getSource(), getSource(), conditional, setFlags);
    }
    else if (opCodes.first != OPADD_NOP) {
        //use MUL ALU
        {
            //TODO flags are not correct ?!?
            op = new Operation("v8min", getOutput(), getSource(), getSource(), conditional, setFlags);
        }
    }
    if (op != nullptr) {
        op->packMode = packMode;
        op->unpackMode = unpackMode;
        op->signal = signal;
        op->decoration = decoration;
    }
    //if the combination failed, the instructions remain separate
    return op;
}

bool MoveOperation::mapsToASMInstruction() const
{
	//this instruction is not mapped to an ASM instruction, if the source is undefined, since then the result is undefined too
	return !getSource().isUndefined();
}

Optional<Value> MoveOperation::precalculate() const
{
	return getPrecalculatedValueForArg(0);
}

void MoveOperation::setSource(const Value& value)
{
	setArgument(0, value);
}

const Value MoveOperation::getSource() const
{
	return getArgument(0);
}

VectorRotation::VectorRotation(const Value& dest, const Value& src, const Value& offset, const ConditionCode cond, const SetFlag setFlags) :
MoveOperation(dest, src, cond, setFlags)
{
    signal = Signaling::ALU_IMMEDIATE;
    setArgument(1, offset);
}

VectorRotation::~VectorRotation()
{

}

std::string VectorRotation::to_string() const
{
    return (getOutput().get().to_string(true) + " = ") + (getSource().to_string() + " ") + getOffset().immediate.toString() + createAdditionalInfoString();
}

IntermediateInstruction* VectorRotation::copyFor(Method& method, const std::string& localPrefix) const
{
    return (new VectorRotation(renameValue(method, getOutput(), localPrefix), renameValue(method, getSource(), localPrefix), renameValue(method, getOffset(), localPrefix), conditional, setFlags))->copyExtrasFrom(this);
}

qpu_asm::Instruction* VectorRotation::convertToAsm(const FastMap<const Local*, Register>& registerMapping, const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    const Register outReg = getOutput().get().hasType(ValueType::LOCAL) ? registerMapping.at(getOutput().get().local) : getOutput().get().reg;

    auto input = getInputValue(getSource(), registerMapping);
    auto rotationOffset = getInputValue(getOffset(), registerMapping);
    const InputMutex inMux = getInputMux(input.first, false, Optional<SmallImmediate>(false, SmallImmediate(0)));
    if (inMux == InputMutex::REGA || inMux == InputMutex::REGB) {
        throw CompilationError(CompilationStep::CODE_GENERATION, "Can't rotate from register as input", to_string());
    }

    const bool writeSwap = outReg.file == RegisterFile::PHYSICAL_A;
    const WriteSwap swap = writeSwap ? WriteSwap::SWAP : WriteSwap::DONT_SWAP;

    if (getOffset().hasType(ValueType::LITERAL)) {
        SmallImmediate imm = static_cast<SmallImmediate> (VECTOR_ROTATE_R5 + static_cast<unsigned>(rotationOffset.second.get()));
        return new qpu_asm::ALUInstruction(unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
                                  REG_NOP.num, outReg.num, OPMUL_V8MIN, OPADD_NOP,
                                  REG_NOP.num, imm, MUTEX_NONE, MUTEX_NONE, inMux, inMux);
    }
    else if(getOffset().hasType(ValueType::SMALL_IMMEDIATE))
    {
    	return new qpu_asm::ALUInstruction(unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
    	                                  REG_NOP.num, outReg.num, OPMUL_V8MIN, OPADD_NOP,
    	                                  REG_NOP.num, getOffset().immediate, MUTEX_NONE, MUTEX_NONE, inMux, inMux);
    }
    else
        //use offset from r5
        return new qpu_asm::ALUInstruction(unpackMode, packMode, COND_NEVER, conditional, setFlags, swap,
                                  REG_NOP.num, outReg.num, OPMUL_V8MIN, OPADD_NOP,
                                  REG_NOP.num, VECTOR_ROTATE_R5, MUTEX_NONE, MUTEX_NONE, inMux, inMux);
}

Operation* VectorRotation::combineWith(const std::string& otherOpCode) const
{
    //for now, don't support this
    return nullptr;
}

Optional<Value> VectorRotation::precalculate() const
{
	//for now, don't precalculate
	return NO_VALUE;
}

const Value VectorRotation::getOffset() const
{
	return getArgument(1);
}

Nop::Nop(const DelayType type, const Signaling signal) : IntermediateInstruction(NO_VALUE), type(type)
{
    this->signal = signal;
    this->canBeCombined = false;
}

Nop::~Nop()
{

}

std::string Nop::to_string() const
{
    return std::string("nop") + createAdditionalInfoString();
}

IntermediateInstruction* Nop::copyFor(Method& method, const std::string& localPrefix) const
{
    return (new Nop(type))->copyExtrasFrom(this);
}

qpu_asm::Instruction* Nop::convertToAsm(const FastMap<const Local*, Register>& registerMapping, const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    return new qpu_asm::ALUInstruction(signal, UNPACK_NOP, PACK_NOP, COND_NEVER, COND_NEVER, SetFlag::DONT_SET, WriteSwap::DONT_SWAP,
                              REG_NOP.num, REG_NOP.num, OPMUL_NOP, OPADD_NOP, REG_NOP.num, REG_NOP.num, InputMutex::ACC0, InputMutex::ACC0, InputMutex::ACC0, InputMutex::ACC0);
}

Comparison::Comparison(const std::string& comp, const Value& dest, const Value& val0, const Value& val1) :
Operation(comp, dest, val0, val1)
{

}

Comparison::~Comparison()
{

}

IntermediateInstruction* Comparison::copyFor(Method& method, const std::string& localPrefix) const
{
    return (new Comparison(opCode, renameValue(method, getOutput(), localPrefix), renameValue(method, getFirstArg(), localPrefix), renameValue(method, getSecondArg(), localPrefix)))->copyExtrasFrom(this);
}

CombinedOperation::CombinedOperation(Operation* op1, Operation* op2) : IntermediateInstruction(NO_VALUE), op1(op1), op2(op2)
{
	op1->parent = this;
	op2->parent = this;
}

CombinedOperation::~CombinedOperation()
{

}

FastMap<const Local*, LocalUser::Type> CombinedOperation::getUsedLocals() const
{
	FastMap<const Local*, LocalUser::Type> res = op1 != nullptr ? op1->getUsedLocals() : FastMap<const Local*, LocalUser::Type>{};
	const FastMap<const Local*, LocalUser::Type> tmp = op2 != nullptr ? op2->getUsedLocals() : FastMap<const Local*, LocalUser::Type>{};
	res.insert(tmp.begin(), tmp.end());
	return res;
}

void CombinedOperation::forUsedLocals(const std::function<void(const Local*, LocalUser::Type)>& consumer) const
{
	if(op1)
		op1->forUsedLocals(consumer);
	if(op2)
		op2->forUsedLocals(consumer);
}

bool CombinedOperation::readsLocal(const Local* local) const
{
	return (op1 && op1->readsLocal(local)) || (op2 && op2->readsLocal(local));
}

bool CombinedOperation::writesLocal(const Local* local) const
{
	return (op1 && op1->writesLocal(local)) || (op2 && op2->writesLocal(local));
}

void CombinedOperation::replaceLocal(const Local* oldLocal, const Local* newLocal, const Type type)
{
	op1->replaceLocal(oldLocal, newLocal, type);
	op2->replaceLocal(oldLocal, newLocal, type);
}

std::string CombinedOperation::to_string() const
{
    return (op1->to_string() + " and ") +op2->to_string();
}

qpu_asm::Instruction* CombinedOperation::convertToAsm(const FastMap<const Local*, Register>& registerMapping, const FastMap<const Local*, std::size_t>& labelMapping, const std::size_t instructionIndex) const
{
    const auto op1Codes = toOpCode(getFirstOp()->opCode);
    const IntermediateInstruction* addOp = op1Codes.first != OPADD_NOP ? op1.get() : op2.get();
    const IntermediateInstruction* mulOp = op1Codes.second != OPMUL_NOP ? op1.get() : op2.get();

    if(addOp != nullptr && mulOp != nullptr && addOp->getOutput().hasValue && mulOp->getOutput().hasValue && addOp->getOutput().get() != mulOp->getOutput().get())
    {
    	const Register addOut = addOp->getOutput().get().hasType(ValueType::LOCAL) ? registerMapping.at(addOp->getOutput().get().local) : addOp->getOutput().get().reg;
    	const Register mulOut = mulOp->getOutput().get().hasType(ValueType::LOCAL) ? registerMapping.at(mulOp->getOutput().get().local) : mulOp->getOutput().get().reg;
    	if(addOut != mulOut && !addOut.isAccumulator() && addOut.file == mulOut.file)
    		throw CompilationError(CompilationStep::CODE_GENERATION, "Can't map outputs of a combined instruction to two distinct registers in the same file", to_string());
    }

    std::unique_ptr<qpu_asm::ALUInstruction> addInstr((qpu_asm::ALUInstruction*) addOp->convertToAsm(registerMapping, labelMapping, instructionIndex));
    std::unique_ptr<qpu_asm::ALUInstruction> mulInstr((qpu_asm::ALUInstruction*) mulOp->convertToAsm(registerMapping, labelMapping, instructionIndex));

    addInstr->setMulCondition(mulInstr->getMulCondition());
    addInstr->setMulMutexA(mulInstr->getMulMutexA());
    addInstr->setMulMutexB(mulInstr->getMulMutexB());
    addInstr->setMulOut(mulInstr->getMulOut());
	//an instruction writing to accumulator or register-file A does not set the swap, only an instruction writing to file B does
	//so if any of the two instructions want to swap, we need to swap
	addInstr->setWriteSwap(add_flag(addInstr->getWriteSwap(), mulInstr->getWriteSwap()));
    addInstr->setMultiplication(mulInstr->getMultiplication());
    if (addInstr->getInputA() == REG_NOP.num)
        addInstr->setInputA(mulInstr->getInputA());
    if (addInstr->getInputB() == REG_NOP.num) {
        addInstr->setInputB(mulInstr->getInputB());
        //if ADD doesn't use register-file B and no small immediate, set signaling to value of MUL
        if (addInstr->getSig() == Signaling::NO_SIGNAL)
            addInstr->setSig(mulInstr->getSig());
    }

    return addInstr.release();
}

IntermediateInstruction* CombinedOperation::copyFor(Method& method, const std::string& localPrefix) const
{
    return (new CombinedOperation((Operation*) op1->copyFor(method, localPrefix), (Operation*) op2->copyFor(method, localPrefix)))->copyExtrasFrom(this);
}

bool CombinedOperation::mapsToASMInstruction() const
{
	return (op1 && op1->mapsToASMInstruction()) || (op2 && op2->mapsToASMInstruction());
}

const Operation* CombinedOperation::getFirstOp() const
{
	return dynamic_cast<const Operation*>(op1.get());
}

const Operation* CombinedOperation::getSecondOP() const
{
	return dynamic_cast<const Operation*>(op2.get());
}
