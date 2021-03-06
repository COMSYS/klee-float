//===-- Z3Builder.cpp ------------------------------------------*- C++ -*-====//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include "klee/Config/config.h"
#ifdef ENABLE_Z3
#include "Z3Builder.h"

#include "klee/Expr.h"
#include "klee/Solver.h"
#include "klee/util/Bits.h"
#include "ConstantDivision.h"
#include "klee/SolverStats.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"

#include <limits>

using namespace klee;

namespace {
llvm::cl::opt<bool> UseConstructHashZ3(
    "use-construct-hash-z3",
    llvm::cl::desc("Use hash-consing during Z3 query construction."),
    llvm::cl::init(true));
}

void custom_z3_error_handler(Z3_context ctx, Z3_error_code ec) {
  ::Z3_string errorMsg =
#ifdef HAVE_Z3_GET_ERROR_MSG_NEEDS_CONTEXT
      // Z3 > 4.4.1
      Z3_get_error_msg(ctx, ec);
#else
      // Z3 4.4.1
      Z3_get_error_msg(ec);
#endif
  // FIXME: This is kind of a hack. The value comes from the enum
  // Z3_CANCELED_MSG but this isn't currently exposed by Z3's C API
  if (strcmp(errorMsg, "canceled") == 0) {
    // Solver timeout is not a fatal error
    return;
  }
  llvm::errs() << "Error: Incorrect use of Z3. [" << ec << "] " << errorMsg
               << "\n";
  abort();
}

Z3ArrayExprHash::~Z3ArrayExprHash() {}

void Z3ArrayExprHash::clear() {
  _update_node_hash.clear();
  _array_hash.clear();
}

Z3Builder::Z3Builder(bool autoClearConstructCache)
    : autoClearConstructCache(autoClearConstructCache) {
  // FIXME: Should probably let the client pass in a Z3_config instead
  Z3_config cfg = Z3_mk_config();
  // It is very important that we ask Z3 to let us manage memory so that
  // we are able to cache expressions and sorts.
  ctx = Z3_mk_context_rc(cfg);
  // Make sure we handle any errors reported by Z3.
  Z3_set_error_handler(ctx, custom_z3_error_handler);
  // When emitting Z3 expressions make them SMT-LIBv2 compliant
  Z3_set_ast_print_mode(ctx, Z3_PRINT_SMTLIB2_COMPLIANT);
  Z3_del_config(cfg);
}

Z3Builder::~Z3Builder() {
  // Clear caches so exprs/sorts gets freed before the destroying context
  // they aren associated with.
  clearConstructCache();
  _arr_hash.clear();
  Z3_del_context(ctx);
}

Z3SortHandle Z3Builder::getBvSort(unsigned width) {
  // FIXME: cache these
  return Z3SortHandle(Z3_mk_bv_sort(ctx, width), ctx);
}

Z3SortHandle Z3Builder::getArraySort(Z3SortHandle domainSort,
                                     Z3SortHandle rangeSort) {
  // FIXME: cache these
  return Z3SortHandle(Z3_mk_array_sort(ctx, domainSort, rangeSort), ctx);
}

Z3ASTHandle Z3Builder::buildArray(const char *name, unsigned indexWidth,
                                  unsigned valueWidth) {
  Z3SortHandle domainSort = getBvSort(indexWidth);
  Z3SortHandle rangeSort = getBvSort(valueWidth);
  Z3SortHandle t = getArraySort(domainSort, rangeSort);
  Z3_symbol s = Z3_mk_string_symbol(ctx, const_cast<char *>(name));
  return Z3ASTHandle(Z3_mk_const(ctx, s, t), ctx);
}

Z3ASTHandle Z3Builder::getTrue() { return Z3ASTHandle(Z3_mk_true(ctx), ctx); }

Z3ASTHandle Z3Builder::getFalse() { return Z3ASTHandle(Z3_mk_false(ctx), ctx); }

Z3ASTHandle Z3Builder::bvOne(unsigned width) { return bvZExtConst(width, 1); }

Z3ASTHandle Z3Builder::bvZero(unsigned width) { return bvZExtConst(width, 0); }

Z3ASTHandle Z3Builder::bvMinusOne(unsigned width) {
  return bvSExtConst(width, (int64_t)-1);
}

Z3ASTHandle Z3Builder::bvConst32(unsigned width, uint32_t value) {
  Z3SortHandle t = getBvSort(width);
  return Z3ASTHandle(Z3_mk_unsigned_int(ctx, value, t), ctx);
}

Z3ASTHandle Z3Builder::bvConst64(unsigned width, uint64_t value) {
  Z3SortHandle t = getBvSort(width);
  return Z3ASTHandle(Z3_mk_unsigned_int64(ctx, value, t), ctx);
}

Z3ASTHandle Z3Builder::bvZExtConst(unsigned width, uint64_t value) {
  if (width <= 64)
    return bvConst64(width, value);

  Z3ASTHandle expr = Z3ASTHandle(bvConst64(64, value), ctx);
  Z3ASTHandle zero = Z3ASTHandle(bvConst64(64, 0), ctx);
  for (width -= 64; width > 64; width -= 64)
    expr = concatExpr(zero, expr);
  return concatExpr(bvConst64(width, 0), expr);
}

Z3ASTHandle Z3Builder::bvSExtConst(unsigned width, uint64_t value) {
  if (width <= 64)
    return bvConst64(width, value);

  Z3SortHandle t = getBvSort(width - 64);
  if (value >> 63) {
    Z3ASTHandle r = Z3ASTHandle(Z3_mk_int64(ctx, -1, t), ctx);
    return concatExpr(r, bvConst64(64, value));
  }

  Z3ASTHandle r = Z3ASTHandle(Z3_mk_int64(ctx, 0, t), ctx);
  return concatExpr(r, bvConst64(64, value));
}

Z3ASTHandle Z3Builder::bvBoolExtract(Z3ASTHandle expr, int bit) {
  return Z3ASTHandle(Z3_mk_eq(ctx, bvExtract(expr, bit, bit), bvOne(1)), ctx);
}

Z3ASTHandle Z3Builder::bvExtract(Z3ASTHandle expr, unsigned top,
                                 unsigned bottom) {
  return Z3ASTHandle(Z3_mk_extract(ctx, top, bottom, expr), ctx);
}

Z3ASTHandle Z3Builder::eqExpr(Z3ASTHandle a, Z3ASTHandle b) {
  return Z3ASTHandle(Z3_mk_eq(ctx, a, b), ctx);
}

// logical right shift
Z3ASTHandle Z3Builder::bvRightShift(Z3ASTHandle expr, unsigned shift) {
  unsigned width = getBVLength(expr);

  if (shift == 0) {
    return expr;
  } else if (shift >= width) {
    return bvZero(width); // Overshift to zero
  } else {
    return concatExpr(bvZero(shift), bvExtract(expr, width - 1, shift));
  }
}

