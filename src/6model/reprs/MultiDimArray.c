#include "moar.h"

/* This representation's function pointer table. */
static const MVMREPROps this_repr;

/* Computes the flat number of elements from the given dimension list. */
static MVMint64 flat_elements(MVMint64 num_dimensions, MVMint64 *dimensions) {
    MVMint64 result = dimensions[0];
    MVMint64 i;
    for (i = 1; i < num_dimensions; i++)
        result *= dimensions[i];
    return result;
}

/* Computes the flat size from representation data. */
static size_t flat_size(MVMMultiDimArrayREPRData *repr_data, MVMint64 *dimensions) {
    return repr_data->elem_size * flat_elements(repr_data->num_dimensions, dimensions);
}

/* Creates a new type object of this representation, and associates it with
 * the given HOW. */
static MVMObject * type_object_for(MVMThreadContext *tc, MVMObject *HOW) {
    MVMSTable *st  = MVM_gc_allocate_stable(tc, &this_repr, HOW);

    MVMROOT(tc, st, {
        MVMObject *obj = MVM_gc_allocate_type_object(tc, st);
        MVM_ASSIGN_REF(tc, &(st->header), st->WHAT, obj);
        st->size = sizeof(MVMMultiDimArray);
    });

    return st->WHAT;
}

/* Allocates the mutli-dimensional array and sets up its dimensions array with
 * all zeroes, for later filling. */
static MVMObject * allocate(MVMThreadContext *tc, MVMSTable *st) {
    MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)st->REPR_data;
    if (repr_data) {
        MVMObject *obj = MVM_gc_allocate_object(tc, st);
        ((MVMMultiDimArray *)obj)->body.dimensions = MVM_fixed_size_alloc_zeroed(tc,
            tc->instance->fsa, repr_data->num_dimensions * sizeof(MVMint64));
        return obj;
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "Cannot allocate a multi-dim array type before it is composed");
    }
}

/* Composes the representation. */
static void spec_to_repr_data(MVMThreadContext *tc, MVMMultiDimArrayREPRData *repr_data, const MVMStorageSpec *spec) {
    switch (spec->boxed_primitive) {
        case MVM_STORAGE_SPEC_BP_INT:
            if (spec->is_unsigned) {
                switch (spec->bits) {
                    case 64:
                        repr_data->slot_type = MVM_ARRAY_U64;
                        repr_data->elem_size = sizeof(MVMuint64);
                        break;
                    case 32:
                        repr_data->slot_type = MVM_ARRAY_U32;
                        repr_data->elem_size = sizeof(MVMuint32);
                        break;
                    case 16:
                        repr_data->slot_type = MVM_ARRAY_U16;
                        repr_data->elem_size = sizeof(MVMuint16);
                        break;
                    case 8:
                        repr_data->slot_type = MVM_ARRAY_U8;
                        repr_data->elem_size = sizeof(MVMuint8);
                        break;
                    case 4:
                        repr_data->slot_type = MVM_ARRAY_U4;
                        repr_data->elem_size = 0;
                        break;
                    case 2:
                        repr_data->slot_type = MVM_ARRAY_U2;
                        repr_data->elem_size = 0;
                        break;
                    case 1:
                        repr_data->slot_type = MVM_ARRAY_U1;
                        repr_data->elem_size = 0;
                        break;
                    default:
                        MVM_exception_throw_adhoc(tc,
                            "MVMMultiDimArray: Unsupported uint size");
                }
            }
            else {
                switch (spec->bits) {
                    case 64:
                        repr_data->slot_type = MVM_ARRAY_I64;
                        repr_data->elem_size = sizeof(MVMint64);
                        break;
                    case 32:
                        repr_data->slot_type = MVM_ARRAY_I32;
                        repr_data->elem_size = sizeof(MVMint32);
                        break;
                    case 16:
                        repr_data->slot_type = MVM_ARRAY_I16;
                        repr_data->elem_size = sizeof(MVMint16);
                        break;
                    case 8:
                        repr_data->slot_type = MVM_ARRAY_I8;
                        repr_data->elem_size = sizeof(MVMint8);
                        break;
                    case 4:
                        repr_data->slot_type = MVM_ARRAY_I4;
                        repr_data->elem_size = 0;
                        break;
                    case 2:
                        repr_data->slot_type = MVM_ARRAY_I2;
                        repr_data->elem_size = 0;
                        break;
                    case 1:
                        repr_data->slot_type = MVM_ARRAY_I1;
                        repr_data->elem_size = 0;
                        break;
                    default:
                        MVM_exception_throw_adhoc(tc,
                            "MVMMultiDimArray: Unsupported int size");
                }
            }
            break;
        case MVM_STORAGE_SPEC_BP_NUM:
            switch (spec->bits) {
                case 64:
                    repr_data->slot_type = MVM_ARRAY_N64;
                    repr_data->elem_size = sizeof(MVMnum64);
                    break;
                case 32:
                    repr_data->slot_type = MVM_ARRAY_N32;
                    repr_data->elem_size = sizeof(MVMnum32);
                    break;
                default:
                    MVM_exception_throw_adhoc(tc,
                        "MVMMultiDimArray: Unsupported num size");
            }
            break;
        case MVM_STORAGE_SPEC_BP_STR:
            repr_data->slot_type = MVM_ARRAY_STR;
            repr_data->elem_size = sizeof(MVMString *);
            break;
    }
}
static void compose(MVMThreadContext *tc, MVMSTable *st, MVMObject *repr_info) {
    MVMStringConsts          *str_consts = &(tc->instance->str_consts);
    MVMMultiDimArrayREPRData *repr_data;

    MVMObject *info = MVM_repr_at_key_o(tc, repr_info, str_consts->array);
    if (!MVM_is_null(tc, info)) {
        MVMObject *dims = MVM_repr_at_key_o(tc, info, str_consts->dimensions);
        MVMObject *type = MVM_repr_at_key_o(tc, info, str_consts->type);
        MVMint64 dimensions;
        if (!MVM_is_null(tc, dims)) {
            dimensions = MVM_repr_get_int(tc, dims);
            if (dimensions < 1)
                MVM_exception_throw_adhoc(tc,
                    "MultiDimArray REPR must be composed with at least 1 dimension");
            repr_data = MVM_calloc(1, sizeof(MVMMultiDimArrayREPRData));
            repr_data->num_dimensions = dimensions;
        }
        else {
            MVM_exception_throw_adhoc(tc,
                "MultiDimArray REPR must be composed with a number of dimensions");
        }
        if (!MVM_is_null(tc, type)) {
            const MVMStorageSpec *spec = REPR(type)->get_storage_spec(tc, STABLE(type));
            MVM_ASSIGN_REF(tc, &(st->header), repr_data->elem_type, type);
            spec_to_repr_data(tc, repr_data, spec);
        }
        st->REPR_data = repr_data;
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "MultiDimArray REPR must be composed with array information");
    }
}

