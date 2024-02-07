//
// Yosys slang frontend
//
// Copyright 2024 Martin Povišer <povik@cutebit.org>
// Distributed under the terms of the ISC license, see LICENSE
//
#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/driver/Driver.h"
#include "slang/syntax/SyntaxPrinter.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/Json.h"
#include "slang/util/Util.h"

#include "kernel/fmt.h"
#include "kernel/register.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"
#include "kernel/utils.h"

PRIVATE_NAMESPACE_BEGIN

using Yosys::log;
using Yosys::log_error;
using Yosys::log_warning;
using Yosys::log_id;
using Yosys::log_signal;
using Yosys::ys_debug;
using Yosys::ceil_log2;

namespace RTLIL = Yosys::RTLIL;
namespace ast = slang::ast;

ast::Compilation *global_compilation;
const slang::SourceManager *global_sourcemgr;

slang::SourceRange source_location(const ast::Symbol &obj)			{ return slang::SourceRange(obj.location, obj.location); }
slang::SourceRange source_location(const ast::Expression &expr)		{ return expr.sourceRange; }
slang::SourceRange source_location(const ast::Statement &stmt)		{ return stmt.sourceRange; }
slang::SourceRange source_location(const ast::TimingControl &stmt)	{ return stmt.sourceRange; }

template<typename T>
std::string format_src(const T &obj)
{
	auto sm = global_sourcemgr;
	auto sr = source_location(obj);

	if (!sm->isFileLoc(sr.start()) || !sm->isFileLoc(sr.end()))
		return "";

	if (sr.start() == sr.end()) {
		auto loc = sr.start();
		std::string fn{sm->getFileName(loc)};
		return Yosys::stringf("%s:%d.%d", fn.c_str(),
			(int) sm->getLineNumber(loc), (int) sm->getColumnNumber(loc));
	} else {
		std::string fn{sm->getFileName(sr.start())};
		return Yosys::stringf("%s:%d.%d-%d.%d", fn.c_str(),
			(int) sm->getLineNumber(sr.start()), (int) sm->getColumnNumber(sr.start()),
			(int) sm->getLineNumber(sr.end()), (int) sm->getColumnNumber(sr.end()));
	}
}

template<typename T>
void unimplemented_(const T &obj, const char *file, int line, const char *condition)
{
	slang::JsonWriter writer;
	writer.setPrettyPrint(true);
	ast::ASTSerializer serializer(*global_compilation, writer);
	serializer.serialize(obj);
	std::cout << writer.view() << std::endl;
	auto loc = source_location(obj);
	log_assert(loc.start().buffer() == loc.end().buffer());
	std::string_view source_text = global_sourcemgr->getSourceText(loc.start().buffer());
	int col_no = global_sourcemgr->getColumnNumber(loc.start());
	const char *line_start = source_text.data() + loc.start().offset() - col_no + 1;
	const char *line_end = line_start;
	while (*line_end && *line_end != '\n' && *line_end != '\r') line_end++;
	std::cout << "Source line " << format_src(obj) << ": " << std::string_view(line_start, line_end) << std::endl;
	log_error("Feature unimplemented at %s:%d, see AST and code line dump above%s%s%s\n",
			  file, line, condition ? " (failed condition \"" : "", condition ? condition : "", condition ? "\")" : "");
}
#define require(obj, property) { if (!(property)) unimplemented_(obj, __FILE__, __LINE__, #property); }
#define unimplemented(obj) { unimplemented_(obj, __FILE__, __LINE__, NULL); }

const RTLIL::IdString id(const std::string_view &view)
{
	return RTLIL::escape_id(std::string(view));
}

static const RTLIL::IdString net_id(const ast::Symbol &symbol)
{
	std::string hierPath;
	symbol.getHierarchicalPath(hierPath);
	return RTLIL::escape_id(hierPath);
}

static const RTLIL::Const svint_const(const slang::SVInt &svint)
{
	RTLIL::Const ret;
	ret.bits.reserve(svint.getBitWidth());
	for (int i = 0; i < (int) svint.getBitWidth(); i++)
	switch (svint[i].value) {
	case 0: ret.bits.push_back(RTLIL::State::S0); break;
	case 1: ret.bits.push_back(RTLIL::State::S1); break;
	case slang::logic_t::X_VALUE: ret.bits.push_back(RTLIL::State::Sx); break;
	case slang::logic_t::Z_VALUE: ret.bits.push_back(RTLIL::State::Sz); break;
	}
	return ret;
}

static const RTLIL::Const const_const(const slang::ConstantValue &constval)
{
	log_assert(!constval.isReal());
	log_assert(!constval.isShortReal());
	log_assert(!constval.isNullHandle());
	log_assert(!constval.isUnbounded());
	log_assert(!constval.isMap());
	log_assert(!constval.isQueue());
	log_assert(!constval.isUnion());

	if (constval.isInteger()) {
		return svint_const(constval.integer());
	} else if (constval.isUnpacked()) {
		RTLIL::Const ret;
		// TODO: is this right?
		for (auto &el : constval.elements()) {
			auto piece = const_const(el);
			ret.bits.insert(ret.bits.begin(), piece.bits.begin(), piece.bits.end());
		}
		log_assert(ret.size() == constval.getBitstreamWidth());
		return ret;
	} else if (constval.isString()) {
		RTLIL::Const ret = svint_const(constval.convertToInt().integer());
		ret.flags |= RTLIL::CONST_FLAG_STRING;
		return ret;
	}
}

template<typename T>
void transfer_attrs(T &from, RTLIL::AttrObject *to)
{
	auto src = format_src(from);
	if (!src.empty())
		to->attributes[Yosys::ID::src] = src;

	for (auto attr : global_compilation->getAttributes(from)) {
		require(*attr, attr->getValue().isInteger());
		to->attributes[id(attr->name)] = svint_const(attr->getValue().integer());
	}
}