// logical left shift
Z3ASTHandle Z3Builder::bvLeftShift(Z3ASTHandle expr, unsigned shift) {
  unsigned width = getBVLength(expr);

  if (shift == 0) {
    return expr;
  } else if (shift >= width) {
    return bvZero(width); // Overshift to zero
  } else {
    return concatExpr(bvExtract(expr, width - shift - 1, 0), bvZero(shift));
  }
}

// left shift by a variable amount on an expression of the specified width
Z3ASTHandle Z3Builder::bvVarLeftShift(Z3ASTHandle expr, Z3ASTHandle shift) {
  unsigned width = getBVLength(expr);
  Z3ASTHandle res = bvZero(width);

  // construct a big if-then-elif-elif-... with one case per possible shift
  // amount
  for (int i = width - 1; i >= 0; i--) {
    res =
        iteExpr(eqExpr(shift, bvConst32(width, i)), bvLeftShift(expr, i), res);
  }

  // If overshifting, shift to zero
  Z3ASTHandle ex = bvLtExpr(shift, bvConst32(getBVLength(shift), width));
  res = iteExpr(ex, res, bvZero(width));
  return res;
}

// logical right shift by a variable amount on an expression of the specified
// width
Z3ASTHandle Z3Builder::bvVarRightShift(Z3ASTHandle expr, Z3ASTHandle shift) {
  unsigned width = getBVLength(expr);
  Z3ASTHandle res = bvZero(width);

  // construct a big if-then-elif-elif-... with one case per possible shift
  // amount
  for (int i = width - 1; i >= 0; i--) {
    res =
        iteExpr(eqExpr(shift, bvConst32(width, i)), bvRightShift(expr, i), res);
  }

  // If overshifting, shift to zero
  Z3ASTHandle ex = bvLtExpr(shift, bvConst32(getBVLength(shift), width));
  res = iteExpr(ex, res, bvZero(width));
  return res;
}

// arithmetic right shift by a variable amount on an expression of the specified
// width
Z3ASTHandle Z3Builder::bvVarArithRightShift(Z3ASTHandle expr,
                                            Z3ASTHandle shift) {
  unsigned width = getBVLength(expr);

  // get the sign bit to fill with
  Z3ASTHandle signedBool = bvBoolExtract(expr, width - 1);

  // start with the result if shifting by width-1
  Z3ASTHandle res = constructAShrByConstant(expr, width - 1, signedBool);

  // construct a big if-then-elif-elif-... with one case per possible shift
  // amount
  // XXX more efficient to move the ite on the sign outside all exprs?
  // XXX more efficient to sign extend, right shift, then extract lower bits?
  for (int i = width - 2; i >= 0; i--) {
    res = iteExpr(eqExpr(shift, bvConst32(width, i)),
                  constructAShrByConstant(expr, i, signedBool), res);
  }

  // If overshifting, shift to zero
  Z3ASTHandle ex = bvLtExpr(shift, bvConst32(getBVLength(shift), width));
  res = iteExpr(ex, res, bvZero(width));
  return res;
}