/* Copies to the body of one object to another. */
static void copy_to(MVMThreadContext *tc, MVMSTable *st, void *src, MVMObject *dest_root, void *dest) {
    MVM_exception_throw_adhoc(tc,
        "MultiDimArray REPR copy_to NYI");
}

/* Adds held objects to the GC worklist. */
static void gc_mark(MVMThreadContext *tc, MVMSTable *st, void *data, MVMGCWorklist *worklist) {
    MVMMultiDimArrayBody *body = (MVMMultiDimArrayBody *)data;
    if (body->slots.any) {
        MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)st->REPR_data;
        MVMint64 flat_elems = flat_elements(repr_data->num_dimensions, body->dimensions);
        MVMint64 i;
        switch (repr_data->slot_type) {
            case MVM_ARRAY_OBJ: {
                MVMObject **slots = body->slots.o;
                for (i = 0; i < flat_elems; i++)
                    MVM_gc_worklist_add(tc, worklist, &slots[i]);
                break;
            }
            case MVM_ARRAY_STR: {
                MVMString **slots = body->slots.s;
                for (i = 0; i < flat_elems; i++)
                    MVM_gc_worklist_add(tc, worklist, &slots[i]);
                break;
            }
        }
    }
}

/* Called by the VM in order to free memory associated with this object. */
static void gc_free(MVMThreadContext *tc, MVMObject *obj) {
	MVMMultiDimArray *arr = (MVMMultiDimArray *)obj;
    MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)STABLE(obj)->REPR_data;
    if (arr->body.slots.any)
        MVM_fixed_size_free(tc, tc->instance->fsa,
            flat_size(repr_data, arr->body.dimensions),
            arr->body.slots.any);
    MVM_fixed_size_free(tc, tc->instance->fsa,
        repr_data->num_dimensions * sizeof(MVMint64),
        arr->body.dimensions);
}

/* Marks the representation data in an STable.*/
static void gc_mark_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMGCWorklist *worklist) {
    MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)st->REPR_data;
    if (repr_data == NULL)
        return;
    MVM_gc_worklist_add(tc, worklist, &repr_data->elem_type);
}

/* Free representation data. */
static void gc_free_repr_data(MVMThreadContext *tc, MVMSTable *st) {
    MVM_free(st->REPR_data);
}

/* Gets the storage specification for this representation. */
static const MVMStorageSpec storage_spec = {
    MVM_STORAGE_SPEC_REFERENCE, /* inlineable */
    0,                          /* bits */
    0,                          /* align */
    MVM_STORAGE_SPEC_BP_NONE,   /* boxed_primitive */
    0,                          /* can_box */
    0,                          /* is_unsigned */
};
static const MVMStorageSpec * get_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    return &storage_spec;
}

/* Deserializes the data held in the array. */
static void deserialize(MVMThreadContext *tc, MVMSTable *st, MVMObject *root, void *data, MVMSerializationReader *reader) {
}