static const RTLIL::SigSpec evaluate_lhs(RTLIL::Module *mod, const ast::Expression &expr)
{
	RTLIL::SigSpec ret;

	switch (expr.kind) {
	case ast::ExpressionKind::NamedValue:
		{
			const ast::Symbol &sym = expr.as<ast::NamedValueExpression>().symbol;
			RTLIL::Wire *wire = mod->wire(net_id(sym));
			log_assert(wire);
			ret = wire;
		}
		break;
	case ast::ExpressionKind::RangeSelect:
		{
			const ast::RangeSelectExpression &sel = expr.as<ast::RangeSelectExpression>();
			require(expr, sel.getSelectionKind() == ast::RangeSelectionKind::Simple);
			require(expr, sel.left().constant && sel.right().constant);
			int left = sel.left().constant->integer().as<int>().value();
			int right = sel.right().constant->integer().as<int>().value();
			require(expr, sel.value().type->hasFixedRange());
			auto range = sel.value().type->getFixedRange();
			int raw_left = range.translateIndex(left);
			int raw_right = range.translateIndex(right);
			log_assert(sel.value().type->getBitstreamWidth() % range.width() == 0);
			int stride = sel.value().type->getBitstreamWidth() / range.width();
			ret = evaluate_lhs(mod, sel.value()).extract(raw_right * stride,
												stride * (raw_left - raw_right + 1));
		}
		break;
	case ast::ExpressionKind::Concatenation:
		{
			const ast::ConcatenationExpression &concat = expr.as<ast::ConcatenationExpression>();
			for (auto op : concat.operands())
				ret = {ret, evaluate_lhs(mod, *op)};
		}
		break;
	case ast::ExpressionKind::ElementSelect:
		{
			const ast::ElementSelectExpression &elemsel = expr.as<ast::ElementSelectExpression>();
			require(expr, elemsel.selector().constant);
			require(expr, elemsel.value().type->isArray() && elemsel.value().type->hasFixedRange());
			int idx = elemsel.selector().constant->integer().as<int>().value();
			int stride = elemsel.type->getBitstreamWidth();
			uint32_t raw_idx = elemsel.value().type->getFixedRange().translateIndex(idx);
			ret = evaluate_lhs(mod, elemsel.value()).extract(stride * raw_idx, stride);
		}
		break;
	case ast::ExpressionKind::MemberAccess:
		{
			const auto &acc = expr.as<ast::MemberAccessExpression>();
			require(expr, acc.member.kind == ast::SymbolKind::Field);
			const auto &member = acc.member.as<ast::FieldSymbol>();
			require(acc, member.randMode == ast::RandMode::None);
			return evaluate_lhs(mod, acc.value()).extract(member.bitOffset,
								expr.type->getBitstreamWidth());
		}
		break;
	default:
		unimplemented(expr);
		break;
	}

	log_assert(expr.type->isFixedSize());
	log_assert(ret.size() == (int) expr.type->getBitstreamWidth());
	return ret;
}

struct ProcedureContext
{
	// rvalue substitutions from blocking assignments
	Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> rvalue_subs;

	// arguments
	Yosys::dict<const ast::Symbol*, RTLIL::SigSpec, Yosys::hash_ptr_ops> args;
};

static RTLIL::SigSpec evaluate_function(RTLIL::Module *mod, const ast::CallExpression &call,
										ProcedureContext *ctx);

static const RTLIL::SigSpec evaluate_rhs(RTLIL::Module *mod, const ast::Expression &expr,
										 ProcedureContext *ctx);

static const std::pair<RTLIL::SigSpec, RTLIL::SigBit> translate_index(RTLIL::Module *mod, const ast::Expression &idxexpr,
																	  slang::ConstantRange range, ProcedureContext *ctx)
{
	RTLIL::SigSpec idx = evaluate_rhs(mod, idxexpr, ctx);
	bool idx_signed = idxexpr.type->isSigned();

	if (!idx_signed) {
		idx.append(RTLIL::S0);
		idx_signed = true;
	}

	RTLIL::SigBit valid = mod->LogicAnd(NEW_ID,
		mod->Le(NEW_ID, idx, RTLIL::Const(range.upper()), /* is_signed */ true),
		mod->Ge(NEW_ID, idx, RTLIL::Const(range.lower()), /* is_signed */ true)
	);

	RTLIL::SigSpec raw_idx;
	if (range.left > range.right)
		raw_idx = mod->Sub(NEW_ID, idx, RTLIL::Const(range.right), /* is_signed */ true);
	else
		raw_idx = mod->Sub(NEW_ID, RTLIL::Const(range.right), idx, /* is_signed */ true);
	raw_idx.extend_u0(ceil_log2(range.width()));
	return std::make_pair(raw_idx, valid);
}

