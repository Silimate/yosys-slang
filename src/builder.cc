//
// Yosys slang frontend
//
// Copyright 2024 Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#include "slang_frontend.h"

namespace slang_frontend {

using RTLIL::Cell;
using RTLIL::IdString;
using RTLIL::SigSpec;

SigSpec RTLILBuilder::ReduceBool(SigSpec a) {
	if (a.is_fully_const())
		return RTLIL::const_reduce_bool(a.as_const(), RTLIL::Const(), false, false, 1);
	return canvas->ReduceBool(NEW_ID, a, false);
}

SigSpec RTLILBuilder::Sub(SigSpec a, SigSpec b, bool is_signed) {
	if (b.is_fully_ones())
		return a;
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_sub(a.as_const(), b.as_const(), is_signed, is_signed,
								std::max(a.size(), b.size()) + 1);
	return canvas->Sub(NEW_ID, a, b, is_signed);
}

SigSpec RTLILBuilder::Demux(SigSpec a, SigSpec s) {
	log_assert(s.size() < 24);
	SigSpec zeropad(RTLIL::S0, a.size());
	if (s.is_fully_const()) {
		int idx_const = s.as_const().as_int();
		return {zeropad.repeat((1 << s.size()) - 1 - idx_const),
					a, zeropad.repeat(idx_const)};
	}
	return canvas->Demux(NEW_ID, a, s);
}

SigSpec RTLILBuilder::Le(SigSpec a, SigSpec b, bool is_signed) {
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_le(a.as_const(), b.as_const(), is_signed, is_signed, 1);
	return canvas->Le(NEW_ID, a, b, is_signed);
}

SigSpec RTLILBuilder::Ge(SigSpec a, SigSpec b, bool is_signed) {
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_ge(a.as_const(), b.as_const(), is_signed, is_signed, 1);
	return canvas->Ge(NEW_ID, a, b, is_signed);
}

SigSpec RTLILBuilder::Lt(SigSpec a, SigSpec b, bool is_signed) {
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_lt(a.as_const(), b.as_const(), is_signed, is_signed, 1);
	return canvas->Lt(NEW_ID, a, b, is_signed);
}

SigSpec RTLILBuilder::Eq(SigSpec a, SigSpec b) {
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_eq(a.as_const(), b.as_const(), false, false, 1);
	return canvas->Eq(NEW_ID, a, b);
}

SigSpec RTLILBuilder::EqWildcard(SigSpec a, SigSpec b) {
	log_assert(a.size() == b.size());
	log_assert(b.is_fully_const());

	for (int i = a.size() - 1; i >= 0; i--) {
		if (b[i] == RTLIL::Sx || b[i] == RTLIL::Sz) {
			a.remove(i);
			b.remove(i);
		}
	}
	log_assert(a.size() == b.size());
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_eq(a.as_const(), b.as_const(), false, false, 1);
	return canvas->Eq(NEW_ID, a, b);
}

SigSpec RTLILBuilder::LogicAnd(SigSpec a, SigSpec b) {
	if (a.is_fully_zero() || b.is_fully_zero())
		return RTLIL::Const(0, 1);
	if (a.is_fully_def() && b.size() == 1)
		return b;
	if (b.is_fully_def() && a.size() == 1)
		return a;
	return canvas->LogicAnd(NEW_ID, a, b);
}

SigSpec RTLILBuilder::LogicOr(SigSpec a, SigSpec b) {
	if (a.is_fully_ones() || b.is_fully_ones())
		return RTLIL::Const(1, 1);
	if (a.is_fully_zero() && b.is_fully_zero())
		return RTLIL::Const(0, 1);
	return canvas->LogicOr(NEW_ID, a, b);
}

SigSpec RTLILBuilder::LogicNot(SigSpec a) {
	if (a.is_fully_const())
		return RTLIL::const_logic_not(a.as_const(), RTLIL::Const(), false, false, -1);
	return canvas->LogicNot(NEW_ID, a);
}

SigSpec RTLILBuilder::Mux(SigSpec a, SigSpec b, SigSpec s) {
	log_assert(a.size() == b.size());
	log_assert(s.size() == 1);
	if (s[0] == RTLIL::S0)
		return a;
	if (s[0] == RTLIL::S1)
		return b;
	return canvas->Mux(NEW_ID, a, b, s);
}

SigSpec RTLILBuilder::Bwmux(SigSpec a, SigSpec b, SigSpec s) {
	log_assert(a.size() == b.size());
	log_assert(a.size() == s.size());
	if (s.is_fully_const()) {
		SigSpec result(RTLIL::Sx, a.size());
		for (int i = 0; i < a.size(); i++) {
			if (s[i] == RTLIL::S0)
				result[i] = a[i];
			else if (s[i] == RTLIL::S1)
				result[i] = b[i];
		}
		return result;
	}
	return canvas->Bwmux(NEW_ID, a, b, s);
}

SigSpec RTLILBuilder::Shift(SigSpec a, bool a_signed, SigSpec b,
							bool b_signed, int result_width)
{
	if (a.is_fully_const() && b.is_fully_const())
		return RTLIL::const_shift(a.as_const(), b.as_const(),
								  a_signed, b_signed, result_width);

	if (b.is_fully_const() && b.size() < 24) {
		log_assert(!a.empty());
		int shift_amount = b.as_int(b_signed);
		RTLIL::SigSpec ret;
		int i, j;
		for (i = shift_amount, j = 0; j < result_width; i++, j++) {
			if (a_signed && i >= a.size())
				ret.append(a.msb());
			else if (i >= a.size() || i < 0)
				ret.append(RTLIL::S0);
			else
				ret.append(a[i]);
		}
		return ret;
	}

	SigSpec y = canvas->addWire(NEW_ID, result_width);
	Cell *cell = canvas->addCell(NEW_ID, ID($shift));
	cell->parameters[Yosys::ID::A_SIGNED] = a_signed;
	cell->parameters[Yosys::ID::B_SIGNED] = b_signed;
	cell->parameters[Yosys::ID::A_WIDTH] = a.size();
	cell->parameters[Yosys::ID::B_WIDTH] = b.size();
	cell->parameters[Yosys::ID::Y_WIDTH] = y.size();
	cell->setPort(Yosys::ID::A, a);
	cell->setPort(Yosys::ID::B, b);
	cell->setPort(Yosys::ID::Y, y);
	return y;
}

SigSpec RTLILBuilder::Shiftx(SigSpec a, SigSpec s,
							 bool s_signed, int result_width)
{
	if (a.is_fully_const() && s.is_fully_const())
		return RTLIL::const_shiftx(a.as_const(), s.as_const(),
								   false, s_signed, result_width);
	SigSpec y = canvas->addWire(NEW_ID, result_width);
	canvas->addShiftx(NEW_ID, a, s, y, s_signed);
	return y;
}

SigSpec RTLILBuilder::Neg(SigSpec a, bool signed_)
{
	if (a.is_fully_const())
		return RTLIL::const_neg(a.as_const(), RTLIL::Const(),
								signed_, false, a.size() + 1);
	SigSpec y = canvas->addWire(NEW_ID, a.size() + 1);
	canvas->addNeg(NEW_ID, a, y, signed_);
	return y;
}

SigSpec RTLILBuilder::Bmux(SigSpec a, SigSpec s) {
	log_assert(a.size() % (1 << s.size()) == 0);
	log_assert(a.size() >= 1 << s.size());
	int stride = a.size() >> s.size();
	if (s.is_fully_def()) {
		return a.extract(s.as_const().as_int() * stride, stride);
	}
	return canvas->Bmux(NEW_ID, a, s);
}

SigSpec RTLILBuilder::Not(SigSpec a)
{
	if (a.is_fully_const())
		return RTLIL::const_not(a.as_const(), RTLIL::Const(), false, false, -1);
	return canvas->Not(NEW_ID, a);
}

namespace ThreeValued {
	int AND(int a, int b)
	{
		if (a < 0 || b < 0)
			return -1;
		if (a > 0 && b > 0)
			return 1;
		return 0;
	}