/* Serializes the data held in the array. */
static void serialize(MVMThreadContext *tc, MVMSTable *st, void *data, MVMSerializationWriter *writer) {
}

/* Serializes the REPR data. */
static void serialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationWriter *writer) {
    /* XXX */
}

/* Deserializes the REPR data. */
static void deserialize_repr_data(MVMThreadContext *tc, MVMSTable *st, MVMSerializationReader *reader) {
    /* XXX */
}

static void deserialize_stable_size(MVMThreadContext *tc, MVMSTable *st, MVMSerializationReader *reader) {
    st->size = sizeof(MVMMultiDimArray);
}

static void dimensions(MVMThreadContext *tc, MVMSTable *st, MVMObject *root, void *data, MVMint64 *num_dimensions, MVMint64 **dimensions) {
    MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)st->REPR_data;
    if (repr_data) {
        MVMMultiDimArrayBody *body = (MVMMultiDimArrayBody *)data;
        *num_dimensions = repr_data->num_dimensions;
        *dimensions = body->dimensions;
    }
    else {
        MVM_exception_throw_adhoc(tc,
            "Cannot query a multi-dim array's dimensionality before it is composed");
    }
}

static MVMStorageSpec get_elem_storage_spec(MVMThreadContext *tc, MVMSTable *st) {
    MVMMultiDimArrayREPRData *repr_data = (MVMMultiDimArrayREPRData *)st->REPR_data;
    MVMStorageSpec spec;
    switch (repr_data->slot_type) {
        case MVM_ARRAY_STR:
            spec.inlineable      = MVM_STORAGE_SPEC_INLINED;
            spec.boxed_primitive = MVM_STORAGE_SPEC_BP_STR;
            spec.can_box         = MVM_STORAGE_SPEC_CAN_BOX_STR;
            break;
        case MVM_ARRAY_I64:
        case MVM_ARRAY_I32:
        case MVM_ARRAY_I16:
        case MVM_ARRAY_I8:
            spec.inlineable      = MVM_STORAGE_SPEC_INLINED;
            spec.boxed_primitive = MVM_STORAGE_SPEC_BP_INT;
            spec.can_box         = MVM_STORAGE_SPEC_CAN_BOX_INT;
            break;
        case MVM_ARRAY_N64:
        case MVM_ARRAY_N32:
            spec.inlineable      = MVM_STORAGE_SPEC_INLINED;
            spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NUM;
            spec.can_box         = MVM_STORAGE_SPEC_CAN_BOX_NUM;
            break;
        case MVM_ARRAY_U64:
        case MVM_ARRAY_U32:
        case MVM_ARRAY_U16:
        case MVM_ARRAY_U8:
            spec.inlineable      = MVM_STORAGE_SPEC_INLINED;
            spec.boxed_primitive = MVM_STORAGE_SPEC_BP_INT;
            spec.can_box         = MVM_STORAGE_SPEC_CAN_BOX_INT;
            spec.is_unsigned     = 1;
            break;
        default:
            spec.inlineable      = MVM_STORAGE_SPEC_REFERENCE;
            spec.boxed_primitive = MVM_STORAGE_SPEC_BP_NONE;
            spec.can_box         = 0;
            break;
    }
    return spec;
}

/* Initializes the representation. */
const MVMREPROps * MVMMultiDimArray_initialize(MVMThreadContext *tc) {
    return &this_repr;
}

static const MVMREPROps this_repr = {
    type_object_for,
    allocate,
    NULL, /* initialize */
    copy_to,
    MVM_REPR_DEFAULT_ATTR_FUNCS,
    MVM_REPR_DEFAULT_BOX_FUNCS,
    {
        MVM_REPR_DEFAULT_AT_POS,
        MVM_REPR_DEFAULT_BIND_POS,
        MVM_REPR_DEFAULT_SET_ELEMS,
        MVM_REPR_DEFAULT_PUSH,
        MVM_REPR_DEFAULT_POP,
        MVM_REPR_DEFAULT_UNSHIFT,
        MVM_REPR_DEFAULT_SHIFT,
        MVM_REPR_DEFAULT_SPLICE,
        MVM_REPR_DEFAULT_AT_POS_MULTIDIM,
        MVM_REPR_DEFAULT_BIND_POS_MULTIDIM,
        dimensions,
        MVM_REPR_DEFAULT_SET_DIMENSIONS,
        get_elem_storage_spec
    },
    MVM_REPR_DEFAULT_ASS_FUNCS,
    MVM_REPR_DEFAULT_ELEMS,
    get_storage_spec,
    NULL, /* change_type */
    serialize,
    deserialize,
    serialize_repr_data,
    deserialize_repr_data,
    deserialize_stable_size,
    gc_mark,
    gc_free,
    NULL, /* gc_cleanup */
    gc_mark_repr_data,
    gc_free_repr_data,
    compose,
    NULL, /* spesh */
    "MultiDimArray", /* name */
    MVM_REPR_ID_MultiDimArray,
    0, /* refs_frames */
};