static const RTLIL::SigSpec evaluate_rhs(RTLIL::Module *mod, const ast::Expression &expr,
										 ProcedureContext *ctx)
{
	RTLIL::SigSpec ret;

	{
		// TODO: we seem to need this for `expr.constant`, are we using it right?
		ast::ASTContext ctx(global_compilation->getRoot(), ast::LookupLocation::max);
		ctx.tryEval(expr);
	}

	if (true && expr.constant) {
		ret = svint_const(expr.constant->integer());
		goto done;
	}

	switch (expr.kind) {
	case ast::ExpressionKind::NamedValue:
		{
			const ast::Symbol &sym = expr.as<ast::NamedValueExpression>().symbol;

			switch (sym.kind) {
			case ast::SymbolKind::Net:
			case ast::SymbolKind::Variable:
				{
					const auto &valsym = sym.as<ast::ValueSymbol>();
					require(expr, valsym.getParentScope()->asSymbol().kind == ast::SymbolKind::InstanceBody);
					RTLIL::Wire *wire = mod->wire(net_id(sym));
					log_assert(wire);
					ret = wire;
					if (ctx)
						ret.replace(ctx->rvalue_subs);
				}
				break;
			case ast::SymbolKind::Parameter:
				{
					auto &valsym = sym.as<ast::ValueSymbol>();
					require(valsym, valsym.getInitializer());
					auto exprconst = valsym.getInitializer()->constant;
					require(valsym, exprconst && exprconst->isInteger());
					ret = svint_const(exprconst->integer());
				}
				break;
			case ast::SymbolKind::FormalArgument:
				{
					require(expr, ctx && ctx->args.count(&sym));
					ret = ctx->args.at(&sym);
				}
				break;
			default:
				unimplemented(sym);
			}
		}
		break;
	case ast::ExpressionKind::UnaryOp:
		{
			const ast::UnaryExpression &unop = expr.as<ast::UnaryExpression>();
			RTLIL::SigSpec left = evaluate_rhs(mod, unop.operand(), ctx);
			bool invert = false;

			RTLIL::IdString type;
			switch (unop.op) {
			case ast::UnaryOperator::LogicalNot: type = ID($logic_not); break;
			case ast::UnaryOperator::BitwiseNot: type = ID($not); break;
			case ast::UnaryOperator::BitwiseOr: type = ID($reduce_or); break;
			case ast::UnaryOperator::BitwiseAnd: type = ID($reduce_and); break;
			case ast::UnaryOperator::BitwiseNand: type = ID($reduce_and); invert = true; break;
			case ast::UnaryOperator::BitwiseNor: type = ID($reduce_or); invert = true; break;
			default:
				unimplemented(unop);
			}

			RTLIL::Cell *cell = mod->addCell(NEW_ID, type);
			cell->setPort(RTLIL::ID::A, left);
			cell->setParam(RTLIL::ID::A_WIDTH, left.size());
			cell->setParam(RTLIL::ID::A_SIGNED, unop.operand().type->isSigned());
			cell->setParam(RTLIL::ID::Y_WIDTH, expr.type->getBitstreamWidth());
			ret = mod->addWire(NEW_ID, expr.type->getBitstreamWidth());
			cell->setPort(RTLIL::ID::Y, ret);
			transfer_attrs(unop, cell);

			if (invert) {
				RTLIL::SigSpec new_ret = mod->addWire(NEW_ID, 1);
				transfer_attrs(unop, mod->addLogicNot(NEW_ID, ret, new_ret));
			}
		}
		break;
	case ast::ExpressionKind::BinaryOp:
		{
			const ast::BinaryExpression &biop = expr.as<ast::BinaryExpression>();
			RTLIL::SigSpec left = evaluate_rhs(mod, biop.left(), ctx);
			RTLIL::SigSpec right = evaluate_rhs(mod, biop.right(), ctx);

			RTLIL::IdString type;
			switch (biop.op) {
			case ast::BinaryOperator::Add:      type = ID($add); break;
			case ast::BinaryOperator::Subtract: type = ID($sub); break;
			case ast::BinaryOperator::Multiply:	type = ID($mul); break;
			case ast::BinaryOperator::Divide:	type = ID($divfloor); break; // TODO: check
			case ast::BinaryOperator::Mod:		type = ID($mod); break; // TODO: check
			case ast::BinaryOperator::BinaryAnd: type = ID($and); break;
			case ast::BinaryOperator::BinaryOr:	type = ID($or); break;
			case ast::BinaryOperator::BinaryXor:	type = ID($xor); break;
			case ast::BinaryOperator::BinaryXnor:	type = ID($xnor); break;
			case ast::BinaryOperator::Equality:		type = ID($eq); break;
			case ast::BinaryOperator::Inequality:	type = ID($ne); break;
			//case ast::BinaryOperator::CaseEquality;
			//case ast::BinaryOperator::CaseInequality;
			case ast::BinaryOperator::GreaterThanEqual:	type = ID($ge); break;
			case ast::BinaryOperator::GreaterThan:		type = ID($gt); break;
			case ast::BinaryOperator::LessThanEqual:	type = ID($le); break;
			case ast::BinaryOperator::LessThan:			type = ID($lt); break;
			//case ast::BinaryOperator::WildcardEquality;
			//case ast::BinaryOperator::WildcardInequality;
			case ast::BinaryOperator::LogicalAnd:	type = ID($logic_and); break;
			case ast::BinaryOperator::LogicalOr:	type = ID($logic_or); break;
			//case ast::BinaryOperator::LogicalImplication;
			//case ast::BinaryOperator::LogicalEquivalence;
			case ast::BinaryOperator::LogicalShiftLeft:	type = ID($sshl); break;
			case ast::BinaryOperator::LogicalShiftRight:	type = ID($sshr); break;
			case ast::BinaryOperator::ArithmeticShiftLeft:	type = ID($shl); break; // TODO: check shl vs sshl
			case ast::BinaryOperator::ArithmeticShiftRight:	type = ID($shr); break;
			case ast::BinaryOperator::Power:	type = ID($pow); break;
			default:
				unimplemented(biop);
			}

			RTLIL::Cell *cell = mod->addCell(NEW_ID, type);
			cell->setPort(RTLIL::ID::A, left);
			cell->setPort(RTLIL::ID::B, right);
			cell->setParam(RTLIL::ID::A_WIDTH, left.size());
			cell->setParam(RTLIL::ID::B_WIDTH, right.size());
			cell->setParam(RTLIL::ID::A_SIGNED, biop.left().type->isSigned());
			cell->setParam(RTLIL::ID::B_SIGNED, biop.right().type->isSigned());
			cell->setParam(RTLIL::ID::Y_WIDTH, expr.type->getBitWidth());
			ret = mod->addWire(NEW_ID, expr.type->getBitstreamWidth());
			cell->setPort(RTLIL::ID::Y, ret);
			transfer_attrs(biop, cell);

			// fixups
			if (cell->type == ID($shr)) {
				// TODO: is this kosher?
				cell->setParam(RTLIL::ID::B_SIGNED, false);
			}

			if (cell->type.in(ID($sshr), ID($sshl))) {
				// TODO: is this kosher?
				cell->setParam(RTLIL::ID::A_SIGNED, false);
				cell->setParam(RTLIL::ID::B_SIGNED, false);
			}
		}
		break;
	case ast::ExpressionKind::Conversion:
		{
			const ast::ConversionExpression &conv = expr.as<ast::ConversionExpression>();
			const ast::Type &from = conv.operand().type->getCanonicalType();
			const ast::Type &to = conv.type->getCanonicalType();
			require(expr, from.isIntegral() /* && from.isScalar() */);
			require(expr, to.isIntegral() /* && to.isScalar() */);
			require(conv, from.isSigned() == to.isSigned() || to.getBitWidth() <= from.getBitWidth());
			ret = evaluate_rhs(mod, conv.operand(), ctx);
			ret.extend_u0((int) to.getBitWidth(), to.isSigned());
		}
		break;
	case ast::ExpressionKind::IntegerLiteral:
		{
			const ast::IntegerLiteral &lit = expr.as<ast::IntegerLiteral>();
			ret = svint_const(lit.getValue());
		}
		break;
	case ast::ExpressionKind::RangeSelect:
		{
			const ast::RangeSelectExpression &sel = expr.as<ast::RangeSelectExpression>();
			require(expr, sel.getSelectionKind() == ast::RangeSelectionKind::Simple);
			require(expr, sel.left().constant && sel.right().constant);
			int left = sel.left().constant->integer().as<int>().value();
			int right = sel.right().constant->integer().as<int>().value();
			require(expr, sel.value().type->hasFixedRange());
			auto range = sel.value().type->getFixedRange();
			int raw_left = range.translateIndex(left);
			int raw_right = range.translateIndex(right);
			log_assert(sel.value().type->getBitstreamWidth() % range.width() == 0);
			int stride = sel.value().type->getBitstreamWidth() / range.width();
			ret = evaluate_rhs(mod, sel.value(), ctx).extract(raw_right * stride,
												stride * (raw_left - raw_right + 1));
		}
		break;
	case ast::ExpressionKind::ElementSelect:
		{
			const ast::ElementSelectExpression &elemsel = expr.as<ast::ElementSelectExpression>();
			require(expr, elemsel.value().type->isArray() && elemsel.value().type->hasFixedRange());
			int stride = elemsel.type->getBitstreamWidth();
			RTLIL::SigSpec base_value = evaluate_rhs(mod, elemsel.value(), ctx);
			log_assert(base_value.size() % stride == 0);
			auto range = elemsel.value().type->getFixedRange();
			RTLIL::SigSpec raw_idx;
			RTLIL::SigBit valid;
			std::tie(raw_idx, valid) = translate_index(mod, elemsel.selector(), range, ctx);
			log_assert(stride * (1 << raw_idx.size()) >= base_value.size());
			base_value.append(RTLIL::SigSpec(RTLIL::Sx, stride * (1 << raw_idx.size()) - base_value.size()));
			// TODO: check what's proper out-of-range handling
			ret = mod->Mux(NEW_ID, RTLIL::SigSpec(RTLIL::State::Sx, stride), mod->Bmux(NEW_ID, base_value, raw_idx), valid);
		}
		break;
	case ast::ExpressionKind::Concatenation:
		{
			const ast::ConcatenationExpression &concat = expr.as<ast::ConcatenationExpression>();
			for (auto op : concat.operands())
				ret = {ret, evaluate_rhs(mod, *op, ctx)};
		}
		break;
	case ast::ExpressionKind::ConditionalOp:
		{
			const auto &ternary = expr.as<ast::ConditionalExpression>();

			require(expr, ternary.conditions.size() == 1);
			require(expr, !ternary.conditions[0].pattern);

			ret = mod->Mux(NEW_ID,
				evaluate_rhs(mod, ternary.right(), ctx),
				evaluate_rhs(mod, ternary.left(), ctx),
				mod->ReduceBool(NEW_ID, evaluate_rhs(mod, *(ternary.conditions[0].expr), ctx))
			);
		}
		break;
	case ast::ExpressionKind::Replication:
		{
			const auto &repl = expr.as<ast::ReplicationExpression>();
			require(expr, repl.count().constant); // TODO: message
			int reps = repl.count().constant->integer().as<int>().value(); // TODO: checking
			RTLIL::SigSpec concat = evaluate_rhs(mod, repl.concat(), ctx);
			for (int i = 0; i < reps; i++)
				ret.append(concat);
		}
		break;
	case ast::ExpressionKind::MemberAccess:
		{
			const auto &acc = expr.as<ast::MemberAccessExpression>();
			require(expr, acc.member.kind == ast::SymbolKind::Field);
			const auto &member = acc.member.as<ast::FieldSymbol>();
			require(acc, member.randMode == ast::RandMode::None);
			return evaluate_rhs(mod, acc.value(), ctx).extract(member.bitOffset,
								expr.type->getBitstreamWidth());
		}
		break;
	case ast::ExpressionKind::Call:
		{
			const auto &call = expr.as<ast::CallExpression>();
			if (call.isSystemCall()) {
				require(expr, call.getSubroutineName() == "$signed");
				require(expr, call.arguments().size() == 1);
				ret = evaluate_rhs(mod, *call.arguments()[0], ctx);
			} else {
				const auto &subr = *std::get<0>(call.subroutine);
				require(subr, subr.subroutineKind == ast::SubroutineKind::Function);
				return evaluate_function(mod, call, ctx);

			}
		}
		break;
	default:
		unimplemented(expr);
	}

done:
	log_assert(expr.type->isFixedSize());
	log_assert(ret.size() == (int) expr.type->getBitstreamWidth());
	return ret;
}

