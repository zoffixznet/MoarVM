struct MVMJitTileTemplate {
    void (*emit)(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                 MVMint32 node, MVMJitExprValue **values, MVMJitExprNode *args);
    const MVMint8 *path;
    const char    *expr;
    MVMint32  left_sym;
    MVMint32 right_sym;

    MVMint32  num_vals;
    MVMint32  regs;
    MVMJitExprVtype vtype;
};

struct MVMJitTile {
    const MVMJitTileTemplate *template;
    void (*emit)(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitExprTree *tree,
                 MVMint32 node, MVMJitExprValue **values, MVMJitExprNode *args);
    MVMJitTile *next;
    MVMint32 order_nr;
    MVMint32 node;
    MVMint32 num_vals;
    /* buffers for the args of this (pseudo) tile */
    MVMJitExprValue *values[8];
    MVMJitExprNode args[8];
};

struct MVMJitTileList {
    MVMJitExprTree *tree;
    MVMJitTile *first;
    MVMJitTile *last;
};

MVMJitTileList * MVM_jit_tile_expr_tree(MVMThreadContext *tc, MVMJitExprTree *tree);
void MVM_jit_tile_get_values(MVMThreadContext *tc, MVMJitExprTree *tree, MVMint32 node,
                             const MVMint8 *path, MVMint32 regs,
                             MVMJitExprValue **values, MVMJitExprNode *args);
