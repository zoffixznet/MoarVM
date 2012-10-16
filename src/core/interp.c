#include "moarvm.h"
#include "math.h"

/* Macros for getting things from the bytecode stream. */
#define GET_REG(pc, idx)    reg_base[*((MVMuint16 *)(pc + idx))]
#define GET_LEX(pc, idx, f) f->env[*((MVMuint16 *)(pc + idx))]
#define GET_I16(pc, idx)    *((MVMint16 *)(pc + idx))
#define GET_UI16(pc, idx)   *((MVMuint16 *)(pc + idx))
#define GET_I32(pc, idx)    *((MVMint32 *)(pc + idx))
#define GET_UI32(pc, idx)   *((MVMuint32 *)(pc + idx))
#define GET_I64(pc, idx)    *((MVMint64 *)(pc + idx))
#define GET_UI64(pc, idx)   *((MVMuint64 *)(pc + idx))
#define GET_N32(pc, idx)    *((MVMnum32 *)(pc + idx))
#define GET_N64(pc, idx)    *((MVMnum64 *)(pc + idx))

/* This is the interpreter run loop. We have one of these per thread. */
void MVM_interp_run(MVMThreadContext *tc, void (*initial_invoke)(MVMThreadContext *, void *), void *invoke_data) {
    /* Points to the current opcode. */
    MVMuint8 *cur_op = NULL;
    
    /* The current frame's bytecode start. */
    MVMuint8 *bytecode_start = NULL;
    
    /* Points to the base of the current register set for the frame we
     * are presently in. */
    MVMRegister *reg_base = NULL;
    
    /* Points to the current compilation unit. */
    MVMCompUnit *cu = NULL;
    
    /* The current call site we're constructing. */
    MVMCallsite *cur_callsite = NULL;
    
    /* Stash addresses of current op, register base and SC deref base
     * in the TC; this will be used by anything that needs to switch
     * the current place we're interpreting. */
    tc->interp_cur_op         = &cur_op;
    tc->interp_bytecode_start = &bytecode_start;
    tc->interp_reg_base       = &reg_base;
    tc->interp_cu             = &cu;
    
    /* With everything set up, do the initial invocation (exactly what this does
     * varies depending on if this is starting a new thread or is the top-level
     * program entry point). */
    initial_invoke(tc, invoke_data);
    
    /* Enter runloop. */
    while (1) {
        /* Primary dispatch by op bank. */
        switch (*(cur_op++)) {
            /* Control flow and primitive operations. */
            case MVM_OP_BANK_primitives: {
                switch (*(cur_op++)) {
                    case MVM_OP_no_op:
                        break;
                    case MVM_OP_goto:
                        cur_op = bytecode_start + GET_UI32(cur_op, 0);
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_if_i:
                        if (GET_REG(cur_op, 0).i64)
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        else
                            cur_op += 6;
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_unless_i:
                        if (GET_REG(cur_op, 0).i64)
                            cur_op += 6;
                        else
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_if_n:
                        if (GET_REG(cur_op, 0).n64 != 0.0)
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        else
                            cur_op += 6;
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_unless_n:
                        if (GET_REG(cur_op, 0).n64 != 0.0)
                            cur_op += 6;
                        else
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_if_s: {
                        MVMString *str = GET_REG(cur_op, 0).s;
                        if (!str || str->body.graphs == 0)
                            cur_op += 6;
                        else
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        GC_SYNC_POINT(tc);
                        break;
                    }
                    case MVM_OP_unless_s: {
                        MVMString *str = GET_REG(cur_op, 0).s;
                        if (!str || str->body.graphs == 0)
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        else
                            cur_op += 6;
                        GC_SYNC_POINT(tc);
                        break;
                    }
                    case MVM_OP_if_s0: {
                        MVMString *str = GET_REG(cur_op, 0).s;
                        if (!str || str->body.graphs != 1
                            || str->body.data[0] != 48) /* zero char */
                            cur_op += 6;
                        else
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        GC_SYNC_POINT(tc);
                        break;
                    }
                    case MVM_OP_unless_s0: {
                        MVMString *str = GET_REG(cur_op, 0).s;
                        if (!str || str->body.graphs != 1
                            || str->body.data[0] != 48) /* zero char */
                            cur_op = bytecode_start + GET_UI32(cur_op, 2);
                        else
                            cur_op += 6;
                        GC_SYNC_POINT(tc);
                        break;
                    }
                    case MVM_OP_set:
                        GET_REG(cur_op, 0) = GET_REG(cur_op, 2);
                        cur_op += 4;
                        break;
                    case MVM_OP_getlex: {
                        MVMFrame *f = tc->cur_frame;
                        MVMuint16 outers = GET_UI16(cur_op, 4);
                        while (outers) {
                            f = f->outer;
                            outers--;
                        }
                        GET_REG(cur_op, 0) = GET_LEX(cur_op, 2, f);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindlex: {
                        MVMFrame *f = tc->cur_frame;
                        MVMuint16 outers = GET_UI16(cur_op, 2);
                        while (outers) {
                            f = f->outer;
                            outers--;
                        }
                        GET_LEX(cur_op, 0, f) = GET_REG(cur_op, 4);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_getlex_ni:
                        GET_REG(cur_op, 0).i64 = MVM_frame_find_lexical_by_name(tc,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_reg_int64)->i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_getlex_nn:
                        GET_REG(cur_op, 0).n64 = MVM_frame_find_lexical_by_name(tc,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_reg_num64)->n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_getlex_ns:
                        GET_REG(cur_op, 0).s = MVM_frame_find_lexical_by_name(tc,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_reg_str)->s;
                        cur_op += 4;
                        break;
                    case MVM_OP_getlex_no:
                        GET_REG(cur_op, 0).o = MVM_frame_find_lexical_by_name(tc,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_reg_obj)->o;
                        cur_op += 4;
                        break;
                    case MVM_OP_bindlex_ni:
                        MVM_frame_find_lexical_by_name(tc, cu->strings[GET_UI16(cur_op, 0)],
                            MVM_reg_int64)->i64 = GET_REG(cur_op, 2).i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_bindlex_nn:
                        MVM_frame_find_lexical_by_name(tc, cu->strings[GET_UI16(cur_op, 0)],
                            MVM_reg_num64)->n64 = GET_REG(cur_op, 2).n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_bindlex_ns:
                        MVM_frame_find_lexical_by_name(tc, cu->strings[GET_UI16(cur_op, 0)],
                            MVM_reg_str)->s = GET_REG(cur_op, 2).s;
                        cur_op += 4;
                        break;
                    case MVM_OP_bindlex_no:
                        MVM_frame_find_lexical_by_name(tc, cu->strings[GET_UI16(cur_op, 0)],
                            MVM_reg_obj)->o = GET_REG(cur_op, 2).o;
                        cur_op += 4;
                        break;
                    case MVM_OP_return_i:
                        MVM_args_set_result_int(tc, GET_REG(cur_op, 0).i64,
                            MVM_RETURN_CALLER_FRAME);
                        if (MVM_frame_try_return(tc))
                            break;
                        else
                            return;
                    case MVM_OP_return_n:
                        MVM_args_set_result_num(tc, GET_REG(cur_op, 0).n64,
                            MVM_RETURN_CALLER_FRAME);
                        if (MVM_frame_try_return(tc))
                            break;
                        else
                            return;
                    case MVM_OP_return_s:
                        MVM_args_set_result_str(tc, GET_REG(cur_op, 0).s,
                            MVM_RETURN_CALLER_FRAME);
                        if (MVM_frame_try_return(tc))
                            break;
                        else
                            return;
                    case MVM_OP_return_o:
                        MVM_args_set_result_obj(tc, GET_REG(cur_op, 0).o,
                            MVM_RETURN_CALLER_FRAME);
                        if (MVM_frame_try_return(tc))
                            break;
                        else
                            return;
                    case MVM_OP_return:
                        MVM_args_assert_void_return_ok(tc, MVM_RETURN_CALLER_FRAME);
                        if (MVM_frame_try_return(tc))
                            break;
                        else
                            return;
                    case MVM_OP_const_i64:
                        GET_REG(cur_op, 0).i64 = GET_I64(cur_op, 2);
                        cur_op += 10;
                        break;
                    case MVM_OP_const_n64:
                        GET_REG(cur_op, 0).n64 = GET_N64(cur_op, 2);
                        cur_op += 10;
                        break;
                    case MVM_OP_const_s:
                        GET_REG(cur_op, 0).s = cu->strings[GET_UI16(cur_op, 2)];
                        cur_op += 4;
                        break;
                    case MVM_OP_add_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 + GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_sub_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 - GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_mul_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 * GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_div_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 / GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_div_u:
                        GET_REG(cur_op, 0).ui64 = GET_REG(cur_op, 2).ui64 / GET_REG(cur_op, 4).ui64;
                        cur_op += 6;
                        break;
                    case MVM_OP_mod_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 % GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_mod_u:
                        GET_REG(cur_op, 0).ui64 = GET_REG(cur_op, 2).ui64 % GET_REG(cur_op, 4).ui64;
                        cur_op += 6;
                        break;
                    case MVM_OP_neg_i:
                        GET_REG(cur_op, 0).i64 = -GET_REG(cur_op, 2).i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_inc_i:
                        GET_REG(cur_op, 0).i64++;
                        cur_op += 2;
                        break;
                    case MVM_OP_inc_u:
                        GET_REG(cur_op, 0).ui64++;
                        cur_op += 2;
                        break;
                    case MVM_OP_dec_i:
                        GET_REG(cur_op, 0).i64--;
                        cur_op += 2;
                        break;
                    case MVM_OP_dec_u:
                        GET_REG(cur_op, 0).ui64--;
                        cur_op += 2;
                        break;
                    case MVM_OP_getcode:
                        GET_REG(cur_op, 0).o = cu->coderefs[GET_UI16(cur_op, 2)];
                        cur_op += 4;
                        break;
                    case MVM_OP_prepargs:
                        cur_callsite = cu->callsites[GET_UI16(cur_op, 0)];
                        cur_op += 2;
                        break;
                    case MVM_OP_arg_i:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].i64 = GET_REG(cur_op, 2).i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_arg_n:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].n64 = GET_REG(cur_op, 2).n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_arg_s:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].s = GET_REG(cur_op, 2).s;
                        cur_op += 4;
                        break;
                    case MVM_OP_arg_o:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].o = GET_REG(cur_op, 2).o;
                        cur_op += 4;
                        break;
                    case MVM_OP_invoke_v:
                        {
                            MVMObject *code = GET_REG(cur_op, 0).o;
                            tc->cur_frame->return_value = NULL;
                            tc->cur_frame->return_type = MVM_RETURN_VOID;
                            cur_op += 2;
                            tc->cur_frame->return_address = cur_op;
                            STABLE(code)->invoke(tc, code, cur_callsite, tc->cur_frame->args);
                        }
                        break;
                    case MVM_OP_invoke_i:
                        {
                            MVMObject *code = GET_REG(cur_op, 2).o;
                            tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                            tc->cur_frame->return_type = MVM_RETURN_INT;
                            cur_op += 4;
                            tc->cur_frame->return_address = cur_op;
                            STABLE(code)->invoke(tc, code, cur_callsite, tc->cur_frame->args);
                        }
                        break;
                    case MVM_OP_invoke_n:
                        {
                            MVMObject *code = GET_REG(cur_op, 2).o;
                            tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                            tc->cur_frame->return_type = MVM_RETURN_NUM;
                            cur_op += 4;
                            tc->cur_frame->return_address = cur_op;
                            STABLE(code)->invoke(tc, code, cur_callsite, tc->cur_frame->args);
                        }
                        break;
                    case MVM_OP_invoke_s:
                        {
                            MVMObject *code = GET_REG(cur_op, 2).o;
                            tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                            tc->cur_frame->return_type = MVM_RETURN_STR;
                            cur_op += 4;
                            tc->cur_frame->return_address = cur_op;
                            STABLE(code)->invoke(tc, code, cur_callsite, tc->cur_frame->args);
                        }
                        break;
                    case MVM_OP_invoke_o:
                        {
                            MVMObject *code = GET_REG(cur_op, 2).o;
                            tc->cur_frame->return_value = &GET_REG(cur_op, 0);
                            tc->cur_frame->return_type = MVM_RETURN_OBJ;
                            cur_op += 4;
                            tc->cur_frame->return_address = cur_op;
                            STABLE(code)->invoke(tc, code, cur_callsite, tc->cur_frame->args);
                        }
                        break;
                    case MVM_OP_add_n:
                        GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 + GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_sub_n:
                        GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 - GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_mul_n:
                        GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 * GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_div_n:
                        GET_REG(cur_op, 0).n64 = GET_REG(cur_op, 2).n64 / GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_neg_n:
                        GET_REG(cur_op, 0).n64 = -GET_REG(cur_op, 2).n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_eq_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 == GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_ne_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 != GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_lt_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 <  GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_le_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 <= GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_gt_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >  GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_ge_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >= GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_eq_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 == GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_ne_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 != GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_lt_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 <  GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_le_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 <= GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_gt_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 >  GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_ge_n:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).n64 >= GET_REG(cur_op, 4).n64;
                        cur_op += 6;
                        break;
                    case MVM_OP_argconst_i:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].i64 = GET_I64(cur_op, 2);
                        cur_op += 10;
                        break;
                    case MVM_OP_argconst_n:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].n64 = GET_N64(cur_op, 2);
                        cur_op += 10;
                        break;
                    case MVM_OP_argconst_s:
                        tc->cur_frame->args[GET_UI16(cur_op, 0)].s = cu->strings[GET_UI16(cur_op, 2)];
                        cur_op += 4;
                        break;
                    case MVM_OP_checkarity:
                        MVM_args_checkarity(tc, &tc->cur_frame->params, GET_UI16(cur_op, 0), GET_UI16(cur_op, 2));
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rp_i:
                        GET_REG(cur_op, 0).i64 = MVM_args_get_pos_int(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_REQUIRED)->i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rp_n:
                        GET_REG(cur_op, 0).n64 = MVM_args_get_pos_num(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_REQUIRED)->n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rp_s:
                        GET_REG(cur_op, 0).s = MVM_args_get_pos_str(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_REQUIRED)->s;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rp_o:
                        GET_REG(cur_op, 0).o = MVM_args_get_pos_obj(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_REQUIRED)->o;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_op_i:
                    {
                        MVMRegister *param = MVM_args_get_pos_int(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).i64 = param->i64;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_op_n:
                    {
                        MVMRegister *param = MVM_args_get_pos_num(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).n64 = param->n64;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_op_s:
                    {
                        MVMRegister *param = MVM_args_get_pos_str(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).s = param->s;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_op_o:
                    {
                        MVMRegister *param = MVM_args_get_pos_obj(tc, &tc->cur_frame->params,
                            GET_UI16(cur_op, 2), MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).o = param->o;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_rn_i:
                        GET_REG(cur_op, 0).i64 = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_REQUIRED)->i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rn_n:
                        GET_REG(cur_op, 0).n64 = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_REQUIRED)->n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rn_s:
                        GET_REG(cur_op, 0).s = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_REQUIRED)->s;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_rn_o:
                        GET_REG(cur_op, 0).o = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_REQUIRED)->o;
                        cur_op += 4;
                        break;
                    case MVM_OP_param_on_i:
                    {
                        MVMRegister *param = MVM_args_get_named_int(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).i64 = param->i64;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_on_n:
                    {
                        MVMRegister *param = MVM_args_get_named_num(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).n64 = param->n64;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_on_s:
                    {
                        MVMRegister *param = MVM_args_get_named_str(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).s = param->s;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_param_on_o:
                    {
                        MVMRegister *param = MVM_args_get_named_obj(tc, &tc->cur_frame->params,
                            cu->strings[GET_UI16(cur_op, 2)], MVM_ARG_OPTIONAL);
                        if (param) {
                            GET_REG(cur_op, 0).o = param->o;
                            cur_op = bytecode_start + GET_UI32(cur_op, 4);
                        }
                        else {
                            cur_op += 8;
                        }
                        break;
                    }
                    case MVM_OP_coerce_in:
                        GET_REG(cur_op, 0).n64 = (MVMnum64)GET_REG(cur_op, 2).i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_coerce_ni:
                        GET_REG(cur_op, 0).i64 = (MVMint64)GET_REG(cur_op, 2).n64;
                        cur_op += 4;
                        break;
                    case MVM_OP_band_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 & GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_bor_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 | GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_bxor_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 ^ GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_bnot_i:
                        GET_REG(cur_op, 0).i64 = ~GET_REG(cur_op, 2).i64;
                        cur_op += 4;
                        break;
                    case MVM_OP_blshift_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 << GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_brshift_i:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).i64 >> GET_REG(cur_op, 4).i64;
                        cur_op += 6;
                        break;
                    case MVM_OP_pow_i: {
                            MVMint64 base = GET_REG(cur_op, 2).i64;
                            MVMint64 exp = GET_REG(cur_op, 4).i64;
                            MVMint64 result = 1;
                            /* "Exponentiation by squaring" */
                            if (exp < 0) {
                                result = 0; /* because 1/base**-exp is between 0 and 1 */
                            }
                            else {
                                while (exp) {
                                    if (exp & 1)
                                        result *= base;
                                    exp >>= 1;
                                    base *= base;
                                }
                            }
                            GET_REG(cur_op, 0).i64 = result;
                        }
                        cur_op += 6;
                        break;
                    case MVM_OP_pow_n:
                        GET_REG(cur_op, 0).n64 = pow(GET_REG(cur_op, 2).n64, GET_REG(cur_op, 4).n64);
                        cur_op += 6;
                        break;
                    case MVM_OP_takeclosure:
                        GET_REG(cur_op, 0).o = MVM_frame_takeclosure(tc, GET_REG(cur_op, 2).o);
                        cur_op += 4;
                        break;
                    case MVM_OP_jumplist: {
                        MVMint64 num_labels = GET_I64(cur_op, 0);
                        MVMint64 input = GET_REG(cur_op, 8).i64;
                        cur_op += 10;
                        /* the goto ops are guaranteed valid/existent by validation.c */
                        if (input < 0 || input >= num_labels) { /* implicitly covers num_labels == 0 */
                            /* skip the entire goto list block */
                            cur_op += (6 /* size of each goto op */) * num_labels;
                        }
                        else { /* delve directly into the selected goto op */
                            cur_op = bytecode_start + GET_UI32(cur_op,
                                input * (6 /* size of each goto op */) 
                                + (2 /* size of the goto instruction itself */));
                        }
                        GC_SYNC_POINT(tc);
                        break;
                    }
                    case MVM_OP_caller: {
                        MVMFrame *caller = tc->cur_frame;
                        MVMint64 depth = GET_REG(cur_op, 2).i64;
                        
                        while (caller && depth-- > 0) /* keep the > 0. */
                            caller = caller->caller;
                        
                        GET_REG(cur_op, 0).o = caller ? caller->code_ref : NULL;
                        
                        cur_op += 4;
                        break;
                    }
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_primitives, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* Development operations. */
            case MVM_OP_BANK_dev: {
                switch (*(cur_op++)) {
                    case MVM_OP_say_i:
                        printf("%lld\n", GET_REG(cur_op, 0).i64); /* lld seems to work on msvc and gcc */
                        cur_op += 2;
                        break;
                    case MVM_OP_say_n:
                        printf("%f\n", GET_REG(cur_op, 0).n64);
                        cur_op += 2;
                        break;
                    case MVM_OP_say_s:
                        MVM_string_say(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_sleep: /* microseconds for now */
                        apr_sleep((apr_interval_time_t)GET_REG(cur_op, 0).i64);
                        cur_op += 2;
                        break;
                    case MVM_OP_anonoshtype:
                        GET_REG(cur_op, 0).o = MVM_file_get_anon_oshandle_type(tc);
                        cur_op += 2;
                        break;
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_dev, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* String operations. */
            case MVM_OP_BANK_string: {
                switch (*(cur_op++)) {
                    case MVM_OP_concat_s:
                        GET_REG(cur_op, 0).s = MVM_string_concatenate(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    case MVM_OP_repeat_s:
                        GET_REG(cur_op, 0).s = MVM_string_repeat(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_substr_s:
                        GET_REG(cur_op, 0).s = MVM_string_substring(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64,
                            GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    case MVM_OP_index_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_index(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    case MVM_OP_graphs_s:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).s->body.graphs;
                        cur_op += 4;
                        break;
                    case MVM_OP_codes_s:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).s->body.codes;
                        cur_op += 4;
                        break;
                    case MVM_OP_eq_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_equal(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    case MVM_OP_ne_s:
                        GET_REG(cur_op, 0).i64 = (MVMint64)(MVM_string_equal(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s)?0:1);
                        cur_op += 6;
                        break;
                    case MVM_OP_eqat_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_equal_at(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s,
                            GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    case MVM_OP_haveat_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_have_at(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64,
                            GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).s,
                            GET_REG(cur_op, 10).i64);
                        cur_op += 12;
                        break;
                    case MVM_OP_getcp_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_get_codepoint_at(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_indexcp_s:
                        GET_REG(cur_op, 0).i64 = MVM_string_index_of_codepoint(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_uc:
                        GET_REG(cur_op, 0).s = MVM_string_uc(tc,
                            GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_lc:
                        GET_REG(cur_op, 0).s = MVM_string_lc(tc,
                            GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_tc:
                        GET_REG(cur_op, 0).s = MVM_string_tc(tc,
                            GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_split:
                        GET_REG(cur_op, 0).o = MVM_string_split(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s);
                        cur_op += 8;
                        break;
                    case MVM_OP_join:
                        GET_REG(cur_op, 0).s = MVM_string_join(tc,
                            GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    /*case MVM_OP_replace:
                        GET_REG(cur_op, 0).s = MVM_string_replace(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).s);
                        cur_op += 8;
                        break;*/
                    case MVM_OP_getcpbyname:
                        GET_REG(cur_op, 0).i64 = MVM_unicode_lookup_by_name(tc,
                            GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_indexat_scb:
                        /* branches on *failure* to match in the constant string, to save an instruction in regexes */
                        if (MVM_string_char_at_in_string(tc, GET_REG(cur_op, 0).s,
                                GET_REG(cur_op, 2).i64, cu->strings[GET_UI16(cur_op, 4)]))
                            cur_op += 10;
                        else
                            cur_op = bytecode_start + GET_UI32(cur_op, 6);
                        GC_SYNC_POINT(tc);
                        break;
                    case MVM_OP_unipropcode:
                        GET_REG(cur_op, 0).i64 = (MVMint64)MVM_unicode_name_to_property_code(tc,
                            GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_unipvalcode:
                        GET_REG(cur_op, 0).i64 = (MVMint64)MVM_unicode_name_to_property_value_code(tc,
                            GET_REG(cur_op, 2).i64, GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    case MVM_OP_hasuniprop:
                        GET_REG(cur_op, 0).i64 = MVM_string_offset_has_unicode_property_value(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64,
                            GET_REG(cur_op, 8).i64);
                        cur_op += 10;
                        break;
                    case MVM_OP_hasunipropc:
                        GET_REG(cur_op, 0).i64 = MVM_string_offset_has_unicode_property_value(tc,
                            GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64, (MVMint64)GET_UI16(cur_op, 6),
                            (MVMint64)GET_UI16(cur_op, 8));
                        cur_op += 10;
                        break;
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_string, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* Math operations other than the primitives. */
            case MVM_OP_BANK_math: {
                switch (*(cur_op++)) {
                    case MVM_OP_sin_n:
                        GET_REG(cur_op, 0).n64 = sin(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_asin_n:
                        GET_REG(cur_op, 0).n64 = asin(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_cos_n:
                        GET_REG(cur_op, 0).n64 = cos(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_acos_n:
                        GET_REG(cur_op, 0).n64 = acos(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_tan_n:
                        GET_REG(cur_op, 0).n64 = tan(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_atan_n:
                        GET_REG(cur_op, 0).n64 = atan(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_atan2_n:
                        GET_REG(cur_op, 0).n64 = atan2(GET_REG(cur_op, 2).n64,
                            GET_REG(cur_op, 4).n64);
                        cur_op += 6;
                        break;
                    case MVM_OP_sec_n: /* XXX TODO: handle edge cases */
                        GET_REG(cur_op, 0).n64 = 1.0 / cos(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_asec_n: /* XXX TODO: handle edge cases */
                        GET_REG(cur_op, 0).n64 = acos(1.0 / GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_sinh_n:
                        GET_REG(cur_op, 0).n64 = sinh(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_cosh_n:
                        GET_REG(cur_op, 0).n64 = cosh(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_tanh_n:
                        GET_REG(cur_op, 0).n64 = tanh(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    case MVM_OP_sech_n: /* XXX TODO: handle edge cases */
                        GET_REG(cur_op, 0).n64 = 1.0 / cosh(GET_REG(cur_op, 2).n64);
                        cur_op += 4;
                        break;
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_math, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* Object operations. */
            case MVM_OP_BANK_object: {
                switch (*(cur_op++)) {
                    case MVM_OP_knowhow:
                        GET_REG(cur_op, 0).o = tc->instance->KnowHOW;
                        cur_op += 2;
                        break;
                    case MVM_OP_findmeth:
                        GET_REG(cur_op, 0).o = MVM_6model_find_method(tc,
                            GET_REG(cur_op, 2).o,
                            cu->strings[GET_UI16(cur_op, 4)]);
                        cur_op += 6;
                        break;
                    case MVM_OP_findmeth_s:
                        GET_REG(cur_op, 0).o = MVM_6model_find_method(tc,
                            GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    case MVM_OP_create: {
                        /* Ordering here matters. We write the object into the
                         * register before calling initialize. This is because
                         * if initialize allocates, obj may have moved after
                         * we called it. Note that type is never used after
                         * the initial allocate call also. This saves us having
                         * to put things on the temporary stack. */
                        MVMObject *type = GET_REG(cur_op, 2).o;
                        MVMObject *obj  = REPR(type)->allocate(tc, STABLE(type));
                        GET_REG(cur_op, 0).o = obj;
                        if (REPR(obj)->initialize)
                            REPR(obj)->initialize(tc, STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_gethow:
                        GET_REG(cur_op, 0).o = STABLE(GET_REG(cur_op, 2).o)->HOW;
                        cur_op += 4;
                        break;
                    case MVM_OP_getwhat:
                        GET_REG(cur_op, 0).o = STABLE(GET_REG(cur_op, 2).o)->WHAT;
                        cur_op += 4;
                        break;
                    case MVM_OP_atkey_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        MVMObject *result = REPR(obj)->ass_funcs->at_key_boxed(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            (MVMObject *)GET_REG(cur_op, 4).s);
                        if (REPR(result)->ID != MVM_REPR_ID_MVMString)
                            MVM_exception_throw_adhoc(tc, "object does not have REPR MVMString");
                        GET_REG(cur_op, 0).s = (MVMString *)result;
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_atkey_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).o = REPR(obj)->ass_funcs->at_key_boxed(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            (MVMObject *)GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindkey_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->ass_funcs->bind_key_boxed(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                            (MVMObject *)GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindkey_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->ass_funcs->bind_key_boxed(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s,
                            GET_REG(cur_op, 4).o);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_existskey: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).i64 = REPR(obj)->ass_funcs->exists_key(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            (MVMObject *)GET_REG(cur_op, 4).s);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_deletekey: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->ass_funcs->delete_key(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), (MVMObject *)GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_elemskeyed: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).i64 = REPR(obj)->ass_funcs->elems(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_eqaddr:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).o == GET_REG(cur_op, 4).o ? 1 : 0;
                        cur_op += 6;
                        break;
                    case MVM_OP_reprname:
                        GET_REG(cur_op, 0).s = REPR(GET_REG(cur_op, 2).o)->name;
                        cur_op += 4;
                        break;
                    case MVM_OP_isconcrete:
                        GET_REG(cur_op, 0).i64 = IS_CONCRETE(GET_REG(cur_op, 2).o) ? 1 : 0;
                        cur_op += 4;
                        break;
                    case MVM_OP_atpos_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->at_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                            &GET_REG(cur_op, 0), MVM_reg_int64);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_atpos_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->at_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                            &GET_REG(cur_op, 0), MVM_reg_num64);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_atpos_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->at_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                            &GET_REG(cur_op, 0), MVM_reg_str);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_atpos_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->at_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 4).i64,
                            &GET_REG(cur_op, 0), MVM_reg_obj);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindpos_i: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->bind_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                            GET_REG(cur_op, 4), MVM_reg_int64);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindpos_n: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->bind_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                            GET_REG(cur_op, 4), MVM_reg_num64);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindpos_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->bind_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                            GET_REG(cur_op, 4), MVM_reg_str);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_bindpos_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->bind_pos(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).i64,
                            GET_REG(cur_op, 4), MVM_reg_obj);
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_push_i: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->push(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_int64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_push_n: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->push(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_num64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_push_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->push(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_str);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_push_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->push(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_obj);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_pop_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->pop(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_int64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_pop_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->pop(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_num64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_pop_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->pop(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_str);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_pop_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->pop(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_obj);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unshift_i: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->unshift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_int64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unshift_n: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->unshift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_num64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unshift_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->unshift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_str);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unshift_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->unshift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2), MVM_reg_obj);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_shift_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->shift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_int64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_shift_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->shift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_num64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_shift_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->shift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_str);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_shift_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->pos_funcs->shift(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), &GET_REG(cur_op, 0), MVM_reg_obj);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_splice: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->splice(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).i64, GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_elemspos: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).i64 = REPR(obj)->pos_funcs->elems(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_setelemspos: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->pos_funcs->set_elems(tc, STABLE(obj), obj,
                            OBJECT_BODY(obj), GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_box_i: {
                        MVMObject *type = GET_REG(cur_op, 4).o;
                        MVMObject *box  = REPR(type)->allocate(tc, STABLE(type));
                        MVM_gc_root_temp_push(tc, (MVMCollectable **)&box);
                        if (REPR(box)->initialize)
                            REPR(box)->initialize(tc, STABLE(box), box, OBJECT_BODY(box));
                        REPR(box)->box_funcs->set_int(tc, STABLE(box), box,
                            OBJECT_BODY(box), GET_REG(cur_op, 2).i64);
                        MVM_gc_root_temp_pop(tc);
                        GET_REG(cur_op, 0).o = box;
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_box_n: {
                        MVMObject *type = GET_REG(cur_op, 4).o;
                        MVMObject *box  = REPR(type)->allocate(tc, STABLE(type));
                        MVM_gc_root_temp_push(tc, (MVMCollectable **)&box);
                        if (REPR(box)->initialize)
                            REPR(box)->initialize(tc, STABLE(box), box, OBJECT_BODY(box));
                        REPR(box)->box_funcs->set_num(tc, STABLE(box), box,
                            OBJECT_BODY(box), GET_REG(cur_op, 2).n64);
                        MVM_gc_root_temp_pop(tc);
                        GET_REG(cur_op, 0).o = box;
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_box_s: {
                        MVMObject *type = GET_REG(cur_op, 4).o;
                        MVMObject *box  = REPR(type)->allocate(tc, STABLE(type));
                        MVM_gc_root_temp_push(tc, (MVMCollectable **)&box);
                        if (REPR(box)->initialize)
                            REPR(box)->initialize(tc, STABLE(box), box, OBJECT_BODY(box));
                        REPR(box)->box_funcs->set_str(tc, STABLE(box), box,
                            OBJECT_BODY(box), GET_REG(cur_op, 2).s);
                        MVM_gc_root_temp_pop(tc);
                        GET_REG(cur_op, 0).o = box;
                        cur_op += 6;
                        break;
                    }
                    case MVM_OP_unbox_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).i64 = REPR(obj)->box_funcs->get_int(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unbox_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).n64 = REPR(obj)->box_funcs->get_num(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_unbox_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        GET_REG(cur_op, 0).s = REPR(obj)->box_funcs->get_str(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj));
                        cur_op += 4;
                        break;
                    }
                    case MVM_OP_bindattr_i: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, cu->strings[GET_UI16(cur_op, 4)],
                            GET_I16(cur_op, 8), GET_REG(cur_op, 6), MVM_reg_int64);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_bindattr_n: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, cu->strings[GET_UI16(cur_op, 4)],
                            GET_I16(cur_op, 8), GET_REG(cur_op, 6), MVM_reg_num64);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_bindattr_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, cu->strings[GET_UI16(cur_op, 4)],
                            GET_I16(cur_op, 8), GET_REG(cur_op, 6), MVM_reg_str);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_bindattr_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, cu->strings[GET_UI16(cur_op, 4)],
                            GET_I16(cur_op, 8), GET_REG(cur_op, 6), MVM_reg_obj);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_bindattrs_i: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            -1, GET_REG(cur_op, 6), MVM_reg_int64);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_bindattrs_n: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            -1, GET_REG(cur_op, 6), MVM_reg_num64);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_bindattrs_s: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            -1, GET_REG(cur_op, 6), MVM_reg_str);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_bindattrs_o: {
                        MVMObject *obj = GET_REG(cur_op, 0).o;
                        REPR(obj)->attr_funcs->bind_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            -1, GET_REG(cur_op, 6), MVM_reg_obj);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_getattr_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, cu->strings[GET_UI16(cur_op, 6)],
                            GET_I16(cur_op, 8), &GET_REG(cur_op, 0), MVM_reg_int64);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_getattr_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, cu->strings[GET_UI16(cur_op, 6)],
                            GET_I16(cur_op, 8), &GET_REG(cur_op, 0), MVM_reg_num64);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_getattr_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, cu->strings[GET_UI16(cur_op, 6)],
                            GET_I16(cur_op, 8), &GET_REG(cur_op, 0), MVM_reg_str);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_getattr_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, cu->strings[GET_UI16(cur_op, 6)],
                            GET_I16(cur_op, 8), &GET_REG(cur_op, 0), MVM_reg_obj);
                        cur_op += 10;
                        break;
                    }
                    case MVM_OP_getattrs_i: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                            -1, &GET_REG(cur_op, 0), MVM_reg_int64);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_getattrs_n: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                            -1, &GET_REG(cur_op, 0), MVM_reg_num64);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_getattrs_s: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                            -1, &GET_REG(cur_op, 0), MVM_reg_str);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_getattrs_o: {
                        MVMObject *obj = GET_REG(cur_op, 2).o;
                        REPR(obj)->attr_funcs->get_attribute(tc,
                            STABLE(obj), obj, OBJECT_BODY(obj),
                            GET_REG(cur_op, 4).o, GET_REG(cur_op, 6).s,
                            -1, &GET_REG(cur_op, 0), MVM_reg_obj);
                        cur_op += 8;
                        break;
                    }
                    case MVM_OP_isnull:
                        GET_REG(cur_op, 0).i64 = GET_REG(cur_op, 2).o ? 0 : 1;
                        cur_op += 4;
                        break;
                    case MVM_OP_knowhowattr:
                        GET_REG(cur_op, 0).o = tc->instance->KnowHOWAttribute;
                        cur_op += 2;
                        break;
                    case MVM_OP_iscoderef:
                        GET_REG(cur_op, 0).i64 = !GET_REG(cur_op, 2).o ||
                            STABLE(GET_REG(cur_op, 2).o)->invoke == MVM_6model_invoke_default ? 0 : 1;
                        cur_op += 4;
                        break;
                    case MVM_OP_null:
                        GET_REG(cur_op, 0).o = NULL;
                        cur_op += 2;
                        break;
                    case MVM_OP_clone: {
                        MVMObject *cloned, *value = GET_REG(cur_op, 2).o;
                        
                        MVM_gc_root_temp_push(tc, (MVMCollectable **)&value);
                        cloned = REPR(value)->allocate(tc, STABLE(value));
                        REPR(value)->copy_to(tc, STABLE(value), OBJECT_BODY(value), cloned, OBJECT_BODY(cloned));
                        MVM_gc_root_temp_pop(tc);
                        
                        GET_REG(cur_op, 0).o = cloned;
                        cur_op += 4;
                        break;
                    }
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_object, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* IO operations. */
            case MVM_OP_BANK_io: {
                switch (*(cur_op++)) {
                    case MVM_OP_copy_f:
                        MVM_file_copy(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_append_f:
                        MVM_file_copy(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_rename_f:
                        MVM_file_copy(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_delete_f:
                        MVM_file_delete(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_chmod_f:
                        MVM_file_chmod(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    case MVM_OP_exists_f:
                        GET_REG(cur_op, 0).i64 = MVM_file_exists(tc, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_mkdir:
                        MVM_dir_mkdir(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_rmdir:
                        MVM_dir_rmdir(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_open_dir:
                        GET_REG(cur_op, 0).o = MVM_dir_open(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    case MVM_OP_read_dir:
                        GET_REG(cur_op, 0).s = MVM_dir_read(tc, GET_REG(cur_op, 2).o);
                        cur_op += 4;
                        break;
                    case MVM_OP_close_dir:
                        MVM_dir_close(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_open_fh:
                        GET_REG(cur_op, 0).o = MVM_file_open_fh(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                        cur_op += 10;
                        break;
                    case MVM_OP_close_fh:
                        MVM_file_close_fh(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_read_fhs:
                        GET_REG(cur_op, 0).s = MVM_file_read_fhs(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_slurp:
                        GET_REG(cur_op, 0).s = MVM_file_slurp(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64);
                        cur_op += 8;
                        break;
                    case MVM_OP_spew:
                        MVM_file_spew(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_write_fhs:
                        GET_REG(cur_op, 9).i64 = MVM_file_write_fhs(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                        cur_op += 10;
                        break;
                    case MVM_OP_seek_fh:
                        MVM_file_seek(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64,
                            GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_lock_fh:
                        GET_REG(cur_op, 0).i64 = MVM_file_lock(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_unlock_fh:
                        MVM_file_unlock(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_flush_fh:
                        MVM_file_flush(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_sync_fh:
                        MVM_file_sync(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_pipe_fh:
                        MVM_file_pipe(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).o);
                        cur_op += 4;
                        break;
                    case MVM_OP_trunc_fh:
                        MVM_file_truncate(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    case MVM_OP_eof_fh:
                        GET_REG(cur_op, 0).i64 = MVM_file_eof(tc, GET_REG(cur_op, 2).o);
                        cur_op += 4;
                        break;
                    case MVM_OP_getstdin:
                        GET_REG(cur_op, 0).o = MVM_file_get_stdin(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_getstdout:
                        GET_REG(cur_op, 0).o = MVM_file_get_stdout(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_getstderr:
                        GET_REG(cur_op, 0).o = MVM_file_get_stderr(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_connect_sk:
                        GET_REG(cur_op, 0).o = MVM_socket_connect(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).i64);
                        cur_op += 12;
                        break;
                    case MVM_OP_close_sk:
                        MVM_socket_close(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    case MVM_OP_bind_sk:
                        GET_REG(cur_op, 0).o = MVM_socket_bind(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).s, GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64, GET_REG(cur_op, 10).i64);
                        cur_op += 12;
                        break;
                    case MVM_OP_listen_sk:
                        MVM_socket_listen(tc, GET_REG(cur_op, 0).o, GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    case MVM_OP_accept_sk:
                        GET_REG(cur_op, 0).o = MVM_socket_accept(tc, GET_REG(cur_op, 2).o);
                        cur_op += 4;
                        break;
                    case MVM_OP_send_sks:
                        GET_REG(cur_op, 0).i64 = MVM_socket_send_string(tc, GET_REG(cur_op, 2).o, GET_REG(cur_op, 4).s,
                            GET_REG(cur_op, 6).i64, GET_REG(cur_op, 8).i64);
                        cur_op += 10;
                        break;
                    case MVM_OP_recv_sks:
                        GET_REG(cur_op, 0).s = MVM_socket_receive_string(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).i64);
                        cur_op += 6;
                        break;
                    case MVM_OP_hostname:
                        GET_REG(cur_op, 0).s = MVM_socket_hostname(tc);
                        cur_op += 2;
                        break;
                    default: {
                        MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_object, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* Process-wide and thread operations. */
            case MVM_OP_BANK_processthread: {
                switch (*(cur_op++)) {
                    case MVM_OP_getenv:
                        GET_REG(cur_op, 0).s = MVM_proc_getenv(tc, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_setenv:
                        MVM_proc_setenv(tc, GET_REG(cur_op, 0).s, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_delenv:
                        MVM_proc_delenv(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_nametogid:
                        GET_REG(cur_op, 0).i64 = MVM_proc_nametogid(tc, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_gidtoname:
                        GET_REG(cur_op, 0).s = MVM_proc_gidtoname(tc, GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    case MVM_OP_nametouid:
                        GET_REG(cur_op, 0).i64 = MVM_proc_nametouid(tc, GET_REG(cur_op, 2).s);
                        cur_op += 4;
                        break;
                    case MVM_OP_uidtoname:
                        GET_REG(cur_op, 0).s = MVM_proc_uidtoname(tc, GET_REG(cur_op, 2).i64);
                        cur_op += 4;
                        break;
                    case MVM_OP_getusername:
                        GET_REG(cur_op, 0).s = MVM_proc_getusername(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_getuid:
                        GET_REG(cur_op, 0).i64 = MVM_proc_getuid(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_getgid:
                        GET_REG(cur_op, 0).i64 = MVM_proc_getgid(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_gethomedir:
                        GET_REG(cur_op, 0).s = MVM_proc_gethomedir(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_chdir:
                        MVM_dir_chdir(tc, GET_REG(cur_op, 0).s);
                        cur_op += 2;
                        break;
                    case MVM_OP_rand_i:
                        GET_REG(cur_op, 0).i64 = MVM_proc_rand_i(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_rand_n:
                        GET_REG(cur_op, 0).n64 = MVM_proc_rand_n(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_time_i:
                        GET_REG(cur_op, 0).i64 = MVM_proc_time_i(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_clargs:
                        GET_REG(cur_op, 0).o = MVM_proc_clargs(tc);
                        cur_op += 2;
                        break;
                    case MVM_OP_newthread:
                        GET_REG(cur_op, 0).o = MVM_thread_start(tc, GET_REG(cur_op, 2).o,
                            GET_REG(cur_op, 4).o);
                        cur_op += 6;
                        break;
                    case MVM_OP_jointhread:
                        MVM_thread_join(tc, GET_REG(cur_op, 0).o);
                        cur_op += 2;
                        break;
                    default: {
                        MVM_panic(13, "Invalid opcode executed (corrupt bytecode stream?) bank %u opcode %u",
                                MVM_OP_BANK_object, *(cur_op-1));
                    }
                    break;
                }
            }
            break;
            
            /* Dispatch to bank function. */
            default:
            {
                MVM_panic(MVM_exitcode_invalidopcode, "Invalid opcode executed (corrupt bytecode stream?) bank %u", *(cur_op-1));
            }
            break;
        }
    }
}