struct SwitchBuilder {
	RTLIL::CaseRule *parent;
	RTLIL::SwitchRule *sw;
	Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> *rvalue_subs;
	Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> rvalue_subs_save;

	std::vector<std::pair<RTLIL::CaseRule *, RTLIL::SigSig>> branch_updates;

	SwitchBuilder(RTLIL::CaseRule *parent, Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> *rvalue_subs,
				  RTLIL::SigSpec signal)
		: parent(parent), rvalue_subs(rvalue_subs), rvalue_subs_save(*rvalue_subs)
	{
		sw = new RTLIL::SwitchRule;
		sw->signal = signal;
		parent->switches.push_back(sw);
	}

	void branch(std::vector<RTLIL::SigSpec> compare,
				std::function<void(RTLIL::CaseRule *case_rule)> f)
	{
		RTLIL::CaseRule *case_rule = new RTLIL::CaseRule;	
		sw->cases.push_back(case_rule);
		case_rule->compare = compare;
		f(case_rule);

		RTLIL::SigSpec update;
		for (auto pair : *rvalue_subs)
		if (!rvalue_subs_save.count(pair.first)
				|| pair.second != rvalue_subs_save.at(pair.first))
			update.append(pair.first);
		update.sort();

		RTLIL::SigSpec update_map = update;
		update_map.replace(*rvalue_subs);
		branch_updates.push_back(std::make_pair(case_rule, RTLIL::SigSig(update, update_map)));

		*rvalue_subs = rvalue_subs_save;
	}

	void finish(RTLIL::Module *mod)
	{
		RTLIL::SigSpec updated_anybranch;
		for (auto &branch : branch_updates)
			updated_anybranch.append(branch.second.first);
		updated_anybranch.sort_and_unify();

		for (auto chunk : updated_anybranch.chunks()) {
			RTLIL::SigSpec w = mod->addWire(NEW_ID, chunk.size());
			RTLIL::SigSpec w_default = chunk;
			w_default.replace(*rvalue_subs);
			parent->actions.push_back(RTLIL::SigSig(w, w_default));
			for (int i = 0; i < chunk.size(); i++)
				(*rvalue_subs)[RTLIL::SigSpec(chunk)[i]] = w[i];
		}

		for (auto &branch : branch_updates) {
			RTLIL::CaseRule *rule = branch.first;
			RTLIL::SigSpec target = branch.second.first;
			RTLIL::SigSpec source = branch.second.second;
			int done = 0;
			for (auto chunk : target.chunks()) {
				RTLIL::SigSpec target_w = chunk;
				target_w.replace(*rvalue_subs);
				rule->actions.push_back(RTLIL::SigSig(target_w, source.extract(done, chunk.size())));
				done += chunk.size();
			}
		}
	}
};

void crop_zero_mask(const RTLIL::SigSpec &mask, RTLIL::SigSpec &target)
{
	for (int i = mask.size() - 1; i >= 0; i--) {
		if (mask[i] == RTLIL::S0)
			target.remove(i, 1);
	}
}

struct ProceduralVisitor : public ast::ASTVisitor<ProceduralVisitor, true, false> {
public:
	RTLIL::Module *mod;
	RTLIL::Process *proc;
	RTLIL::CaseRule *current_case;

	ProcedureContext ctx;

	Yosys::SigPool assigned_blocking;
	Yosys::SigPool assigned_nonblocking;

	// TODO: static
	int print_priority = 0;

	enum Mode { ALWAYS, FUNCTION } mode;

	ProceduralVisitor(RTLIL::Module *mod, RTLIL::Process *proc, Mode mode)
			: mod(mod), proc(proc), mode(mode) {
		RTLIL::SwitchRule *top_switch = new RTLIL::SwitchRule;
		proc->root_case.switches.push_back(top_switch);
		current_case = new RTLIL::CaseRule;
		top_switch->cases.push_back(current_case);
	}