	int NOT(int lit)
	{
		return -lit;
	}

	int OR(int a, int b)
	{
		return NOT(AND(NOT(a), NOT(b)));
	}

	int XOR(int a, int b)
	{
		return OR(AND(a, NOT(b)), AND(NOT(a), b));
	}

	int XNOR(int a, int b)
	{
		return NOT(OR(AND(a, NOT(b)), AND(NOT(a), b)));
	}

	int CARRY(int a, int b, int c)
	{
		if (c > 0)
			return OR(a, b);
		else if (c < -1)
			return AND(a, b);

		return OR(AND(a, b), AND(c, OR(a, b)));
	}

	int convert(RTLIL::SigBit bit)
	{
		if (bit == RTLIL::S1)
			return 1;
		else if (bit == RTLIL::S0)
			return -1;
		else
			return 0;
	}
};

SigSpec RTLILBuilder::Biop(IdString op, SigSpec a, SigSpec b,
						   bool a_signed, bool b_signed, int y_width)
{
	if (a.is_fully_const() && b.is_fully_const()) {
#define OP(type) if (op == ID($##type)) return RTLIL::const_##type(a.as_const(), b.as_const(), a_signed, b_signed, y_width);
		OP(add)
		OP(sub)
		OP(mul)
		OP(divfloor)
		OP(div)
		OP(mod)
		OP(and)
		OP(or)
		OP(xor)
		OP(xnor)
		OP(eq)
		OP(ne)
		OP(nex)
		OP(eqx)
		OP(ge)
		OP(gt)
		OP(le)
		OP(lt)
		OP(logic_and)
		OP(logic_or)
		OP(sshl)
		OP(sshr)
		OP(shl)
		OP(shr)
		OP(pow)
#undef OP
	}

	if (op.in(ID($le), ID($lt), ID($gt), ID($ge)) && !a.empty() && !b.empty()) {
		int carry = op.in(ID($le), ID($ge)) ? -1 : 1;
		int al, bl;
		// Defer to three-valued evaluation over a representation of the operators.
		// This is a bit much, but I'm writing this tired and don't trust doing it
		// another way.
		int width = std::max(a.size(), b.size());
		for (int i = 0; i < width; i++) {
			RTLIL::SigBit abit = i < a.size() ? a[i] : (a_signed ? a.msb() : RTLIL::S0);
			RTLIL::SigBit bbit = i < b.size() ? b[i] : (b_signed ? b.msb() : RTLIL::S0);
			al = ThreeValued::convert(abit);
			bl = ThreeValued::convert(bbit);
			if (op.in(ID($gt), ID($ge)))
				std::swap(al, bl);
			if (i != width - 1)
				carry = ThreeValued::CARRY(al, ThreeValued::NOT(bl), carry);
		}
		int result = ThreeValued::XOR(carry, ThreeValued::XNOR(al, bl));
		if (result < 0)
			return SigSpec(RTLIL::S0, y_width);
		if (result > 0) {
			SigSpec ret = {RTLIL::S1};
			ret.extend_u0(y_width);
			return ret;
		}
	}

	if (op == ID($logic_and)) {
		if (a.is_fully_zero() || b.is_fully_zero())
			return SigSpec(RTLIL::S0, y_width);
	}

	if (op == ID($logic_or)) {
		// IMPROVEMENT: condition could be relaxed
		if ((a.is_fully_const() && a.as_bool()) || (b.is_fully_const() && b.as_bool())) {
			SigSpec ret = {RTLIL::S1};
			ret.extend_u0(y_width);
			return ret;
		}
	}

	Cell *cell = canvas->addCell(NEW_ID, op);
	cell->setPort(RTLIL::ID::A, a);
	cell->setPort(RTLIL::ID::B, b);
	cell->setParam(RTLIL::ID::A_WIDTH, a.size());
	cell->setParam(RTLIL::ID::B_WIDTH, b.size());
	cell->setParam(RTLIL::ID::A_SIGNED, a_signed);
	cell->setParam(RTLIL::ID::B_SIGNED, b_signed);
	cell->setParam(RTLIL::ID::Y_WIDTH, y_width);
	SigSpec ret = canvas->addWire(NEW_ID, y_width);
	cell->setPort(RTLIL::ID::Y, ret);
	return ret;
}

SigSpec RTLILBuilder::Unop(IdString op, SigSpec a, bool a_signed, int y_width)
{
	if (a.is_fully_const()) {
#define OP(type) if (op == ID($##type)) return RTLIL::const_##type(a.as_const(), {}, a_signed, false, y_width);
		OP(pos)
		OP(neg)
		OP(logic_not)
		OP(not)
		OP(reduce_or)
		OP(reduce_and)
		OP(reduce_xor)
		OP(reduce_xnor)
		OP(reduce_bool)
#undef OP
	}

	Cell *cell = canvas->addCell(NEW_ID, op);
	cell->setPort(RTLIL::ID::A, a);
	cell->setParam(RTLIL::ID::A_WIDTH, a.size());
	cell->setParam(RTLIL::ID::A_SIGNED, a_signed);
	cell->setParam(RTLIL::ID::Y_WIDTH, y_width);
	SigSpec ret = canvas->addWire(NEW_ID, y_width);
	cell->setPort(RTLIL::ID::Y, ret);
	return ret;
}

};