Z3ASTHandle Z3Builder::notExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_not(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::bvNotExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_bvnot(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::andExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  ::Z3_ast args[2] = {lhs, rhs};
  return Z3ASTHandle(Z3_mk_and(ctx, 2, args), ctx);
}

Z3ASTHandle Z3Builder::bvAndExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvand(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::orExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  ::Z3_ast args[2] = {lhs, rhs};
  return Z3ASTHandle(Z3_mk_or(ctx, 2, args), ctx);
}

Z3ASTHandle Z3Builder::orExpr(Z3ASTHandle first, Z3ASTHandle second, Z3ASTHandle third) {
  ::Z3_ast args[3] = {first, second, third};
  return Z3ASTHandle(Z3_mk_or(ctx, 3, args), ctx);
}

Z3ASTHandle Z3Builder::bvOrExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvor(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::iffExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  Z3SortHandle lhsSort = Z3SortHandle(Z3_get_sort(ctx, lhs), ctx);
  Z3SortHandle rhsSort = Z3SortHandle(Z3_get_sort(ctx, rhs), ctx);
  assert(Z3_get_sort_kind(ctx, lhsSort) == Z3_get_sort_kind(ctx, rhsSort) &&
         "lhs and rhs sorts must match");
  assert(Z3_get_sort_kind(ctx, lhsSort) == Z3_BOOL_SORT && "args must have BOOL sort");
  return Z3ASTHandle(Z3_mk_iff(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::bvXorExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvxor(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::bvRedorExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_bvredor(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::bvSignExtend(Z3ASTHandle src, unsigned width) {
  unsigned src_width =
      Z3_get_bv_sort_size(ctx, Z3SortHandle(Z3_get_sort(ctx, src), ctx));
  assert(src_width <= width && "attempted to extend longer data");

  return Z3ASTHandle(Z3_mk_sign_ext(ctx, width - src_width, src), ctx);
}

Z3ASTHandle Z3Builder::extractExpr(unsigned high, unsigned low, Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_extract(ctx, high, low, expr), ctx);
}

Z3ASTHandle Z3Builder::concatExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_concat(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::concatExpr(Z3ASTHandle first, Z3ASTHandle second, Z3ASTHandle third) {
  return Z3ASTHandle(Z3_mk_concat(ctx, Z3_mk_concat(ctx, first, second), third), ctx);
}

Z3ASTHandle Z3Builder::concatExpr(Z3ASTHandle first, Z3ASTHandle second, Z3ASTHandle third, Z3ASTHandle fourth) {
  return Z3ASTHandle(Z3_mk_concat(ctx, Z3_mk_concat(ctx, Z3_mk_concat(ctx, first, second), third), fourth), ctx);
}

Z3ASTHandle Z3Builder::isNanExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_fpa_is_nan(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::isInfinityExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_fpa_is_infinite(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::isFPZeroExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_fpa_is_zero(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::isSubnormalExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_fpa_is_subnormal(ctx, expr), ctx);
}

Z3ASTHandle Z3Builder::isFPNegativeExpr(Z3ASTHandle expr) {
  return Z3ASTHandle(Z3_mk_fpa_is_negative(ctx, expr), ctx);
}

Z3_ast Z3Builder::getRoundingModeAST(llvm::APFloat::roundingMode rm) {
  switch (rm) {
  default:
  case llvm::APFloat::rmNearestTiesToEven:
    return Z3_mk_fpa_round_nearest_ties_to_even(ctx);
  case llvm::APFloat::rmTowardPositive:
    return Z3_mk_fpa_round_toward_positive(ctx);
  case llvm::APFloat::rmTowardNegative:
    return Z3_mk_fpa_round_toward_negative(ctx);
  case llvm::APFloat::rmTowardZero:
    return Z3_mk_fpa_round_toward_zero(ctx);
  case llvm::APFloat::rmNearestTiesToAway:
    return Z3_mk_fpa_round_nearest_ties_to_away(ctx);
  }
}

Z3ASTHandle Z3Builder::fpNan(Z3SortHandle sort) {
  return Z3ASTHandle(Z3_mk_fpa_nan(ctx, sort), ctx);
}

Z3ASTHandle Z3Builder::fpZero(Z3SortHandle sort) {
  return Z3ASTHandle(Z3_mk_fpa_zero(ctx, sort, false), ctx);
}

Z3ASTHandle Z3Builder::writeExpr(Z3ASTHandle array, Z3ASTHandle index,
                                 Z3ASTHandle value) {
  return Z3ASTHandle(Z3_mk_store(ctx, array, index, value), ctx);
}

Z3ASTHandle Z3Builder::readExpr(Z3ASTHandle array, Z3ASTHandle index) {
  return Z3ASTHandle(Z3_mk_select(ctx, array, index), ctx);
}

Z3ASTHandle Z3Builder::iteExpr(Z3ASTHandle condition, Z3ASTHandle whenTrue,
                               Z3ASTHandle whenFalse) {
  return Z3ASTHandle(Z3_mk_ite(ctx, condition, whenTrue, whenFalse), ctx);
}

unsigned Z3Builder::getBVLength(Z3ASTHandle expr) {
  return Z3_get_bv_sort_size(ctx, Z3SortHandle(Z3_get_sort(ctx, expr), ctx));
}

Z3ASTHandle Z3Builder::bvLtExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvult(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::bvLeExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvule(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::sbvLtExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvslt(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::sbvLeExpr(Z3ASTHandle lhs, Z3ASTHandle rhs) {
  return Z3ASTHandle(Z3_mk_bvsle(ctx, lhs, rhs), ctx);
}

Z3ASTHandle Z3Builder::constructAShrByConstant(Z3ASTHandle expr, unsigned shift,
                                               Z3ASTHandle isSigned) {
  unsigned width = getBVLength(expr);

  if (shift == 0) {
    return expr;
  } else if (shift >= width) {
    return bvZero(width); // Overshift to zero
  } else {
    // FIXME: Is this really the best way to interact with Z3?
    return iteExpr(isSigned,
                   concatExpr(bvMinusOne(shift),
                              bvExtract(expr, width - 1, shift)),
                   bvRightShift(expr, shift));
  }
}

Z3ASTHandle Z3Builder::getInitialArray(const Array *root) {

  assert(root);
  Z3ASTHandle array_expr;
  bool hashed = _arr_hash.lookupArrayExpr(root, array_expr);

  if (!hashed) {
    // Unique arrays by name, so we make sure the name is unique by
    // using the size of the array hash as a counter.
    std::string unique_id = llvm::itostr(_arr_hash._array_hash.size());
    unsigned const uid_length = unique_id.length();
    unsigned const space = (root->name.length() > 32 - uid_length)
                               ? (32 - uid_length)
                               : root->name.length();
    std::string unique_name = root->name.substr(0, space) + unique_id;

    array_expr = buildArray(unique_name.c_str(), root->getDomain(),
                            root->getRange());

    if (root->isConstantArray()) {
      // FIXME: Flush the concrete values into Z3. Ideally we would do this
      // using assertions, which might be faster, but we need to fix the caching
      // to work correctly in that case.
      for (unsigned i = 0, e = root->size; i != e; ++i) {
        Z3ASTHandle prev = array_expr;
        array_expr = writeExpr(
            prev, construct(ConstantExpr::alloc(i, root->getDomain()), 0),
            construct(root->constantValues[i], 0));
      }
    }

    _arr_hash.hashArrayExpr(root, array_expr);
  }

  return (array_expr);
}

Z3ASTHandle Z3Builder::getInitialRead(const Array *root, unsigned index) {
  return readExpr(getInitialArray(root), bvConst32(32, index));
}

Z3ASTHandle Z3Builder::getArrayForUpdate(const Array *root,
                                         const UpdateNode *un) {
  if (!un) {
    return (getInitialArray(root));
  } else {
    // FIXME: This really needs to be non-recursive.
    Z3ASTHandle un_expr;
    bool hashed = _arr_hash.lookupUpdateNodeExpr(un, un_expr);

    if (!hashed) {
      un_expr = writeExpr(getArrayForUpdate(root, un->next),
                          construct(un->index, 0), construct(un->value, 0));

      _arr_hash.hashUpdateNodeExpr(un, un_expr);
    }

    return (un_expr);
  }
}

/** if *width_out!=1 then result is a bitvector,
    otherwise it is a bool */
Z3ASTHandle Z3Builder::construct(ref<Expr> e, int *width_out) {
  // TODO: We could potentially use Z3_simplify() here
  // to store simpler expressions.
  if (!UseConstructHashZ3 || isa<ConstantExpr>(e)) {
    return constructActual(e, width_out);
  } else {
    ExprHashMap<std::pair<Z3ASTHandle, unsigned> >::iterator it =
        constructed.find(e);
    if (it != constructed.end()) {
      if (width_out)
        *width_out = it->second.second;
      return it->second.first;
    } else {
      int width;
      if (!width_out)
        width_out = &width;
      Z3ASTHandle res = constructActual(e, width_out);
      constructed.insert(std::make_pair(e, std::make_pair(res, *width_out)));
      return res;
    }
  }
}

/** if *width_out!=1 then result is a bitvector,
    otherwise it is a bool */
Z3ASTHandle Z3Builder::constructActual(ref<Expr> e, int *width_out) {
  int width;
  if (!width_out)
    width_out = &width;

  ++stats::queryConstructs;

  switch (e->getKind()) {
  case Expr::Constant: {
    ConstantExpr *CE = cast<ConstantExpr>(e);
    *width_out = CE->getWidth();

    // Coerce to bool if necessary.
    if (*width_out == 1)
      return CE->isTrue() ? getTrue() : getFalse();

    // Fast path.
    if (*width_out <= 32)
      return bvConst32(*width_out, CE->getZExtValue(32));
    if (*width_out <= 64)
      return bvConst64(*width_out, CE->getZExtValue());

    ref<ConstantExpr> Tmp = CE;
    Z3ASTHandle Res = bvConst64(64, Tmp->Extract(0, 64)->getZExtValue());
    while (Tmp->getWidth() > 64) {
      Tmp = Tmp->Extract(64, Tmp->getWidth() - 64);
      unsigned Width = std::min(64U, Tmp->getWidth());
      Res = concatExpr(bvConst64(Width, Tmp->Extract(0, Width)->getZExtValue()),
                       Res);
    }

    return Res;
  }

  case Expr::FConstant: {
    FConstantExpr * CE = cast<FConstantExpr>(e);
    *width_out = CE->getWidth();

    switch (*width_out)
    {
    case Expr::Fl32:
      return Z3ASTHandle(Z3_mk_fpa_numeral_float(ctx, CE->getAPValue().convertToFloat(), Z3_mk_fpa_sort_32(ctx)), ctx);
    case Expr::Fl64:
      return Z3ASTHandle(Z3_mk_fpa_numeral_double(ctx, CE->getAPValue().convertToDouble(), Z3_mk_fpa_sort_64(ctx)), ctx);
    case Expr::Fl80: {
      uint8_t sign = CE->getAPValue().bitcastToAPInt().getRawData()[1] >> 15 & 0x1;
      uint16_t exp = CE->getAPValue().bitcastToAPInt().getRawData()[1] & 0x7FFF;
      uint64_t mnt = CE->getAPValue().bitcastToAPInt().getRawData()[0];
      bool correctHiddenBit = ((exp == 0) == (((mnt >> 63) & 0x1) == 0));

      mnt &= 0x7FFFFFFFFFFFFFFF;

      Z3ASTHandle conv = Z3ASTHandle(Z3_mk_fpa_fp(ctx,
                                                  Z3ASTHandle(Z3_mk_unsigned_int(ctx, sign, Z3_mk_bv_sort(ctx, 1)), ctx),
                                                  Z3ASTHandle(Z3_mk_unsigned_int(ctx, exp, Z3_mk_bv_sort(ctx, 15)), ctx),
                                                  Z3ASTHandle(Z3_mk_unsigned_int(ctx, mnt, Z3_mk_bv_sort(ctx, 63)), ctx)),
                                     ctx);

      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), conv);

      if (correctHiddenBit) {
        arr = writeExpr(arr, bvOne(1), fpZero(sort));
      }
      else
      {
        arr = writeExpr(arr, bvOne(1), fpNan(sort));
      }

      return arr;
    }
    }
  }

  // Special
  case Expr::NotOptimized: {
    NotOptimizedExpr *noe = cast<NotOptimizedExpr>(e);
    return construct(noe->src, width_out);
  }

  case Expr::Read: {
    ReadExpr *re = cast<ReadExpr>(e);
    assert(re && re->updates.root);
    *width_out = re->updates.root->getRange();
    return readExpr(getArrayForUpdate(re->updates.root, re->updates.head),
                    construct(re->index, 0));
  }

  case Expr::Select: {
    SelectExpr *se = cast<SelectExpr>(e);
    Z3ASTHandle cond = construct(se->cond, 0);
    Z3ASTHandle tExpr = construct(se->trueExpr, width_out);
    Z3ASTHandle fExpr = construct(se->falseExpr, width_out);
    return iteExpr(cond, tExpr, fExpr);
  }

  case Expr::FSelect: {
    FSelectExpr *se = cast<FSelectExpr>(e);
    Z3ASTHandle cond = construct(se->cond, 0);
    Z3ASTHandle tExpr = construct(se->trueExpr, width_out);
    Z3ASTHandle fExpr = construct(se->falseExpr, width_out);
    return iteExpr(cond, tExpr, fExpr);
  }

  case Expr::Concat: {
    ConcatExpr *ce = cast<ConcatExpr>(e);
    unsigned numKids = ce->getNumKids();
    Z3ASTHandle res = construct(ce->getKid(numKids - 1), 0);
    for (int i = numKids - 2; i >= 0; i--) {
      res = concatExpr(construct(ce->getKid(i), 0), res);
    }
    *width_out = ce->getWidth();
    return res;
  }

  case Expr::Extract: {
    ExtractExpr *ee = cast<ExtractExpr>(e);
    Z3ASTHandle src = construct(ee->expr, width_out);
    *width_out = ee->getWidth();
    if (*width_out == 1) {
      return bvBoolExtract(src, ee->offset);
    } else {
      return bvExtract(src, ee->offset + *width_out - 1, ee->offset);
    }
  }

  // Casting

  case Expr::ZExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    Z3ASTHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth == 1) {
      return iteExpr(src, bvOne(*width_out), bvZero(*width_out));
    } else {
      return concatExpr(bvZero(*width_out - srcWidth), src);
    }
  }

  case Expr::SExt: {
    int srcWidth;
    CastExpr *ce = cast<CastExpr>(e);
    Z3ASTHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();
    if (srcWidth == 1) {
      return iteExpr(src, bvMinusOne(*width_out), bvZero(*width_out));
    } else {
      return bvSignExtend(src, *width_out);
    }
  }

  case Expr::FExt: {
    FCastRoundExpr *ce = cast<FCastRoundExpr>(e);
    int srcWidth;
    Z3ASTHandle src = construct(ce->src, &srcWidth);

    *width_out = ce->getWidth();

    Z3SortHandle sort;
    switch (*width_out) {
    case 16:
      sort = Z3SortHandle(Z3_mk_fpa_sort_16(ctx), ctx);
      break;
    case Expr::Fl32:
      sort = Z3SortHandle(Z3_mk_fpa_sort_32(ctx), ctx);
      break;
    case Expr::Fl64:
      sort = Z3SortHandle(Z3_mk_fpa_sort_64(ctx), ctx);
      break;
    case Expr::Fl80: {
      sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);

      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), Z3ASTHandle(Z3_mk_fpa_to_fp_float(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx));
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    case 128:
      sort = Z3SortHandle(Z3_mk_fpa_sort_128(ctx), ctx);
      break;
    }

    // casting unnormal f80s results in NaN
    if (srcWidth == Expr::Fl80)
    {
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(src, bvOne(1)));
      src = readExpr(src, bvZero(1));
      return iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_to_fp_float(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx));
    }
    else
    {
      return Z3ASTHandle(Z3_mk_fpa_to_fp_float(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx);
    }
  }

  case Expr::FToU: {
    int srcWidth;
    CastRoundExpr *ce = cast<CastRoundExpr>(e);
    Z3ASTHandle src = construct(ce->src, &srcWidth);

    *width_out = ce->getWidth();

    // casting unnormal f80s results in 0
    if (srcWidth == Expr::Fl80)
    {
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(src, bvOne(1)));
      src = readExpr(src, bvZero(1));
      return iteExpr(wrongHiddenBit, 
                     bvZero(*width_out), 
                     Z3ASTHandle(Z3_mk_fpa_to_ubv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, *width_out), ctx));
    }
    else
    {
      return Z3ASTHandle(Z3_mk_fpa_to_ubv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, *width_out), ctx);
    }
  }

  case Expr::FToS: {
    int srcWidth;
    CastRoundExpr *ce = cast<CastRoundExpr>(e);
    Z3ASTHandle src = construct(ce->src, &srcWidth);

    *width_out = ce->getWidth();

    // casting unnormal f80s results in 0 for char and short, in the least value for int and long long 
    if (srcWidth == Expr::Fl80)
    {
      Z3ASTHandle num = readExpr(src, bvZero(1));
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(src, bvOne(1)));
      if (*width_out == Expr::Int32)
      {
        return iteExpr(wrongHiddenBit,
                       bvSExtConst(Expr::Int32, std::numeric_limits<int32_t>::min()),
                       Z3ASTHandle(Z3_mk_fpa_to_sbv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, Expr::Int32), ctx));
      }
      else if (*width_out == Expr::Int64)
      {
        return iteExpr(wrongHiddenBit,
                       bvSExtConst(Expr::Int64, std::numeric_limits<int64_t>::min()),
                       Z3ASTHandle(Z3_mk_fpa_to_sbv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, Expr::Int64), ctx));
      }
      else
      {
        return iteExpr(wrongHiddenBit,
                       bvZero(*width_out), 
                       Z3ASTHandle(Z3_mk_fpa_to_sbv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, *width_out), ctx));
      }
    }
    else
    {
      return Z3ASTHandle(Z3_mk_fpa_to_sbv(ctx, getRoundingModeAST(ce->getRoundingMode()), src, *width_out), ctx);
    }
  }

  case Expr::UToF:{
    FCastRoundExpr *ce = cast<FCastRoundExpr>(e);
    int srcWidth;
    Z3ASTHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();

    Z3SortHandle sort;
    switch (*width_out) {
    case 16:
      sort = Z3SortHandle(Z3_mk_fpa_sort_16(ctx), ctx);
      break;
    case Expr::Fl32:
      sort = Z3SortHandle(Z3_mk_fpa_sort_32(ctx), ctx);
      break;
    case Expr::Fl64:
      sort = Z3SortHandle(Z3_mk_fpa_sort_64(ctx), ctx);
      break;
    case Expr::Fl80: {
      sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);

      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), Z3ASTHandle(Z3_mk_fpa_to_fp_unsigned(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx));
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    case 128:
      sort = Z3SortHandle(Z3_mk_fpa_sort_128(ctx), ctx);
      break;
    }

    return Z3ASTHandle(Z3_mk_fpa_to_fp_unsigned(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx);
  }

  case Expr::SToF: {
    FCastRoundExpr *ce = cast<FCastRoundExpr>(e);
    int srcWidth;
    Z3ASTHandle src = construct(ce->src, &srcWidth);
    *width_out = ce->getWidth();

    Z3SortHandle sort;
    switch (*width_out) {
    case 16:
      sort = Z3SortHandle(Z3_mk_fpa_sort_16(ctx), ctx);
      break;
    case Expr::Fl32:
      sort = Z3SortHandle(Z3_mk_fpa_sort_32(ctx), ctx);
      break;
    case Expr::Fl64:
      sort = Z3SortHandle(Z3_mk_fpa_sort_64(ctx), ctx);
      break;
    case Expr::Fl80: {
      sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);

      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), Z3ASTHandle(Z3_mk_fpa_to_fp_unsigned(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx));
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    case 128:
      sort = Z3SortHandle(Z3_mk_fpa_sort_128(ctx), ctx);
      break;
    }

    return Z3ASTHandle(Z3_mk_fpa_to_fp_signed(ctx, getRoundingModeAST(ce->getRoundingMode()), src, sort), ctx);
  }

  case Expr::ExplicitFloat: {
    ExplicitFloatExpr *ce = cast<ExplicitFloatExpr>(e);
    Z3ASTHandle src = construct(ce->src, width_out);

    Z3SortHandle sort;

    switch (*width_out) {
    case 16:
      sort = Z3SortHandle(Z3_mk_fpa_sort_16(ctx), ctx);
      break;
    case Expr::Fl32:
      sort = Z3SortHandle(Z3_mk_fpa_sort_32(ctx), ctx);
      break;
    case Expr::Fl64:
      sort = Z3SortHandle(Z3_mk_fpa_sort_64(ctx), ctx);
      break;
    case Expr::Fl80: {
      // turn the 80-bit bitvector into a 79-bit one, discarding the 63rd bit

      // the second parameter is the number of bits in the exponent, the third is the number of bits in the mantissa, *including* the hidden bit
      sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);

      Z3ASTHandle sign = extractExpr(79, 79, src);
      Z3ASTHandle exp = extractExpr(78, 64, src);
      Z3ASTHandle hiddenBit = extractExpr(63, 63, src);
      Z3ASTHandle mnt = extractExpr(62, 0, src);

      Z3ASTHandle correctHiddenBit = eqExpr(hiddenBit, iteExpr(eqExpr(bvRedorExpr(exp), bvZero(1)), bvZero(1), bvOne(1)));

      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), Z3ASTHandle(Z3_mk_fpa_to_fp_bv(ctx, concatExpr(sign, exp, mnt), sort), ctx));
      arr = writeExpr(arr, bvOne(1), iteExpr(correctHiddenBit, fpZero(sort), fpNan(sort)));

      return arr;
    }
    case 128:
      sort = Z3SortHandle(Z3_mk_fpa_sort_128(ctx), ctx);
      break;
    }

    return Z3ASTHandle(Z3_mk_fpa_to_fp_bv(ctx, src, sort), ctx);
  }

  case Expr::ExplicitInt: {
    ExplicitIntExpr *ce = cast<ExplicitIntExpr>(e);
    Z3ASTHandle src = construct(ce->src, width_out);

    Z3ASTHandle ret = Z3ASTHandle(Z3_mk_fpa_to_ieee_bv(ctx, src), ctx);

    if (*width_out == Expr::Fl80)
    {
      Z3ASTHandle sign = extractExpr(78, 78, ret);
      Z3ASTHandle exp = extractExpr(77, 63, ret);
      Z3ASTHandle mnt = extractExpr(62, 0, ret);

      // if the exponent is all zeros, bit 63 has to be 0, else it has to be 1
      Z3ASTHandle ite = iteExpr(eqExpr(bvRedorExpr(exp), bvZero(1)), bvZero(1), bvOne(1));

      ret = concatExpr(sign, exp, ite, mnt);
    }

    return ret;
  }

  // Floating-point special functions
  case Expr::FAbs: {
    FAbsExpr *fe = cast<FAbsExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FAbs");

    // fabs doesn't care about unnormal f80s - probably just sets the sign bit without reading the rest
    if (*width_out == Expr::Fl80)
    {
      expr = writeExpr(expr, bvZero(1), Z3ASTHandle(Z3_mk_fpa_abs(ctx, readExpr(expr, bvZero(1))), ctx));
      return expr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_abs(ctx, expr), ctx);
      return result;
    }
  }

  case Expr::FpClassify: {
    FpClassifyExpr *fe = cast<FpClassifyExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FpClassify");
    // classification functions don't care about unnormal f80s (in Clang 3.4)
    if (*width_out == Expr::Fl80)
    {
      expr = readExpr(expr, bvZero(1));
    }

    *width_out = sizeof(int) * 8;

    // this is the same if-then-else chain as in ConstantExpr::FpClassify()
    Z3ASTHandle result = iteExpr(isNanExpr(expr), 
                           bvSExtConst(*width_out, FP_NAN)
                         ,//else
                           iteExpr(isInfinityExpr(expr), 
                             bvSExtConst(*width_out, FP_INFINITE)
                           ,//else
                             iteExpr(isFPZeroExpr(expr),
                               bvSExtConst(*width_out, FP_ZERO)
                             ,//else
                               iteExpr(isSubnormalExpr(expr),
                                 bvSExtConst(*width_out, FP_SUBNORMAL)
                               ,//else
                                 bvSExtConst(*width_out, FP_NORMAL)
                               )
                             )
                           )
                         );
    return result;
  }

  case Expr::FIsFinite: {
    FIsFiniteExpr *fe = cast<FIsFiniteExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FIsFinite");
    // classification functions don't care about unnormal f80s (in Clang 3.4)
    if (*width_out == Expr::Fl80)
    {
      expr = readExpr(expr, bvZero(1));
    }

    *width_out = sizeof(int) * 8;

    Z3ASTHandle result = iteExpr(orExpr(isNanExpr(expr), isInfinityExpr(expr)),
                           bvZero(*width_out)
                         ,//else
                           bvOne(*width_out)
                         );
    return result;
  }

  case Expr::FIsNan: {
    FIsNanExpr *fe = cast<FIsNanExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FIsNan");
    // classification functions don't care about unnormal f80s (in Clang 3.4)
    if (*width_out == Expr::Fl80)
    {
      expr = readExpr(expr, bvZero(1));
    }

    *width_out = sizeof(int) * 8;

    Z3ASTHandle result = iteExpr(isNanExpr(expr),
                           bvOne(*width_out)
                         ,//else
                           bvZero(*width_out)
                         );
    return result;
  }

  case Expr::FIsInf: {
    FIsInfExpr *fe = cast<FIsInfExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FIsInf");
    // isinf does care about unnormal f80s for some reason

    if (*width_out == Expr::Fl80)
    {
      *width_out = sizeof(int) * 8;

      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(expr, bvOne(1)));
      expr = readExpr(expr, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, bvZero(*width_out), iteExpr(isInfinityExpr(expr), iteExpr(isFPNegativeExpr(expr), bvMinusOne(*width_out), bvOne(*width_out)), bvZero(*width_out)));
      return result;
    }
    else
    {
      *width_out = sizeof(int) * 8;

      Z3ASTHandle result = iteExpr(isInfinityExpr(expr),
                             iteExpr(isFPNegativeExpr(expr),
                               bvMinusOne(*width_out)
                             ,//else
                               bvOne(*width_out)
                             )
                           ,//else
                             bvZero(*width_out)
                           );
      return result;
    }
  }

  case Expr::FSqrt: {
    FSqrtExpr *fe = cast<FSqrtExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FSqrt");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(expr, bvOne(1)));
      expr = readExpr(expr, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_sqrt(ctx, getRoundingModeAST(fe->getRoundingMode()), expr), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_sqrt(ctx, getRoundingModeAST(fe->getRoundingMode()), expr), ctx);
      return result;
    }
  }

  case Expr::FNearbyInt: {
    FNearbyIntExpr *fe = cast<FNearbyIntExpr>(e);
    Z3ASTHandle expr = construct(fe->expr, width_out);

    assert((*width_out == Expr::Int32 || *width_out == Expr::Int64 || *width_out == Expr::Fl80) && "non-float argument to FNearbyInt");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = isNanExpr(readExpr(expr, bvOne(1)));
      expr = readExpr(expr, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_round_to_integral(ctx, getRoundingModeAST(fe->getRoundingMode()), expr), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_round_to_integral(ctx, getRoundingModeAST(fe->getRoundingMode()), expr), ctx);
      return result;
    }
  }

  // Arithmetic
  case Expr::Add: {
    AddExpr *ae = cast<AddExpr>(e);
    Z3ASTHandle left = construct(ae->left, width_out);
    Z3ASTHandle right = construct(ae->right, width_out);
    assert(*width_out != 1 && "uncanonicalized add");
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvadd(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::Sub: {
    SubExpr *se = cast<SubExpr>(e);
    Z3ASTHandle left = construct(se->left, width_out);
    Z3ASTHandle right = construct(se->right, width_out);
    assert(*width_out != 1 && "uncanonicalized sub");
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvsub(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::Mul: {
    MulExpr *me = cast<MulExpr>(e);
    Z3ASTHandle right = construct(me->right, width_out);
    assert(*width_out != 1 && "uncanonicalized mul");
    Z3ASTHandle left = construct(me->left, width_out);
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvmul(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::UDiv: {
    UDivExpr *de = cast<UDivExpr>(e);
    Z3ASTHandle left = construct(de->left, width_out);
    assert(*width_out != 1 && "uncanonicalized udiv");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(de->right)) {
      if (CE->getWidth() <= 64) {
        uint64_t divisor = CE->getZExtValue();
        if (bits64::isPowerOfTwo(divisor))
          return bvRightShift(left, bits64::indexOfSingleBit(divisor));
      }
    }

    Z3ASTHandle right = construct(de->right, width_out);
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvudiv(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::SDiv: {
    SDivExpr *de = cast<SDivExpr>(e);
    Z3ASTHandle left = construct(de->left, width_out);
    assert(*width_out != 1 && "uncanonicalized sdiv");
    Z3ASTHandle right = construct(de->right, width_out);
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvsdiv(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::URem: {
    URemExpr *de = cast<URemExpr>(e);
    Z3ASTHandle left = construct(de->left, width_out);
    assert(*width_out != 1 && "uncanonicalized urem");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(de->right)) {
      if (CE->getWidth() <= 64) {
        uint64_t divisor = CE->getZExtValue();

        if (bits64::isPowerOfTwo(divisor)) {
          unsigned bits = bits64::indexOfSingleBit(divisor);

          // special case for modding by 1 or else we bvExtract -1:0
          if (bits == 0) {
            return bvZero(*width_out);
          } else {
            return concatExpr(bvZero(*width_out - bits),
                              bvExtract(left, bits - 1, 0));
          }
        }
      }
    }

    Z3ASTHandle right = construct(de->right, width_out);
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvurem(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  case Expr::SRem: {
    SRemExpr *de = cast<SRemExpr>(e);
    Z3ASTHandle left = construct(de->left, width_out);
    Z3ASTHandle right = construct(de->right, width_out);
    assert(*width_out != 1 && "uncanonicalized srem");
    // LLVM's srem instruction says that the sign follows the dividend
    // (``left``).
    // Z3's C API says ``Z3_mk_bvsrem()`` does this so these seem to match.
    Z3ASTHandle result = Z3ASTHandle(Z3_mk_bvsrem(ctx, left, right), ctx);
    assert(getBVLength(result) == static_cast<unsigned>(*width_out) &&
           "width mismatch");
    return result;
  }

  // Bitwise
  case Expr::Not: {
    NotExpr *ne = cast<NotExpr>(e);
    Z3ASTHandle expr = construct(ne->expr, width_out);
    if (*width_out == 1) {
      return notExpr(expr);
    } else {
      return bvNotExpr(expr);
    }
  }

  case Expr::And: {
    AndExpr *ae = cast<AndExpr>(e);
    Z3ASTHandle left = construct(ae->left, width_out);
    Z3ASTHandle right = construct(ae->right, width_out);
    if (*width_out == 1) {
      return andExpr(left, right);
    } else {
      return bvAndExpr(left, right);
    }
  }

  case Expr::Or: {
    OrExpr *oe = cast<OrExpr>(e);
    Z3ASTHandle left = construct(oe->left, width_out);
    Z3ASTHandle right = construct(oe->right, width_out);
    if (*width_out == 1) {
      return orExpr(left, right);
    } else {
      return bvOrExpr(left, right);
    }
  }

  case Expr::Xor: {
    XorExpr *xe = cast<XorExpr>(e);
    Z3ASTHandle left = construct(xe->left, width_out);
    Z3ASTHandle right = construct(xe->right, width_out);

    if (*width_out == 1) {
      // XXX check for most efficient?
      return iteExpr(left, Z3ASTHandle(notExpr(right)), right);
    } else {
      return bvXorExpr(left, right);
    }
  }

  case Expr::Shl: {
    ShlExpr *se = cast<ShlExpr>(e);
    Z3ASTHandle left = construct(se->left, width_out);
    assert(*width_out != 1 && "uncanonicalized shl");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(se->right)) {
      return bvLeftShift(left, (unsigned)CE->getLimitedValue());
    } else {
      int shiftWidth;
      Z3ASTHandle amount = construct(se->right, &shiftWidth);
      return bvVarLeftShift(left, amount);
    }
  }

  case Expr::LShr: {
    LShrExpr *lse = cast<LShrExpr>(e);
    Z3ASTHandle left = construct(lse->left, width_out);
    assert(*width_out != 1 && "uncanonicalized lshr");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(lse->right)) {
      return bvRightShift(left, (unsigned)CE->getLimitedValue());
    } else {
      int shiftWidth;
      Z3ASTHandle amount = construct(lse->right, &shiftWidth);
      return bvVarRightShift(left, amount);
    }
  }

  case Expr::AShr: {
    AShrExpr *ase = cast<AShrExpr>(e);
    Z3ASTHandle left = construct(ase->left, width_out);
    assert(*width_out != 1 && "uncanonicalized ashr");

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ase->right)) {
      unsigned shift = (unsigned)CE->getLimitedValue();
      Z3ASTHandle signedBool = bvBoolExtract(left, *width_out - 1);
      return constructAShrByConstant(left, shift, signedBool);
    } else {
      int shiftWidth;
      Z3ASTHandle amount = construct(ase->right, &shiftWidth);
      return bvVarArithRightShift(left, amount);
    }
  }

  // Floating-point

  case Expr::FAdd: {
    FAddExpr *fe = cast<FAddExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FAdd");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_add(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_add(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx);
      return result;
    }
  }

  case Expr::FSub: {
    FSubExpr *fe = cast<FSubExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FSub");
    
    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_sub(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_sub(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx);
      return result;
    }
  }

  case Expr::FMul: {
    FMulExpr *fe = cast<FMulExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FMul");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_mul(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_mul(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx);
      return result;
    }
  }

  case Expr::FDiv: {
    FDivExpr *fe = cast<FDivExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FDiv");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_div(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_div(ctx, getRoundingModeAST(fe->getRoundingMode()), left, right), ctx);
      return result;
    }
  }

  case Expr::FRem: {
    FRemExpr *fe = cast<FRemExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FRem");

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBit, fpNan(sort), Z3ASTHandle(Z3_mk_fpa_rem(ctx, left, right), ctx));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_rem(ctx, left, right), ctx); // Z3's frem doesn't ask for rounding mode
      return result;
    }
  }

  case Expr::FMin: {
    FMinExpr *fe = cast<FMinExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FMin");

    // fmin is weird with unnormal f80s - if one operand is unnormal it returns the other operand, if both are unnormal it returns the left one
    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBitLeft = isNanExpr(readExpr(left, bvOne(1)));
      Z3ASTHandle wrongHiddenBitRight = isNanExpr(readExpr(right, bvOne(1)));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBitLeft, iteExpr(wrongHiddenBitRight, left, right), iteExpr(wrongHiddenBitRight, left, Z3ASTHandle(Z3_mk_fpa_min(ctx, left, right), ctx)));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_min(ctx, left, right), ctx);
      return result;
    }
  }

  case Expr::FMax: {
    FMaxExpr *fe = cast<FMaxExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FMax");

    // fmax is weird with unnormal f80s - if one operand is unnormal it returns the other operand, if both are unnormal it returns the left one
    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBitLeft = isNanExpr(readExpr(left, bvOne(1)));
      Z3ASTHandle wrongHiddenBitRight = isNanExpr(readExpr(right, bvOne(1)));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      Z3ASTHandle result = iteExpr(wrongHiddenBitLeft, iteExpr(wrongHiddenBitRight, left, right), iteExpr(wrongHiddenBitRight, left, Z3ASTHandle(Z3_mk_fpa_max(ctx, left, right), ctx)));
      Z3ASTHandle arr = Z3ASTHandle(Z3_mk_const(ctx, Z3_mk_string_symbol(ctx, "[F80, unnormal]"), getArraySort(getBvSort(1), sort)), ctx);
      arr = writeExpr(arr, bvZero(1), result);
      arr = writeExpr(arr, bvOne(1), fpZero(sort));
      return arr;
    }
    else
    {
      Z3ASTHandle result = Z3ASTHandle(Z3_mk_fpa_max(ctx, left, right), ctx);
      return result;
    }
  }

  // Comparison

  case Expr::Eq: {
    EqExpr *ee = cast<EqExpr>(e);
    Z3ASTHandle left = construct(ee->left, width_out);
    Z3ASTHandle right = construct(ee->right, width_out);
    if (*width_out == 1) {
      if (ConstantExpr *CE = dyn_cast<ConstantExpr>(ee->left)) {
        if (CE->isTrue())
          return right;
        return notExpr(right);
      } else {
        return iffExpr(left, right);
      }
    } else {
      *width_out = 1;
      return eqExpr(left, right);
    }
  }

  case Expr::Ult: {
    UltExpr *ue = cast<UltExpr>(e);
    Z3ASTHandle left = construct(ue->left, width_out);
    Z3ASTHandle right = construct(ue->right, width_out);
    assert(*width_out != 1 && "uncanonicalized ult");
    *width_out = 1;
    return bvLtExpr(left, right);
  }

  case Expr::Ule: {
    UleExpr *ue = cast<UleExpr>(e);
    Z3ASTHandle left = construct(ue->left, width_out);
    Z3ASTHandle right = construct(ue->right, width_out);
    assert(*width_out != 1 && "uncanonicalized ule");
    *width_out = 1;
    return bvLeExpr(left, right);
  }

  case Expr::Slt: {
    SltExpr *se = cast<SltExpr>(e);
    Z3ASTHandle left = construct(se->left, width_out);
    Z3ASTHandle right = construct(se->right, width_out);
    assert(*width_out != 1 && "uncanonicalized slt");
    *width_out = 1;
    return sbvLtExpr(left, right);
  }

  case Expr::Sle: {
    SleExpr *se = cast<SleExpr>(e);
    Z3ASTHandle left = construct(se->left, width_out);
    Z3ASTHandle right = construct(se->right, width_out);
    assert(*width_out != 1 && "uncanonicalized sle");
    *width_out = 1;
    return sbvLeExpr(left, right);
  }

  // Floating-point comparison

  case Expr::FOrd: {
    FOrdExpr *fe = cast<FOrdExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOrd");

    // don't care about unnormal f80s, just act like isnan
    if (*width_out == Expr::Fl80)
    {
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
    }

    *width_out = 1;
    Z3ASTHandle result = andExpr(notExpr(isNanExpr(left)), notExpr(isNanExpr(right)));
    return result;
  }

  case Expr::FUno: {
    FUnoExpr *fe = cast<FUnoExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUno");

    // don't care about unnormal f80s, just act like isnan
    if (*width_out == Expr::Fl80)
    {
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
    }

    *width_out = 1;
    Z3ASTHandle result = orExpr(isNanExpr(left), isNanExpr(right));
    return result;
  }

  case Expr::FUeq: {
    FUeqExpr *fe = cast<FUeqExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUeq");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx)));
    }
    else
    {
      result = orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOeq: {
    FOeqExpr *fe = cast<FOeqExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOeq");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx));
    }
    else
    {
      result = Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx);
    }

    *width_out = 1;
    return result;
  }

  case Expr::FUgt: {
    FUgtExpr *fe = cast<FUgtExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUgt");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_gt(ctx, left, right), ctx)));
    }
    else
    {
      result = orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_gt(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOgt: {
    FOgtExpr *fe = cast<FOgtExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOgt");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), Z3ASTHandle(Z3_mk_fpa_gt(ctx, left, right), ctx));
    }
    else
    {
      result = Z3ASTHandle(Z3_mk_fpa_gt(ctx, left, right), ctx);
    }

    *width_out = 1;
    return result;
  }

  case Expr::FUge: {
    FUgeExpr *fe = cast<FUgeExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUge");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_geq(ctx, left, right), ctx)));
    }
    else
    {
      result = orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_geq(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOge: {
    FOgeExpr *fe = cast<FOgeExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOge");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), Z3ASTHandle(Z3_mk_fpa_geq(ctx, left, right), ctx));
    }
    else
    {
      result = Z3ASTHandle(Z3_mk_fpa_geq(ctx, left, right), ctx);
    }

    *width_out = 1;
    return result;
  }

  case Expr::FUlt: {
    FUltExpr *fe = cast<FUltExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUlt");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_lt(ctx, left, right), ctx)));
    }
    else
    {
      result = orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_lt(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOlt: {
    FOltExpr *fe = cast<FOltExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOlt");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), Z3ASTHandle(Z3_mk_fpa_lt(ctx, left, right), ctx));
    }
    else
    {
      result = Z3ASTHandle(Z3_mk_fpa_lt(ctx, left, right), ctx);
    }

    *width_out = 1;
    return result;
  }

  case Expr::FUle: {
    FUleExpr *fe = cast<FUleExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUle");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_leq(ctx, left, right), ctx)));
    }
    else
    {
      result = orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_leq(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOle: {
    FOleExpr *fe = cast<FOleExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOle");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      result = andExpr(notExpr(wrongHiddenBit), Z3ASTHandle(Z3_mk_fpa_leq(ctx, left, right), ctx));
    }
    else
    {
      result = Z3ASTHandle(Z3_mk_fpa_leq(ctx, left, right), ctx);
    }

    *width_out = 1;
    return result;
  }

  case Expr::FUne: {
    FUneExpr *fe = cast<FUneExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FUne");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      // != is the only comparison that is true for unnormal f80s
      result = orExpr(wrongHiddenBit, notExpr(Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx)));
    }
    else
    {
      result = notExpr(Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx));
    }

    *width_out = 1;
    return result;
  }

  case Expr::FOne: {
    FOneExpr *fe = cast<FOneExpr>(e);
    Z3ASTHandle left = construct(fe->left, width_out);
    Z3ASTHandle right = construct(fe->right, width_out);

    assert((*width_out == Expr::Fl32 || *width_out == Expr::Fl64 || *width_out == Expr::Fl80) && "non-float argument to FOne");

    Z3ASTHandle result;

    if (*width_out == Expr::Fl80)
    {
      Z3SortHandle sort = Z3SortHandle(Z3_mk_fpa_sort(ctx, 15, 64), ctx);
      Z3ASTHandle wrongHiddenBit = orExpr(isNanExpr(readExpr(left, bvOne(1))), isNanExpr(readExpr(right, bvOne(1))));
      left = readExpr(left, bvZero(1));
      right = readExpr(right, bvZero(1));
      // != is the only comparison that is true for unnormal f80s
      result = orExpr(wrongHiddenBit, notExpr(orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx))));
    }
    else
    {
      result = notExpr(orExpr(isNanExpr(left), isNanExpr(right), Z3ASTHandle(Z3_mk_fpa_eq(ctx, left, right), ctx)));
    }

    *width_out = 1;
    return result;
  }

// unused due to canonicalization
#if 0
  case Expr::Ne:
  case Expr::Ugt:
  case Expr::Uge:
  case Expr::Sgt:
  case Expr::Sge:
#endif

  default:
    e->dump();
    assert(0 && "unhandled Expr type");
    return getTrue();
  }
}
#endif // ENABLE_Z3