	Yosys::dict<RTLIL::SigBit, RTLIL::SigBit> staging;
	RTLIL::SigSpec staging_signal(RTLIL::SigSpec lvalue)
	{
		RTLIL::SigSpec to_create;
		for (auto bit : lvalue) {
			log_assert(bit.wire);
			if (!staging.count(bit))
				to_create.append(bit);
		}

		to_create.sort_and_unify();
		for (auto chunk : to_create.chunks()) {
			RTLIL::SigSpec w = mod->addWire(NEW_ID_SUFFIX("staging"), chunk.size());
			for (int i = 0; i < chunk.size(); i++)
				staging[RTLIL::SigSpec(chunk)[i]] = w[i];
		}

		lvalue.replace(staging);
		return lvalue;
	}

	void staging_done()
	{
		RTLIL::SigSpec all_driven;
		for (auto pair : staging)
			all_driven.append(pair.first);
		all_driven.sort_and_unify();

		RTLIL::CaseRule *root_case = &proc->root_case;
		for (auto chunk : all_driven.chunks()) {
			RTLIL::SigSpec mapped = chunk;
			mapped.replace(staging);
			for (auto sync : proc->syncs)
				sync->actions.push_back(RTLIL::SigSig(chunk, mapped));
			root_case->actions.push_back(RTLIL::SigSig(mapped, chunk));
		}
	}

	// Return an enable signal for the current context
	RTLIL::SigBit context_enable()
	{
		RTLIL::SigBit ret = mod->addWire(NEW_ID, 1);
		proc->root_case.actions.emplace_back(ret, RTLIL::State::S0);
		current_case->actions.emplace_back(ret, RTLIL::State::S1);
		return ret;
	}

	// For $check, $print cells
	void set_cell_trigger(RTLIL::Cell *cell)
	{
		bool implicit = false;
		RTLIL::SigSpec triggers;
		RTLIL::Const polarity;

		for (auto sync : proc->syncs)
		switch(sync->type) {
		case RTLIL::STn:
		case RTLIL::STp:
			log_assert(sync->signal.size() == 1);
			triggers.append(sync->signal);
			polarity.bits.push_back(sync->type == RTLIL::STp ? RTLIL::S1 : RTLIL::S0);
			break;

		case RTLIL::STa:
			implicit = true;
			break;

		default:
			log_abort();
		}

		log_assert(!triggers.empty() || implicit);
		log_assert(!(!triggers.empty() && implicit));
		cell->parameters[Yosys::ID::TRG_ENABLE] = !implicit;
		cell->parameters[Yosys::ID::TRG_WIDTH] = triggers.size();
		cell->parameters[Yosys::ID::TRG_POLARITY] = polarity;
		cell->setPort(Yosys::ID::TRG, triggers);
		cell->setPort(Yosys::ID::EN, context_enable());
	}

	// TODO: add other kids of statements

