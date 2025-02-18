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

EE flatten_let(EnvI& env, const Ctx& ctx, Expression* e, VarDecl* r, VarDecl* b) {
  CallStackItem _csi(env, e);
  EE ret;
  Let* let = e->cast<Let>();
  std::vector<EE> cs;
  std::vector<KeepAlive> flatmap;
  {
    LetPushBindings lpb(let);
    for (unsigned int i = 0; i < let->let().size(); i++) {
      Expression* le = let->let()[i];
      if (auto* vd = le->dynamicCast<VarDecl>()) {
        Expression* let_e = nullptr;
        if (vd->e() != nullptr) {
          Ctx nctx = ctx;
          BCtx transfer_ctx = let->type().bt() == Type::BT_INT ? nctx.i : nctx.b;
          nctx.neg = false;
          if (vd->ann().contains(env.constants.ctx.promise_monotone)) {
            if (vd->e()->type().bt() == Type::BT_BOOL) {
              nctx.b = +transfer_ctx;
            } else {
              nctx.i = +transfer_ctx;
            }
          } else if (vd->ann().contains(env.constants.ctx.promise_antitone)) {
            if (vd->e()->type().bt() == Type::BT_BOOL) {
              nctx.b = -transfer_ctx;
            } else {
              nctx.i = -transfer_ctx;
            }
          } else if (vd->e()->type().bt() == Type::BT_BOOL) {
            nctx.b = C_MIX;
          }

          CallStackItem csi_vd(env, vd);
          EE ee = flat_exp(env, nctx, vd->e(), nullptr, nctx.partialityVar(env));
          let_e = ee.r();
          cs.push_back(ee);
          check_index_sets(env, vd, let_e);
          if (vd->ti()->domain() != nullptr) {
            GCLock lock;
            auto* c = mk_domain_constraint(env, ee.r(), vd->ti()->domain());
            VarDecl* b_b = (nctx.b == C_ROOT && b == env.constants.varTrue) ? b : nullptr;
            VarDecl* r_r = (nctx.b == C_ROOT && b == env.constants.varTrue) ? b : nullptr;
            ee = flat_exp(env, nctx, c, r_r, b_b);
            cs.push_back(ee);
            ee.b = ee.r;
            cs.push_back(ee);
          }
          flatten_vardecl_annotations(env, vd, nullptr, vd);
        } else {
          if ((ctx.b == C_NEG || ctx.b == C_MIX) &&
              !vd->ann().contains(env.constants.ann.promise_total)) {
            CallStackItem csi_vd(env, vd);
            throw FlatteningError(env, vd->loc(), "free variable in non-positive context");
          }
          CallStackItem csi_vd(env, vd);
          GCLock lock;
          TypeInst* ti = eval_typeinst(env, ctx, vd);
          VarDecl* nvd = new_vardecl(env, ctx, ti, nullptr, vd, nullptr);
          let_e = nvd->id();
        }
        vd->e(let_e);
        flatmap.emplace_back(vd->flat());
        if (Id* id = Expression::dynamicCast<Id>(let_e)) {
          vd->flat(id->decl());
        } else {
          vd->flat(vd);
        }
      } else {
        if (ctx.b == C_ROOT || le->ann().contains(env.constants.ann.promise_total)) {
          (void)flat_exp(env, Ctx(), le, env.constants.varTrue, env.constants.varTrue);
        } else {
          EE ee = flat_exp(env, ctx, le, nullptr, env.constants.varTrue);
          ee.b = ee.r;
          cs.push_back(ee);
        }
      }
    }
    if (r == env.constants.varTrue && ctx.b == C_ROOT && !ctx.neg) {
      ret.b = bind(env, Ctx(), b, env.constants.literalTrue);
      (void)flat_exp(env, ctx, let->in(), r, b);
      ret.r = conj(env, r, Ctx(), cs);
    } else {
      Ctx nctx = ctx;
      nctx.neg = false;
      VarDecl* bb = b;
      for (EE& ee : cs) {
        if (ee.b() != env.constants.literalTrue) {
          bb = nullptr;
          break;
        }
      }
      EE ee = flat_exp(env, nctx, let->in(), nullptr, bb);
      if (let->type().isbool() && !let->type().isOpt()) {
        ee.b = ee.r;
        cs.push_back(ee);
        ret.r = conj(env, r, ctx, cs);
        ret.b = bind(env, Ctx(), b, env.constants.literalTrue);
      } else {
        cs.push_back(ee);
        ret.r = bind(env, Ctx(), r, ee.r());
        ret.b = conj(env, b, Ctx(), cs);
      }
    }
  }
  // Restore previous mapping
  for (unsigned int i = 0, j = 0; i < let->let().size(); i++) {
    if (auto* vd = let->let()[i]->dynamicCast<VarDecl>()) {
      vd->flat(Expression::cast<VarDecl>(flatmap[j++]()));
    }
  }
  return ret;
}
}  // namespace MiniZinc
