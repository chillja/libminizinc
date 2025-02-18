/* -*- mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */

/*
 *  Main authors:
 *     Guido Tack <guido.tack@monash.edu>
 */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <minizinc/flat_exp.hh>

namespace MiniZinc {

EE flatten_comp(EnvI& env, const Ctx& ctx, Expression* e, VarDecl* r, VarDecl* b) {
  CallStackItem _csi(env, e);
  EE ret;
  auto* c = e->cast<Comprehension>();
  KeepAlive c_ka(c);

  bool isvarset = false;
  if (c->set()) {
    for (int i = 0; i < c->numberOfGenerators(); i++) {
      Expression* g_in = c->in(i);
      if (g_in != nullptr) {
        const Type& ty_in = g_in->type();
        if (ty_in == Type::varsetint()) {
          isvarset = true;
          break;
        }
        if (c->where(i) != nullptr) {
          if (c->where(i)->type() == Type::varbool()) {
            isvarset = true;
            break;
          }
        }
      }
    }
  }

  if (c->type().isOpt() || isvarset) {
    std::vector<Expression*> in(c->numberOfGenerators());
    std::vector<Expression*> orig_where(c->numberOfGenerators());
    std::vector<Expression*> where;
    GCLock lock;
    for (int i = 0; i < c->numberOfGenerators(); i++) {
      if (c->in(i) == nullptr) {
        in[i] = nullptr;
        orig_where[i] = c->where(i);
      } else {
        if (c->in(i)->type().isvar() && c->in(i)->type().dim() == 0) {
          std::vector<Expression*> args(1);
          args[0] = c->in(i);
          Call* ub = Call::a(Location().introduce(), "ub", args);
          Type t = Type::parsetint();
          t.cv(true);
          ub->type(t);
          ub->decl(env.model->matchFn(env, ub, false));
          in[i] = ub;
          for (int j = 0; j < c->numberOfDecls(i); j++) {
            auto* bo = new BinOp(Location().introduce(), c->decl(i, j)->id(), BOT_IN, c->in(i));
            bo->type(Type::varbool());
            where.push_back(bo);
          }
        } else {
          in[i] = c->in(i);
        }
        if ((c->where(i) != nullptr) && c->where(i)->type().isvar()) {
          // This is a generalised where clause. Split into par and var part.
          // The par parts can remain in where clause. The var parts are translated
          // into optionality constraints.
          if (c->where(i)->isa<BinOp>() && c->where(i)->cast<BinOp>()->op() == BOT_AND) {
            std::vector<Expression*> parWhere;
            std::vector<BinOp*> todo;
            todo.push_back(c->where(i)->cast<BinOp>());
            while (!todo.empty()) {
              BinOp* bo = todo.back();
              todo.pop_back();
              if (bo->rhs()->type().isPar()) {
                parWhere.push_back(bo->rhs());
              } else if (bo->rhs()->isa<BinOp>() && bo->rhs()->cast<BinOp>()->op() == BOT_AND) {
                todo.push_back(bo->rhs()->cast<BinOp>());
              } else {
                where.push_back(bo->rhs());
              }
              if (bo->lhs()->type().isPar()) {
                parWhere.push_back(bo->lhs());
              } else if (bo->lhs()->isa<BinOp>() && bo->lhs()->cast<BinOp>()->op() == BOT_AND) {
                todo.push_back(bo->lhs()->cast<BinOp>());
              } else {
                where.push_back(bo->lhs());
              }
            }
            switch (parWhere.size()) {
              case 0:
                orig_where[i] = nullptr;
                break;
              case 1:
                orig_where[i] = parWhere[0];
                break;
              case 2:
                orig_where[i] = new BinOp(c->where(i)->loc(), parWhere[0], BOT_AND, parWhere[1]);
                orig_where[i]->type(Type::parbool());
                break;
              default: {
                auto* parWhereAl = new ArrayLit(c->where(i)->loc(), parWhere);
                parWhereAl->type(Type::parbool(1));
                Call* forall = Call::a(c->where(i)->loc(), env.constants.ids.forall, {parWhereAl});
                forall->type(Type::parbool());
                forall->decl(env.model->matchFn(env, forall, false));
                orig_where[i] = forall;
                break;
              }
            }
          } else {
            orig_where[i] = nullptr;
            where.push_back(c->where(i));
          }
        } else {
          orig_where[i] = c->where(i);
        }
      }
    }
    if (!where.empty()) {
      Generators gs;
      for (int i = 0; i < c->numberOfGenerators(); i++) {
        std::vector<VarDecl*> vds(c->numberOfDecls(i));
        for (int j = 0; j < c->numberOfDecls(i); j++) {
          vds[j] = c->decl(i, j);
        }
        gs.g.emplace_back(vds, in[i], orig_where[i]);
      }
      Expression* cond;
      if (where.size() > 1) {
        auto* al = new ArrayLit(Location().introduce(), where);
        al->type(Type::varbool(1));
        std::vector<Expression*> args(1);
        args[0] = al;
        Call* forall = Call::a(Location().introduce(), env.constants.ids.forall, args);
        forall->type(Type::varbool());
        forall->decl(env.model->matchFn(env, forall, false));
        cond = forall;
      } else {
        cond = where[0];
      }

      Expression* new_e;

      Call* surround = env.surroundingCall();

      Type ntype = c->type();

      auto* indexes = c->e()->dynamicCast<ArrayLit>();
      Expression* generatedExp = c->e();
      if (indexes != nullptr && indexes->isTuple() &&
          indexes->type().typeId() == Type::COMP_INDEX) {
        generatedExp = (*indexes)[indexes->size() - 1];
      } else {
        indexes = nullptr;
      }

      if ((surround != nullptr) && surround->id() == env.constants.ids.forall) {
        new_e = new BinOp(Location().introduce(), cond, BOT_IMPL, generatedExp);
        new_e->type(Type::varbool());
        ntype.ot(Type::OT_PRESENT);
      } else if ((surround != nullptr) && surround->id() == env.constants.ids.exists) {
        new_e = new BinOp(Location().introduce(), cond, BOT_AND, generatedExp);
        new_e->type(Type::varbool());
        ntype.ot(Type::OT_PRESENT);
      } else if ((surround != nullptr) && surround->id() == env.constants.ids.sum) {
        // If the body of the comprehension is par, turn the whole expression into a linear sum.
        // Otherwise, generate if-then-else expressions.
        Type tt;
        tt = generatedExp->type();
        tt.ti(Type::TI_VAR);
        tt.ot(Type::OT_PRESENT);
        if (generatedExp->type().isPar()) {
          ASTString cid = generatedExp->type().bt() == Type::BT_INT ? env.constants.ids.bool2int
                                                                    : env.constants.ids.bool2float;
          Type b2i_t =
              generatedExp->type().bt() == Type::BT_INT ? Type::varint() : Type::varfloat();
          auto* b2i = Call::a(c->loc().introduce(), cid, {cond});
          b2i->type(b2i_t);
          b2i->decl(env.model->matchFn(env, b2i, false));
          auto* product = new BinOp(c->loc().introduce(), b2i, BOT_MULT, generatedExp);
          product->type(tt);
          new_e = product;
          ntype.ot(Type::OT_PRESENT);
        } else {
          auto* if_b_else_zero = new ITE(c->loc().introduce(), {cond, generatedExp}, IntLit::a(0));
          if_b_else_zero->type(tt);
          new_e = if_b_else_zero;
          ntype.ot(Type::OT_PRESENT);
        }
      } else {
        ITE* if_b_else_absent =
            new ITE(c->loc().introduce(), {cond, generatedExp}, env.constants.absent);
        Type tt;
        tt = generatedExp->type();
        tt.ti(Type::TI_VAR);
        tt.ot(Type::OT_OPTIONAL);
        if_b_else_absent->type(tt);
        new_e = if_b_else_absent;
      }
      if (indexes != nullptr) {
        std::vector<Expression*> new_indexes_v(indexes->size());
        for (unsigned int i = 0; i < indexes->size() - 1; i++) {
          new_indexes_v[i] = (*indexes)[i];
        }
        new_indexes_v.back() = new_e;
        new_e = ArrayLit::constructTuple(indexes->loc(), new_indexes_v);
      }
      auto* nc = new Comprehension(c->loc(), new_e, gs, c->set());
      nc->type(ntype);
      c = nc;
      c_ka = c;
    }
  }

  Ctx eval_ctx = ctx;
  if (ctx.b == C_ROOT && r != env.constants.varIgnore && c->type().bt() == Type::BT_BOOL &&
      c->type().st() == Type::ST_PLAIN) {
    eval_ctx.b = C_MIX;
  }

  class EvalF : public EvalBase {
  public:
    Ctx ctx;
    VarDecl* rr;
    EvalF(const Ctx& ctx0, VarDecl* rr0) : ctx(ctx0), rr(rr0) {}
    typedef EE ArrayVal;
    EE e(EnvI& env, Expression* e0) const {
      return flat_exp(env, ctx, e0, rr, ctx.partialityVar(env));
    }
  } _evalf(eval_ctx, r == env.constants.varIgnore ? env.constants.varTrue : nullptr);
  std::vector<EE> elems_ee;
  bool wasUndefined = false;
  EvaluatedComp<EE> evalResult;
  try {
    evalResult = eval_comp<EvalF>(env, _evalf, c);
    elems_ee = evalResult.a;
  } catch (ResultUndefinedError&) {
    wasUndefined = true;
  }
  std::vector<Expression*> elems(elems_ee.size());
  Type elemType = Type::bot();
  bool allPar = true;
  bool someOpt = false;
  for (auto i = static_cast<unsigned int>(elems.size()); (i--) != 0U;) {
    elems[i] = elems_ee[i].r();
    if (elemType == Type::bot()) {
      elemType = elems[i]->type();
    }
    if (!elems[i]->type().isPar()) {
      allPar = false;
    }
    if (elems[i]->type().isOpt()) {
      someOpt = true;
    }
  }
  if (elemType.isbot()) {
    elemType = c->type();
    elemType.mkPar(env);
  }
  if (!allPar) {
    elemType.mkVar(env);
  }
  if (someOpt) {
    elemType.ot(Type::OT_OPTIONAL);
  }
  if (c->set()) {
    elemType.st(Type::ST_SET);
  } else {
    elemType = Type::arrType(env, c->type(), elemType);
  }
  KeepAlive ka;
  {
    GCLock lock;
    if (c->set()) {
      if (c->type().isPar() && allPar) {
        auto* sl = new SetLit(c->loc(), elems);
        sl->type(elemType);
        Expression* slr = eval_par(env, sl);
        slr->type(elemType);
        ka = slr;
      } else {
        auto* alr = new ArrayLit(Location().introduce(), elems);
        elemType.st(Type::ST_PLAIN);
        elemType.dim(1);
        alr->type(elemType);
        alr->flat(true);
        Call* a2s = Call::a(Location().introduce(), "array2set", {alr});
        a2s->decl(env.model->matchFn(env, a2s, false));
        a2s->type(a2s->decl()->rtype(env, {alr}, nullptr, false));
        EE ee = flat_exp(env, Ctx(), a2s, nullptr, env.constants.varTrue);
        ka = ee.r();
      }
    } else {
      auto* alr = new ArrayLit(Location().introduce(), elems, evalResult.dims);
      alr->type(elemType);
      alr->flat(true);
      ka = alr;
    }
  }
  assert(!ka()->type().isbot());
  if (wasUndefined) {
    ret.b = bind(env, Ctx(), b, env.constants.literalFalse);
  } else {
    ret.b = conj(env, b, Ctx(), elems_ee);
  }
  ret.r = bind(env, Ctx(), r, ka());
  return ret;
}

}  // namespace MiniZinc