	void handle(const ast::ExpressionStatement &expr)
	{
		switch (expr.expr.kind) {
		case ast::ExpressionKind::Call:
			{
				auto &call = expr.expr.as<ast::CallExpression>();
				if (call.getSubroutineName() == "empty_statement") {
					return; // TODO: workaround for picorv32, do better
				} else if (call.getSubroutineName() == "$display") {
					auto cell = mod->addCell(NEW_ID, ID($print));
					transfer_attrs(expr, cell);
					set_cell_trigger(cell);
					cell->parameters[Yosys::ID::PRIORITY] = --print_priority;
					std::vector<Yosys::VerilogFmtArg> fmt_args;
					for (auto arg : call.arguments()) {
						log_assert(arg);
						Yosys::VerilogFmtArg fmt_arg = {};
						// TODO: location info in fmt_arg
						switch (arg->kind) {
						case ast::ExpressionKind::StringLiteral:
							fmt_arg.type = Yosys::VerilogFmtArg::STRING;
							fmt_arg.str = std::string{arg->as<ast::StringLiteral>().getValue()};
							fmt_arg.sig = {}; // TODO
							break;
						case ast::ExpressionKind::Call:
							if (arg->as<ast::CallExpression>().getSubroutineName() == "$time") {
								fmt_arg.type = Yosys::VerilogFmtArg::TIME;
								break;
							} else if (arg->as<ast::CallExpression>().getSubroutineName() == "$realtime") {
								fmt_arg.type = Yosys::VerilogFmtArg::TIME;
								fmt_arg.realtime = true;
								break;
							} else {
								/* fallthrough */
							}
						default:
							fmt_arg.type = Yosys::VerilogFmtArg::INTEGER;
							fmt_arg.sig = evaluate_rhs(mod, *arg, &ctx);
							fmt_arg.signed_ = arg->type->isSigned();
							break;
						}
						fmt_args.push_back(fmt_arg);						
						
					}
					Yosys::Fmt fmt = {};
					fmt.parse_verilog(fmt_args, /* sformat_like */ false, /* default_base */ 10,
									  std::string{call.getSubroutineName()}, mod->name);
					fmt.append_string("\n");
					fmt.emit_rtlil(cell);
				} else {
					unimplemented(expr);
				}
			}
			return;
		case ast::ExpressionKind::Assignment:
			/* handled further below */
			break;
		default:
			unimplemented(expr);
		}

		require(expr, expr.expr.kind == ast::ExpressionKind::Assignment);
		const auto &assign = expr.expr.as<ast::AssignmentExpression>();
		bool blocking = !assign.isNonBlocking();

		RTLIL::SigSpec rvalue = evaluate_rhs(mod, assign.right(), &ctx);

		const ast::Expression *raw_lexpr = &assign.left();
		RTLIL::SigSpec raw_mask = RTLIL::SigSpec(RTLIL::S1, rvalue.size()), raw_rvalue = rvalue;

		bool finished_etching = false;
		while (!finished_etching) {
			switch (raw_lexpr->kind) {
			case ast::ExpressionKind::RangeSelect:
				{
					const ast::RangeSelectExpression &sel = raw_lexpr->as<ast::RangeSelectExpression>();
					require(expr, sel.getSelectionKind() == ast::RangeSelectionKind::Simple);
					require(expr, sel.left().constant && sel.right().constant);
					int left = sel.left().constant->integer().as<int>().value();
					int right = sel.right().constant->integer().as<int>().value();
					require(expr, sel.value().type->hasFixedRange());
					auto range = sel.value().type->getFixedRange();
					int raw_left = range.translateIndex(left);
					int raw_right = range.translateIndex(right);
					log_assert(sel.value().type->getBitstreamWidth() % range.width() == 0);
					int stride = sel.value().type->getBitstreamWidth() / range.width();
					RTLIL::SigSpec elem_0(RTLIL::S0, stride);
					RTLIL::SigSpec elem_x(RTLIL::Sx, stride);
					raw_mask = {elem_0.repeat(range.width() - raw_left - 1), raw_mask, elem_0.repeat(raw_right)};
					raw_rvalue = {elem_x.repeat(range.width() - raw_left - 1), raw_rvalue, elem_x.repeat(raw_right)};
					raw_lexpr = &sel.value();
				}
				break;
			case ast::ExpressionKind::ElementSelect:
				{
					auto &elemsel = raw_lexpr->as<ast::ElementSelectExpression>();
					require(expr, elemsel.value().type->isArray() && elemsel.value().type->hasFixedRange());
					int stride = elemsel.type->getBitstreamWidth();
					auto range = elemsel.value().type->getFixedRange();
					RTLIL::SigSpec raw_idx;
					RTLIL::SigBit valid;
					std::tie(raw_idx, valid) = translate_index(mod, elemsel.selector(), range, &ctx);
					// TODO: use valid
					raw_mask = mod->Demux(NEW_ID, raw_mask, raw_idx);
					raw_mask.extend_u0(stride * range.width());
					raw_rvalue = raw_rvalue.repeat(range.width());
					raw_lexpr = &elemsel.value();
				}
				break;
			case ast::ExpressionKind::MemberAccess:
				{
					const auto &acc = raw_lexpr->as<ast::MemberAccessExpression>();
					require(expr, acc.member.kind == ast::SymbolKind::Field);
					const auto &member = acc.member.as<ast::FieldSymbol>();
					require(acc, member.randMode == ast::RandMode::None);
					int pad = acc.value().type->getBitstreamWidth() - acc.type->getBitstreamWidth() - member.bitOffset;
					raw_mask = {RTLIL::SigSpec(RTLIL::S0, pad), raw_mask, RTLIL::SigSpec(RTLIL::S0, member.bitOffset)};
					raw_rvalue = {RTLIL::SigSpec(RTLIL::Sx, pad), raw_rvalue, RTLIL::SigSpec(RTLIL::Sx, member.bitOffset)};
					raw_lexpr = &acc.value();
				}
				break;
			default:
				finished_etching = true;
				break;
			}
			if (raw_mask.size() != raw_lexpr->type->getBitstreamWidth())
				unimplemented(expr);
			log_assert(raw_mask.size() == raw_lexpr->type->getBitstreamWidth());
			log_assert(raw_rvalue.size() == raw_lexpr->type->getBitstreamWidth());
		}

		RTLIL::SigSpec lvalue = evaluate_lhs(mod, *raw_lexpr);
		crop_zero_mask(raw_mask, lvalue);
		crop_zero_mask(raw_mask, raw_rvalue);
		crop_zero_mask(raw_mask, raw_mask);

		RTLIL::SigSpec masked_rvalue;
		if (raw_mask.is_fully_ones()) {
			masked_rvalue = raw_rvalue;
		} else {
			RTLIL::SigSpec raw_lvalue_sampled = lvalue;
			raw_lvalue_sampled.replace(ctx.rvalue_subs);
			masked_rvalue = mod->Bwmux(NEW_ID, raw_lvalue_sampled, raw_rvalue, raw_mask);
		}

		log_assert(lvalue.size() == masked_rvalue.size());
		if (blocking) {
			for (int i = 0; i < lvalue.size(); i++)
				ctx.rvalue_subs[lvalue[i]] = masked_rvalue[i];
			// TODO: proper message on blocking/nonblocking mixing
			log_assert(!assigned_nonblocking.check_any(lvalue));
			assigned_blocking.add(lvalue);
		} else {
			 // TODO: proper message on blocking/nonblocking mixing
			log_assert(!assigned_blocking.check_any(lvalue));
			assigned_nonblocking.add(lvalue);
		}

		current_case->actions.push_back(RTLIL::SigSig(
			staging_signal(lvalue),
			masked_rvalue
		));
	}

	void handle(const ast::BlockStatement &blk) {
		require(blk, blk.blockKind == ast::StatementBlockKind::Sequential)
		blk.body.visit(*this);
	}

	void handle(const ast::StatementList &list) {
		for (auto &stmt : list.list)
			stmt->visit(*this);
	}

	void handle(const ast::ConditionalStatement &cond)
	{
		require(cond, cond.conditions.size() == 1);
		require(cond, cond.conditions[0].pattern == NULL);

		RTLIL::CaseRule *case_save = current_case;
		RTLIL::SigSpec condition = mod->ReduceBool(NEW_ID,
			evaluate_rhs(mod, *cond.conditions[0].expr, &ctx)
		);
		SwitchBuilder b(current_case, &ctx.rvalue_subs, condition);
		transfer_attrs(cond, b.sw);

		b.branch({RTLIL::S1}, [&](RTLIL::CaseRule *rule){
			current_case = rule;
			transfer_attrs(cond.ifTrue, rule);
			cond.ifTrue.visit(*this);
		});

		if (cond.ifFalse) {
			b.branch({}, [&](RTLIL::CaseRule *rule){
				current_case = rule;
				transfer_attrs(*cond.ifFalse, rule);
				cond.ifFalse->visit(*this);
			});	
		}
		b.finish(mod);

		current_case = case_save;
		// descend into an empty switch so we force action priority for follow up statements
		RTLIL::SwitchRule *dummy_switch = new RTLIL::SwitchRule;
		current_case->switches.push_back(dummy_switch);
		current_case = new RTLIL::CaseRule;
		dummy_switch->cases.push_back(current_case);
	}

	void handle(const ast::CaseStatement &stmt)
	{
		require(stmt, stmt.condition == ast::CaseStatementCondition::Normal);
		if (stmt.check != ast::UniquePriorityCheck::None) {
			auto src = format_src(stmt);
			log_warning("%s: Ignoring priority check\n", src.c_str());
		}

		RTLIL::CaseRule *case_save = current_case;
		RTLIL::SigSpec dispatch = evaluate_rhs(mod, stmt.expr, &ctx);
		SwitchBuilder b(current_case, &ctx.rvalue_subs, dispatch);
		transfer_attrs(stmt, b.sw);

		for (auto item : stmt.items) {
			std::vector<RTLIL::SigSpec> compares;
			for (auto expr : item.expressions) {
				log_assert(expr);
				RTLIL::SigSpec compare = evaluate_rhs(mod, *expr, &ctx);
				log_assert(compare.size() == dispatch.size());
				compares.push_back(compare);
			}
			require(stmt, !compares.empty());
			b.branch(compares, [&](RTLIL::CaseRule *rule) {
				current_case = rule;
				transfer_attrs(*item.stmt, rule);
				item.stmt->visit(*this);
			});
		}

		if (stmt.defaultCase) {
			b.branch(std::vector<RTLIL::SigSpec>{}, [&](RTLIL::CaseRule *rule) {
				current_case = rule;
				transfer_attrs(*stmt.defaultCase, rule);
				stmt.defaultCase->visit(*this);
			});
		}

		b.finish(mod);

		current_case = case_save;
		// descend into an empty switch so we force action priority for follow up statements
		RTLIL::SwitchRule *dummy_switch = new RTLIL::SwitchRule;
		current_case->switches.push_back(dummy_switch);
		current_case = new RTLIL::CaseRule;
		dummy_switch->cases.push_back(current_case);
	}

	void handle(YS_MAYBE_UNUSED const ast::InvalidStatement &stmt) { log_abort(); }
	void handle(YS_MAYBE_UNUSED const ast::EmptyStatement &stmt) {}
	void handle(YS_MAYBE_UNUSED const ast::VariableDeclStatement &stmt) {}

	void handle(const ast::Statement &stmt)
	{
		unimplemented(stmt);
	}
};

static RTLIL::SigSpec evaluate_function(RTLIL::Module *mod, const ast::CallExpression &call,
										ProcedureContext *ctx)
{
	const auto &subr = *std::get<0>(call.subroutine);
	log_assert(subr.subroutineKind == ast::SubroutineKind::Function);
	RTLIL::Process *proc = mod->addProcess(NEW_ID);
	ProceduralVisitor visitor(mod, proc, ProceduralVisitor::FUNCTION);
	log_assert(call.arguments().size() == subr.getArguments().size());
	for (int i = 0; i < call.arguments().size(); i++)
		visitor.ctx.args[subr.getArguments()[i]] = \
			evaluate_rhs(mod, *call.arguments()[i], ctx);
	subr.getBody().visit(visitor);

	// This is either a hack or brilliant: it just so happens that the WireAddingVisitor
	// has created a placeholder wire we can use here. That wire doesn't make sense as a
	// netlist element though.
	RTLIL::SigSpec ret = mod->wire(net_id(*subr.returnValVar));
	ret.replace(visitor.staging);
	return ret;
}

struct WireAddingVisitor : public ast::ASTVisitor<WireAddingVisitor, true, false> {
public:
	RTLIL::Module *mod;

	WireAddingVisitor(RTLIL::Module *mod)
		: mod(mod) {}

	// Do not descend into other modules
	void handle(YS_MAYBE_UNUSED const ast::InstanceSymbol &sym) { }

	void handle(const ast::ValueSymbol &sym)
	{
		require(sym, sym.getType().isFixedSize());
		auto w = mod->addWire(net_id(sym), sym.getType().getBitstreamWidth());
		transfer_attrs(sym, w);
	}
};

struct InitialProceduralVisitor : public ast::ASTVisitor<InitialProceduralVisitor, true, false> {
public:
	RTLIL::Module *mod;
	InitialProceduralVisitor(RTLIL::Module *mod)
		: mod(mod) {}

	void handle(const ast::Statement &stmt)
	{
		unimplemented(stmt);
	}
};

struct ModulePopulatingVisitor : public ast::ASTVisitor<ModulePopulatingVisitor, true, false> {
public:
	RTLIL::Module *mod;
	ModulePopulatingVisitor(RTLIL::Module *mod)
		: mod(mod) {}

	bool populate_sync(RTLIL::Process *proc, const ast::TimingControl &timing)
	{
		switch (timing.kind) {
		case ast::TimingControlKind::SignalEvent:
			{
				const auto &sigevent = timing.as<ast::SignalEventControl>();
				RTLIL::SyncRule *sync = new RTLIL::SyncRule();
				proc->syncs.push_back(sync);
				RTLIL::SigSpec sig = evaluate_rhs(mod, sigevent.expr, NULL);
				require(sigevent, sigevent.iffCondition == NULL);
				sync->signal = sig;
				switch (sigevent.edge) {
				case ast::EdgeKind::None:
					{
						auto src = format_src(timing);
						log_warning("%s: Turning non-edge sensitivity on %s to implicit sensitivity\n",
									src.c_str(), log_signal(sig));
						sync->type = RTLIL::SyncType::STa;
						sync->signal = {};
					}
					break;
				case ast::EdgeKind::PosEdge:
					require(sigevent, sig.size() == 1);
					sync->type = RTLIL::SyncType::STp;
					break;
				case ast::EdgeKind::NegEdge:
					require(sigevent, sig.size() == 1);
					sync->type = RTLIL::SyncType::STn;
					break;
				case ast::EdgeKind::BothEdges:
					require(sigevent, sig.size() == 1);
					sync->type = RTLIL::SyncType::STe;
					break;
				}
			}
			return true;
		case ast::TimingControlKind::ImplicitEvent:
			{
				RTLIL::SyncRule *sync = new RTLIL::SyncRule();
				proc->syncs.push_back(sync);
				sync->type = RTLIL::SyncType::STa;
			}
			return true;
		case ast::TimingControlKind::EventList:
			{
				const auto &evlist = timing.as<ast::EventListControl>();
				for (auto ev : evlist.events) {
					log_assert(ev);
					if (!populate_sync(proc, *ev))
						return false;
				}
			}
			return true;
		default:
			return false;
		}
	}

	void handle(const ast::ProceduralBlockSymbol &sym)
	{
		switch (sym.procedureKind) {
		case ast::ProceduralBlockKind::Always:
		case ast::ProceduralBlockKind::AlwaysFF:
			{
				RTLIL::Process *proc = mod->addProcess(NEW_ID);
				require(sym, sym.getBody().kind == ast::StatementKind::Timed);

				const auto &timed = sym.getBody().as<ast::TimedStatement>();
				if (!populate_sync(proc, timed.timing))
					unimplemented(timed)

				ProceduralVisitor visitor(mod, proc, ProceduralVisitor::ALWAYS);
				timed.stmt.visit(visitor);
				visitor.staging_done();
			}
			break;

		case ast::ProceduralBlockKind::AlwaysComb:
			{
				RTLIL::Process *proc = mod->addProcess(NEW_ID);
				RTLIL::SyncRule *sync = new RTLIL::SyncRule;
				proc->syncs.push_back(sync);
				sync->type = RTLIL::SyncType::STa;

				ProceduralVisitor visitor(mod, proc, ProceduralVisitor::ALWAYS);
				sym.getBody().visit(visitor);
				visitor.staging_done();
			}
			break;

		case ast::ProceduralBlockKind::Initial:
			{
				InitialProceduralVisitor visitor(mod);
				sym.getBody().visit(visitor);
			}
			break;
		case ast::ProceduralBlockKind::Final:
			{
				/* no-op */
			}
			break;
		default:
			unimplemented(sym);
		}		
	}

	void handle(YS_MAYBE_UNUSED const ast::ParameterSymbol &sym) {}

	void handle(const ast::NetSymbol &sym)
	{
		if (sym.getInitializer())
			mod->connect(mod->wire(net_id(sym)), evaluate_rhs(mod, *sym.getInitializer(), NULL));
	}

	void handle(const ast::VariableSymbol &sym)
	{
		RTLIL::Wire *w = mod->wire(net_id(sym));
		log_assert(w);
		slang::ConstantValue defvalue;
		if (sym.getInitializer()) {
			auto init = sym.getInitializer();
			{ // TODO: get rid of
				ast::ASTContext ctx(global_compilation->getRoot(), ast::LookupLocation::max);
				ctx.tryEval(*init);
			}
			require(sym, init->constant);
			defvalue = *init->constant;
		} else {
			defvalue = sym.getType().getDefaultValue();
		}
		RTLIL::Const initval = const_const(defvalue);
		if (!initval.is_fully_undef())
			w->attributes[RTLIL::ID::init] = initval;
	}

	void handle(const ast::PortSymbol &sym)
	{
		RTLIL::Wire *wire = mod->wire(net_id(*sym.internalSymbol));
		log_assert(wire);
		switch (sym.direction) {
		case ast::ArgumentDirection::In:
			wire->port_input = true;
			break;
		case ast::ArgumentDirection::Out:
			wire->port_output = true;
			break;
		case ast::ArgumentDirection::InOut:
			wire->port_input = true;
			wire->port_output = true;
			break;
		case ast::ArgumentDirection::Ref: // TODO: look up what those are
			break;
		}
	}

	void handle(const ast::InstanceSymbol &sym)
	{
		require(sym, sym.isModule());
		std::string modName;
		sym.body.getHierarchicalPath(modName);
		RTLIL::Cell *cell = mod->addCell(id(sym.name), id(modName));
		for (auto *conn : sym.getPortConnections()) {
			if (!conn->getExpression())
				continue;
			auto &expr = *conn->getExpression();
			RTLIL::SigSpec signal;
			if (expr.kind == ast::ExpressionKind::Assignment) {
				auto &assign = expr.as<ast::AssignmentExpression>();
				require(expr, assign.right().kind == ast::ExpressionKind::EmptyArgument);
				signal = evaluate_lhs(mod, assign.left());
			} else {
				signal = evaluate_rhs(mod, expr, NULL);
			}
			cell->setPort(net_id(conn->port), signal);
		}
		transfer_attrs(sym, cell);
	}

	void handle(const ast::ContinuousAssignSymbol &sym)
	{
		const ast::AssignmentExpression &expr = sym.getAssignment().as<ast::AssignmentExpression>();
		mod->connect(evaluate_lhs(mod, expr.left()), evaluate_rhs(mod, expr.right(), NULL));		
	}

	void handle(const ast::GenerateBlockSymbol &sym)
	{
		if (sym.isUninstantiated)
			return;
		visitDefault(sym);
	}

	void handle(const ast::InstanceBodySymbol &sym)
	{
		visitDefault(sym);
	}

	void handle(YS_MAYBE_UNUSED const ast::Type &type) {}
	void handle(YS_MAYBE_UNUSED const ast::NetType &type) {}
	void handle(YS_MAYBE_UNUSED const ast::TransparentMemberSymbol &sym) {}
	void handle(YS_MAYBE_UNUSED const ast::SubroutineSymbol &sym) {}

	void handle(const ast::StatementBlockSymbol &sym)
	{
		visitDefault(sym);
	}

	void handle(const ast::Symbol &sym)
	{
		unimplemented(sym);
	}
};

struct RTLILGenVisitor : public ast::ASTVisitor<RTLILGenVisitor, true, false> {
public:
	ast::Compilation &compilation;
	RTLIL::Design *design;

	RTLILGenVisitor(ast::Compilation &compilation, RTLIL::Design *design)
		: compilation(compilation), design(design) {}

	void handle(const ast::InstanceSymbol &symbol)
	{
		std::string name{symbol.name};

		if (symbol.name.empty()) {
			// NetlistVisitor.h says we should ignore this
			return;
		}

		std::string hierName;
		symbol.body.getHierarchicalPath(hierName);
		RTLIL::Module *mod = design->addModule(id(hierName));
		transfer_attrs(symbol.body, mod);

		WireAddingVisitor wadder(mod);
		symbol.body.visit(wadder);

		ModulePopulatingVisitor modpop(mod);
		symbol.body.visit(modpop);

		mod->fixup_ports();
		mod->check();

		this->visitDefault(symbol);
	}
};

USING_YOSYS_NAMESPACE

struct SlangFrontend : Frontend {
	SlangFrontend() : Frontend("slang", "read SystemVerilog (slang)") {}

	void help() override
	{
		slang::driver::Driver driver;
		driver.addStandardArgs();
		std::optional<bool> dump_ast;
		driver.cmdLine.add("--dump-ast", dump_ast, "Dump the AST");
		log("%s\n", driver.cmdLine.getHelpText("Slang-based SystemVerilog frontend").c_str());
	}

	void execute(std::istream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
	{
		(void) f;
		(void) filename;
		log_header(design, "Executing SLANG frontend.\n");

		slang::driver::Driver driver;
		driver.addStandardArgs();
		std::optional<bool> dump_ast;
		driver.cmdLine.add("--dump-ast", dump_ast, "Dump the AST");
		{
			std::vector<char *> c_args;
			for (auto arg : args) {
				char *c = new char[arg.size() + 1];
				strcpy(c, arg.c_str());
				c_args.push_back(c);
			}
			if (!driver.parseCommandLine(c_args.size(), &c_args[0]))
				log_cmd_error("Bad command\n");
		}
		if (!driver.processOptions())
			log_cmd_error("Bad command\n");

		try {
			if (!driver.parseAllSources())
				log_error("Parsing failed\n");

			auto compilation = driver.createCompilation();

			if (!driver.reportCompilation(*compilation, /* quiet */ false))
				log_error("Compilation failed\n");

			if (dump_ast.has_value() && dump_ast.value()) {
				slang::JsonWriter writer;
				writer.setPrettyPrint(true);
				ast::ASTSerializer serializer(*compilation, writer);
				serializer.serialize(compilation->getRoot());
				std::cout << writer.view() << std::endl;
			}

			global_compilation = &(*compilation);
			global_sourcemgr = compilation->getSourceManager();
			RTLILGenVisitor visitor(*compilation, design);
			compilation->getRoot().visit(visitor);
		} catch (const std::exception& e) {
			log_error("Exception: %s\n", e.what());
		}
	}
} SlangFrontend;

PRIVATE_NAMESPACE_END