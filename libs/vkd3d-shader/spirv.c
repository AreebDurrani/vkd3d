/*
 * Copyright 2017 Józef Kucia for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "vkd3d_shader_private.h"
#include "rbtree.h"

#include <stdarg.h>
#include <stdio.h>
#include "spirv/1.0/spirv.h"
#include "spirv/1.0/GLSL.std.450.h"
#ifdef HAVE_SPIRV_TOOLS
# include "spirv-tools/libspirv.h"
#endif /* HAVE_SPIRV_TOOLS */

#ifdef HAVE_SPIRV_TOOLS

static void vkd3d_spirv_dump(const struct vkd3d_shader_code *spirv)
{
    const static uint32_t options
            = SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES | SPV_BINARY_TO_TEXT_OPTION_INDENT;
    spv_diagnostic diagnostic = NULL;
    spv_text text = NULL;
    spv_context context;
    spv_result_t ret;

    context = spvContextCreate(SPV_ENV_VULKAN_1_0);

    if (!(ret = spvBinaryToText(context, spirv->code, spirv->size / sizeof(uint32_t),
            options, &text, &diagnostic)))
    {
        const char *str, *current = text->str;
        while ((str = strchr(current, '\n')))
        {
            TRACE("%.*s\n", (int)(str - current), current);
            current = str + 1;
        }
    }
    else
    {
        FIXME("Failed to convert SPIR-V binary to text, ret %d.\n", ret);
        FIXME("Diagnostic message: %s.\n", debugstr_a(diagnostic->error));
    }

    spvTextDestroy(text);
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
}

static void vkd3d_spirv_validate(const struct vkd3d_shader_code *spirv)
{
    spv_diagnostic diagnostic = NULL;
    spv_context context;
    spv_result_t ret;

    context = spvContextCreate(SPV_ENV_VULKAN_1_0);

    if ((ret = spvValidateBinary(context, spirv->code, spirv->size / sizeof(uint32_t),
            &diagnostic)))
    {
        FIXME("Failed to validate SPIR-V binary, ret %d.\n", ret);
        FIXME("Diagnostic message: %s.\n", debugstr_a(diagnostic->error));
    }

    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
}

#else

static void vkd3d_spirv_dump(const struct vkd3d_shader_code *spirv) {}
static void vkd3d_spirv_validate(const struct vkd3d_shader_code *spirv) {}

#endif /* HAVE_SPIRV_TOOLS */

struct vkd3d_spirv_stream
{
    uint32_t *words;
    size_t capacity;
    size_t word_count;
};

static void vkd3d_spirv_stream_init(struct vkd3d_spirv_stream *stream)
{
    stream->capacity = 256;
    if (!(stream->words = vkd3d_calloc(stream->capacity, sizeof(*stream->words))))
        stream->capacity = 0;
    stream->word_count = 0;
}

static void vkd3d_spirv_stream_free(struct vkd3d_spirv_stream *stream)
{
    vkd3d_free(stream->words);
}

static void vkd3d_spirv_stream_clear(struct vkd3d_spirv_stream *stream)
{
    stream->word_count = 0;
}

static bool vkd3d_spirv_stream_append(struct vkd3d_spirv_stream *dst_stream,
        const struct vkd3d_spirv_stream *src_stream)
{
    if (!vkd3d_array_reserve((void **)&dst_stream->words, &dst_stream->capacity,
            dst_stream->word_count + src_stream->word_count, sizeof(*dst_stream->words)))
        return false;

    assert(dst_stream->word_count + src_stream->word_count <= dst_stream->capacity);
    memcpy(&dst_stream->words[dst_stream->word_count], src_stream->words,
            src_stream->word_count * sizeof(*src_stream->words));
    dst_stream->word_count += src_stream->word_count;
    return true;
}

struct vkd3d_spirv_builder
{
    uint64_t capability_mask;
    uint32_t ext_instr_set_glsl_450;
    SpvExecutionModel execution_model;

    uint32_t current_id;
    uint32_t main_function_id;
    struct rb_tree declarations;
    uint32_t type_sampler_id;
    uint32_t type_bool_id;
    uint32_t type_void_id;

    struct vkd3d_spirv_stream debug_stream; /* debug instructions */
    struct vkd3d_spirv_stream annotation_stream; /* decoration instructions */
    struct vkd3d_spirv_stream global_stream; /* types, constants, global variables */
    struct vkd3d_spirv_stream function_stream; /* function definitions */

    union
    {
        struct
        {
            uint32_t local_size[3];
        } compute;
    } u;

    /* entry point interface */
    uint32_t *iface;
    size_t iface_capacity;
    size_t iface_element_count;
};

static uint32_t vkd3d_spirv_alloc_id(struct vkd3d_spirv_builder *builder)
{
    return builder->current_id++;
}

static void vkd3d_spirv_enable_capability(struct vkd3d_spirv_builder *builder,
        SpvCapability cap)
{
    assert(cap < sizeof(builder->capability_mask) * CHAR_BIT);
    builder->capability_mask |= 1ull << cap;
}

static uint32_t vkd3d_spirv_get_glsl_std450_instr_set(struct vkd3d_spirv_builder *builder)
{
    if (!builder->ext_instr_set_glsl_450)
        builder->ext_instr_set_glsl_450 = vkd3d_spirv_alloc_id(builder);

    return builder->ext_instr_set_glsl_450;
}

static void vkd3d_spirv_add_iface_variable(struct vkd3d_spirv_builder *builder,
        uint32_t id)
{
    if (!vkd3d_array_reserve((void **)&builder->iface, &builder->iface_capacity,
            builder->iface_element_count + 1, sizeof(*builder->iface)))
        return;

    builder->iface[builder->iface_element_count++] = id;
}

static void vkd3d_spirv_set_execution_model(struct vkd3d_spirv_builder *builder,
        SpvExecutionModel model)
{
    builder->execution_model = model;

    switch (model)
    {
        case SpvExecutionModelVertex:
        case SpvExecutionModelFragment:
        case SpvExecutionModelGLCompute:
            vkd3d_spirv_enable_capability(builder, SpvCapabilityShader);
            break;
        case SpvExecutionModelTessellationControl:
        case SpvExecutionModelTessellationEvaluation:
            vkd3d_spirv_enable_capability(builder, SpvCapabilityTessellation);
            break;
        case SpvExecutionModelGeometry:
            vkd3d_spirv_enable_capability(builder, SpvCapabilityGeometry);
            break;
        default:
            ERR("Unhandled execution model %#x.\n", model);
    }
}

static void vkd3d_spirv_set_local_size(struct vkd3d_spirv_builder *builder,
        unsigned int x, unsigned int y, unsigned int z)
{
    assert(builder->execution_model == SpvExecutionModelGLCompute);

    builder->u.compute.local_size[0] = x;
    builder->u.compute.local_size[1] = y;
    builder->u.compute.local_size[2] = z;
}

static uint32_t vkd3d_spirv_opcode_word(SpvOp op, unsigned int word_count)
{
    assert(!(op & ~SpvOpCodeMask));
    return (word_count << SpvWordCountShift) | op;
}

static void vkd3d_spirv_build_word(struct vkd3d_spirv_stream *stream, uint32_t word)
{
    if (!vkd3d_array_reserve((void **)&stream->words, &stream->capacity,
            stream->word_count + 1, sizeof(*stream->words)))
        return;

    stream->words[stream->word_count++] = word;
}

static unsigned int vkd3d_spirv_string_word_count(const char *str)
{
    return align(strlen(str) + 1, sizeof(uint32_t)) / sizeof(uint32_t);
}

static void vkd3d_spirv_build_string(struct vkd3d_spirv_stream *stream,
        const char *str, unsigned int word_count)
{
    unsigned int word_idx, i;
    const char *ptr = str;

    for (word_idx = 0; word_idx < word_count; ++word_idx)
    {
        uint32_t word = 0;
        for (i = 0; i < sizeof(uint32_t) && *ptr; ++i)
            word |= (uint32_t)*ptr++ << (8 * i);
        vkd3d_spirv_build_word(stream, word);
    }
}

typedef uint32_t (*vkd3d_spirv_build_pfn)(struct vkd3d_spirv_builder *builder);
typedef uint32_t (*vkd3d_spirv_build1_pfn)(struct vkd3d_spirv_builder *builder,
        uint32_t operand0);
typedef uint32_t (*vkd3d_spirv_build1v_pfn)(struct vkd3d_spirv_builder *builder,
        uint32_t operand0, const uint32_t *operands, unsigned int operand_count);
typedef uint32_t (*vkd3d_spirv_build2_pfn)(struct vkd3d_spirv_builder *builder,
        uint32_t operand0, uint32_t operand1);
typedef uint32_t (*vkd3d_spirv_build7_pfn)(struct vkd3d_spirv_builder *builder,
        uint32_t operand0, uint32_t operand1, uint32_t operand2, uint32_t operand3,
        uint32_t operand4, uint32_t operand5, uint32_t operand6);

static uint32_t vkd3d_spirv_build_once(struct vkd3d_spirv_builder *builder,
        uint32_t *id, vkd3d_spirv_build_pfn build_pfn)
{
    if (!(*id))
        *id = build_pfn(builder);
    return *id;
}

#define MAX_SPIRV_DECLARATION_PARAMETER_COUNT 7

struct vkd3d_spirv_declaration
{
    struct rb_entry entry;

    SpvOp op;
    unsigned int parameter_count;
    uint32_t parameters[MAX_SPIRV_DECLARATION_PARAMETER_COUNT];
    uint32_t id;
};

static int vkd3d_spirv_declaration_compare(const void *key, const struct rb_entry *e)
{
    const struct vkd3d_spirv_declaration *a = key;
    const struct vkd3d_spirv_declaration *b = RB_ENTRY_VALUE(e, const struct vkd3d_spirv_declaration, entry);

    if (a->op != b->op)
        return a->op - b->op;
    if (a->parameter_count != b->parameter_count)
        return a->parameter_count - b->parameter_count;
    assert(a->parameter_count <= ARRAY_SIZE(a->parameters));
    return memcmp(&a->parameters, &b->parameters, a->parameter_count * sizeof(*a->parameters));
}

static void vkd3d_spirv_declaration_free(struct rb_entry *entry, void *context)
{
    struct vkd3d_spirv_declaration *d = RB_ENTRY_VALUE(entry, struct vkd3d_spirv_declaration, entry);

    vkd3d_free(d);
}

static void vkd3d_spirv_insert_declaration(struct vkd3d_spirv_builder *builder,
        const struct vkd3d_spirv_declaration *declaration)
{
    struct vkd3d_spirv_declaration *d;

    assert(declaration->parameter_count <= ARRAY_SIZE(declaration->parameters));

    if (!(d = vkd3d_malloc(sizeof(*d))))
        return;
    memcpy(d, declaration, sizeof(*d));
    if (rb_put(&builder->declarations, d, &d->entry) == -1)
    {
        ERR("Failed to insert declaration entry.\n");
        vkd3d_free(d);
    }
}

static uint32_t vkd3d_spirv_build_once1(struct vkd3d_spirv_builder *builder,
        SpvOp op, uint32_t operand0, vkd3d_spirv_build1_pfn build_pfn)
{
    struct vkd3d_spirv_declaration declaration;
    struct rb_entry *entry;

    declaration.op = op;
    declaration.parameter_count = 1;
    declaration.parameters[0] = operand0;

    if ((entry = rb_get(&builder->declarations, &declaration)))
        return RB_ENTRY_VALUE(entry, struct vkd3d_spirv_declaration, entry)->id;

    declaration.id = build_pfn(builder, operand0);
    vkd3d_spirv_insert_declaration(builder, &declaration);
    return declaration.id;
}

static uint32_t vkd3d_spirv_build_once1v(struct vkd3d_spirv_builder *builder,
        SpvOp op, uint32_t operand0, const uint32_t *operands, unsigned int operand_count,
        vkd3d_spirv_build1v_pfn build_pfn)
{
    struct vkd3d_spirv_declaration declaration;
    unsigned int i, param_idx = 0;
    struct rb_entry *entry;

    declaration.op = op;
    declaration.parameters[param_idx++] = operand0;
    for (i = 0; i < operand_count; ++i)
        declaration.parameters[param_idx++] = operands[i];
    declaration.parameter_count = param_idx;

    if ((entry = rb_get(&builder->declarations, &declaration)))
        return RB_ENTRY_VALUE(entry, struct vkd3d_spirv_declaration, entry)->id;

    declaration.id = build_pfn(builder, operand0, operands, operand_count);
    vkd3d_spirv_insert_declaration(builder, &declaration);
    return declaration.id;
}

static uint32_t vkd3d_spirv_build_once2(struct vkd3d_spirv_builder *builder,
        SpvOp op, uint32_t operand0, uint32_t operand1, vkd3d_spirv_build2_pfn build_pfn)
{
    struct vkd3d_spirv_declaration declaration;
    struct rb_entry *entry;

    declaration.op = op;
    declaration.parameter_count = 2;
    declaration.parameters[0] = operand0;
    declaration.parameters[1] = operand1;

    if ((entry = rb_get(&builder->declarations, &declaration)))
        return RB_ENTRY_VALUE(entry, struct vkd3d_spirv_declaration, entry)->id;

    declaration.id = build_pfn(builder, operand0, operand1);
    vkd3d_spirv_insert_declaration(builder, &declaration);
    return declaration.id;
}

static uint32_t vkd3d_spirv_build_once7(struct vkd3d_spirv_builder *builder,
        SpvOp op, const uint32_t *operands, vkd3d_spirv_build7_pfn build_pfn)
{
    struct vkd3d_spirv_declaration declaration;
    struct rb_entry *entry;

    declaration.op = op;
    declaration.parameter_count = 7;
    memcpy(&declaration.parameters, operands, declaration.parameter_count * sizeof(*operands));

    if ((entry = rb_get(&builder->declarations, &declaration)))
        return RB_ENTRY_VALUE(entry, struct vkd3d_spirv_declaration, entry)->id;

    declaration.id = build_pfn(builder, operands[0], operands[1], operands[2],
            operands[3], operands[4], operands[5], operands[6]);
    vkd3d_spirv_insert_declaration(builder, &declaration);
    return declaration.id;
}

/*
 * vkd3d_spirv_build_op[1-3][v]()
 * vkd3d_spirv_build_op_[t][r][1-3][v]()
 *
 * t   - result type
 * r   - result id
 * 1-3 - the number of operands
 * v   - variable number of operands
 */
static void vkd3d_spirv_build_op(struct vkd3d_spirv_stream *stream, SpvOp op)
{
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 1));
}

static void vkd3d_spirv_build_op1(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand)
{
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 2));
    vkd3d_spirv_build_word(stream, operand);
}

static void vkd3d_spirv_build_op1v(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand0, const uint32_t *operands, unsigned int operand_count)
{
    unsigned int i;
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 2 + operand_count));
    vkd3d_spirv_build_word(stream, operand0);
    for (i = 0; i < operand_count; ++i)
        vkd3d_spirv_build_word(stream, operands[i]);
}

static void vkd3d_spirv_build_op2v(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand0, uint32_t operand1,
        const uint32_t *operands, unsigned int operand_count)
{
    unsigned int i;
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 3 + operand_count));
    vkd3d_spirv_build_word(stream, operand0);
    vkd3d_spirv_build_word(stream, operand1);
    for (i = 0; i < operand_count; ++i)
        vkd3d_spirv_build_word(stream, operands[i]);
}

static void vkd3d_spirv_build_op3v(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand0, uint32_t operand1, uint32_t operand2,
        const uint32_t *operands, unsigned int operand_count)
{
    unsigned int i;
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 4 + operand_count));
    vkd3d_spirv_build_word(stream, operand0);
    vkd3d_spirv_build_word(stream, operand1);
    vkd3d_spirv_build_word(stream, operand2);
    for (i = 0; i < operand_count; ++i)
        vkd3d_spirv_build_word(stream, operands[i]);
}

static void vkd3d_spirv_build_op2(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op2v(stream, op, operand0, operand1, NULL, 0);
}

static void vkd3d_spirv_build_op3(struct vkd3d_spirv_stream *stream,
        SpvOp op, uint32_t operand0, uint32_t operand1, uint32_t operand2)
{
    return vkd3d_spirv_build_op2v(stream, op, operand0, operand1, &operand2, 1);
}

static uint32_t vkd3d_spirv_build_op_rv(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op,
        const uint32_t *operands, unsigned int operand_count)
{
    uint32_t result_id = vkd3d_spirv_alloc_id(builder);
    vkd3d_spirv_build_op1v(stream, op, result_id, operands, operand_count);
    return result_id;
}

static uint32_t vkd3d_spirv_build_op_r(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op)
{
    return vkd3d_spirv_build_op_rv(builder, stream, op, NULL, 0);
}

static uint32_t vkd3d_spirv_build_op_r1(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t operand0)
{
    return vkd3d_spirv_build_op_rv(builder, stream, op, &operand0, 1);
}

static uint32_t vkd3d_spirv_build_op_r2(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t operand0, uint32_t operand1)
{
    uint32_t operands[] = {operand0, operand1};
    return vkd3d_spirv_build_op_rv(builder, stream, op, operands, ARRAY_SIZE(operands));
}

static uint32_t vkd3d_spirv_build_op_r1v(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t operand0,
        const uint32_t *operands, unsigned int operand_count)
{
    uint32_t result_id = vkd3d_spirv_alloc_id(builder);
    vkd3d_spirv_build_op2v(stream, op, result_id, operand0, operands, operand_count);
    return result_id;
}

static uint32_t vkd3d_spirv_build_op_trv(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type,
        const uint32_t *operands, unsigned int operand_count)
{
    uint32_t result_id = vkd3d_spirv_alloc_id(builder);
    vkd3d_spirv_build_op2v(stream, op, result_type, result_id, operands, operand_count);
    return result_id;
}

static uint32_t vkd3d_spirv_build_op_tr(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type)
{
    return vkd3d_spirv_build_op_trv(builder, stream, op, result_type, NULL, 0);
}

static uint32_t vkd3d_spirv_build_op_tr1(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type,
        uint32_t operand0)
{
    return vkd3d_spirv_build_op_trv(builder, stream, op, result_type, &operand0, 1);
}

static uint32_t vkd3d_spirv_build_op_tr2(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type,
        uint32_t operand0, uint32_t operand1)
{
    uint32_t operands[] = {operand0, operand1};
    return vkd3d_spirv_build_op_trv(builder, stream, op, result_type,
            operands, ARRAY_SIZE(operands));
}

static uint32_t vkd3d_spirv_build_op_tr1v(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type,
        uint32_t operand0, const uint32_t *operands, unsigned int operand_count)
{
    uint32_t result_id = vkd3d_spirv_alloc_id(builder);
    vkd3d_spirv_build_op3v(stream, op, result_type, result_id, operand0, operands, operand_count);
    return result_id;
}

static uint32_t vkd3d_spirv_build_op_tr2v(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, SpvOp op, uint32_t result_type,
        uint32_t operand0, uint32_t operand1, const uint32_t *operands, unsigned int operand_count)
{
    uint32_t result_id = vkd3d_spirv_alloc_id(builder);
    unsigned int i;
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(op, 5 + operand_count));
    vkd3d_spirv_build_word(stream, result_type);
    vkd3d_spirv_build_word(stream, result_id);
    vkd3d_spirv_build_word(stream, operand0);
    vkd3d_spirv_build_word(stream, operand1);
    for (i = 0; i < operand_count; ++i)
        vkd3d_spirv_build_word(stream, operands[i]);
    return result_id;
}

static void vkd3d_spirv_build_op_capability(struct vkd3d_spirv_stream *stream,
        SpvCapability cap)
{
    vkd3d_spirv_build_op1(stream, SpvOpCapability, cap);
}

static void vkd3d_spirv_build_op_ext_inst_import(struct vkd3d_spirv_stream *stream,
        uint32_t result_id, const char *name)
{
    unsigned int name_size = vkd3d_spirv_string_word_count(name);
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(SpvOpExtInstImport, 2 + name_size));
    vkd3d_spirv_build_word(stream, result_id);
    vkd3d_spirv_build_string(stream, name, name_size);
}

static uint32_t vkd3d_spirv_build_op_ext_inst(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t inst_set, uint32_t inst_number,
        uint32_t *operands, unsigned int operand_count)
{
    return vkd3d_spirv_build_op_tr2v(builder, &builder->function_stream,
            SpvOpExtInst, result_type, inst_set, inst_number, operands, operand_count);
}

static void vkd3d_spirv_build_op_memory_model(struct vkd3d_spirv_stream *stream,
        SpvAddressingModel addressing_model, SpvMemoryModel memory_model)
{
    vkd3d_spirv_build_op2(stream, SpvOpMemoryModel, addressing_model, memory_model);
}

static void vkd3d_spirv_build_op_entry_point(struct vkd3d_spirv_stream *stream,
        SpvExecutionModel model, uint32_t function_id, const char *name,
        uint32_t *interface_list, unsigned int interface_size)
{
    unsigned int i, name_size = vkd3d_spirv_string_word_count(name);
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(SpvOpEntryPoint, 3 + name_size + interface_size));
    vkd3d_spirv_build_word(stream, model);
    vkd3d_spirv_build_word(stream, function_id);
    vkd3d_spirv_build_string(stream, name, name_size);
    for (i = 0; i < interface_size; ++i)
        vkd3d_spirv_build_word(stream, interface_list[i]);
}

static void vkd3d_spirv_build_op_execution_mode(struct vkd3d_spirv_stream *stream,
        uint32_t entry_point, SpvExecutionMode mode, uint32_t *literals, unsigned int literal_count)
{
    vkd3d_spirv_build_op2v(stream, SpvOpExecutionMode, entry_point, mode, literals, literal_count);
}

static void vkd3d_spirv_build_op_name(struct vkd3d_spirv_builder *builder,
        uint32_t id, const char *fmt, ...)
{
    struct vkd3d_spirv_stream *stream = &builder->debug_stream;
    unsigned int name_size;
    char name[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(name, ARRAY_SIZE(name), fmt, args);
    name[ARRAY_SIZE(name) - 1] = '\0';
    va_end(args);

    name_size = vkd3d_spirv_string_word_count(name);
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(SpvOpName, 2 + name_size));
    vkd3d_spirv_build_word(stream, id);
    vkd3d_spirv_build_string(stream, name, name_size);
}

static void vkd3d_spirv_build_op_member_name(struct vkd3d_spirv_builder *builder,
        uint32_t type_id, uint32_t member, const char *fmt, ...)
{
    struct vkd3d_spirv_stream *stream = &builder->debug_stream;
    unsigned int name_size;
    char name[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(name, ARRAY_SIZE(name), fmt, args);
    name[ARRAY_SIZE(name) - 1] = '\0';
    va_end(args);

    name_size = vkd3d_spirv_string_word_count(name);
    vkd3d_spirv_build_word(stream, vkd3d_spirv_opcode_word(SpvOpMemberName, 3 + name_size));
    vkd3d_spirv_build_word(stream, type_id);
    vkd3d_spirv_build_word(stream, member);
    vkd3d_spirv_build_string(stream, name, name_size);
}

static void vkd3d_spirv_build_op_decorate(struct vkd3d_spirv_builder *builder,
        uint32_t target_id, SpvDecoration decoration,
        uint32_t *literals, uint32_t literal_count)
{
    vkd3d_spirv_build_op2v(&builder->annotation_stream,
            SpvOpDecorate, target_id, decoration, literals, literal_count);
}

static void vkd3d_spirv_build_op_decorate1(struct vkd3d_spirv_builder *builder,
        uint32_t target_id, SpvDecoration decoration, uint32_t operand0)
{
    return vkd3d_spirv_build_op_decorate(builder, target_id, decoration, &operand0, 1);
}

static void vkd3d_spirv_build_op_member_decorate(struct vkd3d_spirv_builder *builder,
        uint32_t structure_type_id, uint32_t member_idx, SpvDecoration decoration,
        uint32_t *literals, uint32_t literal_count)
{
    vkd3d_spirv_build_op3v(&builder->annotation_stream, SpvOpMemberDecorate,
            structure_type_id, member_idx, decoration, literals, literal_count);
}

static void vkd3d_spirv_build_op_member_decorate1(struct vkd3d_spirv_builder *builder,
        uint32_t structure_type_id, uint32_t member_idx, SpvDecoration decoration, uint32_t operand0)
{
    vkd3d_spirv_build_op_member_decorate(builder, structure_type_id, member_idx, decoration, &operand0, 1);
}

static uint32_t vkd3d_spirv_build_op_type_void(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_op_r(builder, &builder->global_stream, SpvOpTypeVoid);
}

static uint32_t vkd3d_spirv_get_op_type_void(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_once(builder, &builder->type_void_id, vkd3d_spirv_build_op_type_void);
}

static uint32_t vkd3d_spirv_build_op_type_bool(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_op_r(builder, &builder->global_stream, SpvOpTypeBool);
}

static uint32_t vkd3d_spirv_get_op_type_bool(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_once(builder, &builder->type_bool_id, vkd3d_spirv_build_op_type_bool);
}

static uint32_t vkd3d_spirv_build_op_type_float(struct vkd3d_spirv_builder *builder,
        uint32_t width)
{
    return vkd3d_spirv_build_op_r1(builder, &builder->global_stream, SpvOpTypeFloat, width);
}

static uint32_t vkd3d_spirv_get_op_type_float(struct vkd3d_spirv_builder *builder,
        uint32_t width)
{
    return vkd3d_spirv_build_once1(builder, SpvOpTypeFloat, width, vkd3d_spirv_build_op_type_float);
}

static uint32_t vkd3d_spirv_build_op_type_int(struct vkd3d_spirv_builder *builder,
        uint32_t width, uint32_t signedness)
{
    return vkd3d_spirv_build_op_r2(builder, &builder->global_stream, SpvOpTypeInt, width, signedness);
}

static uint32_t vkd3d_spirv_get_op_type_int(struct vkd3d_spirv_builder *builder,
        uint32_t width, uint32_t signedness)
{
    return vkd3d_spirv_build_once2(builder, SpvOpTypeInt, width, signedness,
            vkd3d_spirv_build_op_type_int);
}

static uint32_t vkd3d_spirv_build_op_type_vector(struct vkd3d_spirv_builder *builder,
        uint32_t component_type, uint32_t component_count)
{
    return vkd3d_spirv_build_op_r2(builder, &builder->global_stream,
            SpvOpTypeVector, component_type, component_count);
}

static uint32_t vkd3d_spirv_get_op_type_vector(struct vkd3d_spirv_builder *builder,
        uint32_t component_type, uint32_t component_count)
{
    return vkd3d_spirv_build_once2(builder, SpvOpTypeVector, component_type, component_count,
            vkd3d_spirv_build_op_type_vector);
}

static uint32_t vkd3d_spirv_build_op_type_array(struct vkd3d_spirv_builder *builder,
        uint32_t element_type, uint32_t length_id)
{
    return vkd3d_spirv_build_op_r2(builder, &builder->global_stream,
            SpvOpTypeArray, element_type, length_id);
}

static uint32_t vkd3d_spirv_build_op_type_struct(struct vkd3d_spirv_builder *builder,
        uint32_t *members, unsigned int member_count)
{
    return vkd3d_spirv_build_op_rv(builder, &builder->global_stream,
            SpvOpTypeStruct, members, member_count);
}

static uint32_t vkd3d_spirv_build_op_type_sampler(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_op_r(builder, &builder->global_stream, SpvOpTypeSampler);
}

static uint32_t vkd3d_spirv_get_op_type_sampler(struct vkd3d_spirv_builder *builder)
{
    return vkd3d_spirv_build_once(builder, &builder->type_sampler_id, vkd3d_spirv_build_op_type_sampler);
}

/* Access qualifiers are not supported. */
static uint32_t vkd3d_spirv_build_op_type_image(struct vkd3d_spirv_builder *builder,
        uint32_t sampled_type_id, SpvDim dim, uint32_t depth, uint32_t arrayed,
        uint32_t ms, uint32_t sampled, SpvImageFormat format)
{
    uint32_t operands[] = {sampled_type_id, dim, depth, arrayed, ms, sampled, format};
    return vkd3d_spirv_build_op_rv(builder, &builder->global_stream,
            SpvOpTypeImage, operands, ARRAY_SIZE(operands));
}

static uint32_t vkd3d_spirv_get_op_type_image(struct vkd3d_spirv_builder *builder,
        uint32_t sampled_type_id, SpvDim dim, uint32_t depth, uint32_t arrayed,
        uint32_t ms, uint32_t sampled, SpvImageFormat format)
{
    uint32_t operands[] = {sampled_type_id, dim, depth, arrayed, ms, sampled, format};
    return vkd3d_spirv_build_once7(builder, SpvOpTypeImage, operands,
            vkd3d_spirv_build_op_type_image);
}

static uint32_t vkd3d_spirv_build_op_type_sampled_image(struct vkd3d_spirv_builder *builder,
        uint32_t image_type_id)
{
    return vkd3d_spirv_build_op_r1(builder, &builder->global_stream,
            SpvOpTypeSampledImage, image_type_id);
}

static uint32_t vkd3d_spirv_get_op_type_sampled_image(struct vkd3d_spirv_builder *builder,
        uint32_t image_type_id)
{
    return vkd3d_spirv_build_once1(builder, SpvOpTypeSampledImage, image_type_id,
            vkd3d_spirv_build_op_type_sampled_image);
}

static uint32_t vkd3d_spirv_build_op_type_function(struct vkd3d_spirv_builder *builder,
        uint32_t return_type, uint32_t *param_types, unsigned int param_count)
{
    return vkd3d_spirv_build_op_r1v(builder, &builder->global_stream,
            SpvOpTypeFunction, return_type, param_types, param_count);
}

static uint32_t vkd3d_spirv_build_op_type_pointer(struct vkd3d_spirv_builder *builder,
        uint32_t storage_class, uint32_t type_id)
{
    return vkd3d_spirv_build_op_r2(builder, &builder->global_stream,
            SpvOpTypePointer, storage_class, type_id);
}

static uint32_t vkd3d_spirv_get_op_type_pointer(struct vkd3d_spirv_builder *builder,
        uint32_t storage_class, uint32_t type_id)
{
    return vkd3d_spirv_build_once2(builder, SpvOpTypePointer, storage_class, type_id,
            vkd3d_spirv_build_op_type_pointer);
}

/* Types larger than 32-bits are not supported. */
static uint32_t vkd3d_spirv_build_op_constant(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t value)
{
    return vkd3d_spirv_build_op_tr1(builder, &builder->global_stream,
            SpvOpConstant, result_type, value);
}

static uint32_t vkd3d_spirv_get_op_constant(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t value)
{
    return vkd3d_spirv_build_once2(builder, SpvOpConstant, result_type, value,
            vkd3d_spirv_build_op_constant);
}

static uint32_t vkd3d_spirv_build_op_constant_composite(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, const uint32_t *constituents, unsigned int constituent_count)
{
    return vkd3d_spirv_build_op_trv(builder, &builder->global_stream,
            SpvOpConstantComposite, result_type, constituents, constituent_count);
}

static uint32_t vkd3d_spirv_get_op_constant_composite(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, const uint32_t *constituents, unsigned int constituent_count)
{
    return vkd3d_spirv_build_once1v(builder, SpvOpConstantComposite, result_type,
            constituents, constituent_count, vkd3d_spirv_build_op_constant_composite);
}

static uint32_t vkd3d_spirv_build_op_variable(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, uint32_t type_id, uint32_t storage_class, uint32_t initializer)
{
    return vkd3d_spirv_build_op_tr1v(builder, stream,
            SpvOpVariable, type_id, storage_class, &initializer, !!initializer);
}

static uint32_t vkd3d_spirv_build_op_function(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t result_id, uint32_t function_control, uint32_t function_type)
{
    vkd3d_spirv_build_op3v(&builder->function_stream,
            SpvOpFunction, result_type, result_id, function_control, &function_type, 1);
    return result_id;
}

static uint32_t vkd3d_spirv_build_op_function_parameter(struct vkd3d_spirv_builder *builder,
        uint32_t result_type)
{
    return vkd3d_spirv_build_op_tr(builder, &builder->function_stream,
            SpvOpFunctionParameter, result_type);
}

static void vkd3d_spirv_build_op_function_end(struct vkd3d_spirv_builder *builder)
{
    vkd3d_spirv_build_op(&builder->function_stream, SpvOpFunctionEnd);
}

static uint32_t vkd3d_spirv_build_op_function_call(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t function_id, const uint32_t *arguments, unsigned int argument_count)
{
    return vkd3d_spirv_build_op_tr1v(builder, &builder->function_stream,
            SpvOpFunctionCall, result_type, function_id, arguments, argument_count);
}

static uint32_t vkd3d_spirv_build_op_undef(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream, uint32_t type_id)
{
    return vkd3d_spirv_build_op_tr(builder, stream, SpvOpUndef, type_id);
}

static uint32_t vkd3d_spirv_build_op_access_chain(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t base_id, uint32_t *indexes, uint32_t index_count)
{
    return vkd3d_spirv_build_op_tr1v(builder, &builder->function_stream,
            SpvOpAccessChain, result_type, base_id, indexes, index_count);
}

static uint32_t vkd3d_spirv_build_op_in_bounds_access_chain(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t base_id, uint32_t *indexes, uint32_t index_count)
{
    return vkd3d_spirv_build_op_tr1v(builder, &builder->function_stream,
            SpvOpInBoundsAccessChain, result_type, base_id, indexes, index_count);
}

static uint32_t vkd3d_spirv_build_op_vector_shuffle(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t vector1_id, uint32_t vector2_id,
        const uint32_t *components, uint32_t component_count)
{
    return vkd3d_spirv_build_op_tr2v(builder, &builder->function_stream, SpvOpVectorShuffle,
            result_type, vector1_id, vector2_id, components, component_count);
}

static uint32_t vkd3d_spirv_build_op_composite_construct(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, const uint32_t *constituents, unsigned int constituent_count)
{
    return vkd3d_spirv_build_op_trv(builder, &builder->function_stream, SpvOpCompositeConstruct,
            result_type, constituents, constituent_count);
}

static uint32_t vkd3d_spirv_build_op_composite_extract(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t composite_id, const uint32_t *indexes, unsigned int index_count)
{
    return vkd3d_spirv_build_op_tr1v(builder, &builder->function_stream, SpvOpCompositeExtract,
            result_type, composite_id, indexes, index_count);
}

static uint32_t vkd3d_spirv_build_op_load(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t pointer_id, uint32_t memory_access)
{
    if (!memory_access)
        return vkd3d_spirv_build_op_tr1(builder, &builder->function_stream, SpvOpLoad,
                result_type, pointer_id);
    else
        return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream, SpvOpLoad,
                result_type, pointer_id, memory_access);
}

static void vkd3d_spirv_build_op_store(struct vkd3d_spirv_builder *builder,
        uint32_t pointer_id, uint32_t object_id, uint32_t memory_access)
{
    if (!memory_access)
        return vkd3d_spirv_build_op2(&builder->function_stream, SpvOpStore,
                pointer_id, object_id);
    else
        return vkd3d_spirv_build_op3(&builder->function_stream, SpvOpStore,
                pointer_id, object_id, memory_access);
}

static uint32_t vkd3d_spirv_build_op_select(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t condition_id, uint32_t object0_id, uint32_t object1_id)
{
    uint32_t operands[] = {condition_id, object0_id, object1_id};
    return vkd3d_spirv_build_op_trv(builder, &builder->function_stream, SpvOpSelect,
            result_type, operands, ARRAY_SIZE(operands));
}

static void vkd3d_spirv_build_op_return(struct vkd3d_spirv_builder *builder)
{
    vkd3d_spirv_build_op(&builder->function_stream, SpvOpReturn);
}

static uint32_t vkd3d_spirv_build_op_label(struct vkd3d_spirv_builder *builder,
        uint32_t label_id)
{
    vkd3d_spirv_build_op1(&builder->function_stream, SpvOpLabel, label_id);
    return label_id;
}

/* Loop control parameters are not supported. */
static void vkd3d_spirv_build_op_loop_merge(struct vkd3d_spirv_builder *builder,
        uint32_t merge_block, uint32_t continue_target, SpvLoopControlMask loop_control)
{
    vkd3d_spirv_build_op3(&builder->function_stream, SpvOpLoopMerge,
            merge_block, continue_target, loop_control);
}

static void vkd3d_spirv_build_op_selection_merge(struct vkd3d_spirv_builder *builder,
        uint32_t merge_block, uint32_t selection_control)
{
    vkd3d_spirv_build_op2(&builder->function_stream, SpvOpSelectionMerge,
            merge_block, selection_control);
}

static void vkd3d_spirv_build_op_branch(struct vkd3d_spirv_builder *builder, uint32_t label)
{
    vkd3d_spirv_build_op1(&builder->function_stream, SpvOpBranch, label);
}

/* Branch weights are not supported. */
static void vkd3d_spirv_build_op_branch_conditional(struct vkd3d_spirv_builder *builder,
        uint32_t condition, uint32_t true_label, uint32_t false_label)
{
    vkd3d_spirv_build_op3(&builder->function_stream, SpvOpBranchConditional,
            condition, true_label, false_label);
}

static uint32_t vkd3d_spirv_build_op_iadd(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpIAdd, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_imul(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpIMul, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_udiv(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpUDiv, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_umod(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpUMod, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_isub(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpISub, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_fdiv(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpFDiv, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_fnegate(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand)
{
    return vkd3d_spirv_build_op_tr1(builder, &builder->function_stream,
            SpvOpFNegate, result_type, operand);
}

static uint32_t vkd3d_spirv_build_op_snegate(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand)
{
    return vkd3d_spirv_build_op_tr1(builder, &builder->function_stream,
            SpvOpSNegate, result_type, operand);
}

static uint32_t vkd3d_spirv_build_op_and(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand0, uint32_t operand1)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpBitwiseAnd, result_type, operand0, operand1);
}

static uint32_t vkd3d_spirv_build_op_bitcast(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand)
{
    return vkd3d_spirv_build_op_tr1(builder, &builder->function_stream,
            SpvOpBitcast, result_type, operand);
}

static uint32_t vkd3d_spirv_build_op_sampled_image(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t image_id, uint32_t sampler_id)
{
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpSampledImage, result_type, image_id, sampler_id);
}

static uint32_t vkd3d_spirv_build_op_image_sample_implicit_lod(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t sampled_image_id, uint32_t coordinate_id,
        uint32_t image_operands, const uint32_t *operands, unsigned int operand_count)
{
    unsigned int i = 0, j;
    uint32_t w[10];

    w[i++] = sampled_image_id;
    w[i++] = coordinate_id;
    if (image_operands)
    {
        assert(i + 1 + operand_count <= ARRAY_SIZE(w));
        w[i++] = image_operands;
        for (j = 0; j < operand_count; ++j)
            w[i++] = operands[j];
    }
    return vkd3d_spirv_build_op_trv(builder, &builder->function_stream,
            SpvOpImageSampleImplicitLod, result_type, w, i);
}

static void vkd3d_spirv_build_op_image_write(struct vkd3d_spirv_builder *builder,
        uint32_t image_id, uint32_t coordinate_id, uint32_t texel_id,
        uint32_t image_operands, const uint32_t *operands, unsigned int operand_count)
{
    if (image_operands)
        FIXME("Image operands not supported.\n");

    vkd3d_spirv_build_op3(&builder->function_stream, SpvOpImageWrite,
            image_id, coordinate_id, texel_id);
}

static uint32_t vkd3d_spirv_build_op_glsl_std450_fabs(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t operand)
{
    uint32_t glsl_std450_id = vkd3d_spirv_get_glsl_std450_instr_set(builder);
    return vkd3d_spirv_build_op_ext_inst(builder, result_type, glsl_std450_id,
            GLSLstd450FAbs, &operand, 1);
}

static uint32_t vkd3d_spirv_build_op_glsl_std450_nclamp(struct vkd3d_spirv_builder *builder,
        uint32_t result_type, uint32_t x, uint32_t min, uint32_t max)
{
    uint32_t glsl_std450_id = vkd3d_spirv_get_glsl_std450_instr_set(builder);
    uint32_t operands[] = {x, min, max};
    return vkd3d_spirv_build_op_ext_inst(builder, result_type, glsl_std450_id,
            GLSLstd450FClamp, operands, ARRAY_SIZE(operands));
}

static uint32_t vkd3d_spirv_get_type_id(struct vkd3d_spirv_builder *builder,
        enum vkd3d_component_type component_type, unsigned int component_count)
{
    uint32_t scalar_id;

    if (component_count == 1)
    {
        switch (component_type)
        {
            case VKD3D_TYPE_VOID:
                return vkd3d_spirv_get_op_type_void(builder);
                break;
            case VKD3D_TYPE_FLOAT:
                return vkd3d_spirv_get_op_type_float(builder, 32);
                break;
            case VKD3D_TYPE_INT:
            case VKD3D_TYPE_UINT:
                return vkd3d_spirv_get_op_type_int(builder, 32, component_type == VKD3D_TYPE_INT);
                break;
            case VKD3D_TYPE_BOOL:
                return vkd3d_spirv_get_op_type_bool(builder);
                break;
            default:
                FIXME("Unhandled component type %#x.\n", component_type);
                return 0;
        }
    }
    else
    {
        assert(component_type != VKD3D_TYPE_VOID);
        scalar_id = vkd3d_spirv_get_type_id(builder, component_type, 1);
        return vkd3d_spirv_get_op_type_vector(builder, scalar_id, component_count);
    }
}

static void vkd3d_spirv_builder_init(struct vkd3d_spirv_builder *builder)
{
    uint32_t void_id, function_type_id;

    vkd3d_spirv_stream_init(&builder->debug_stream);
    vkd3d_spirv_stream_init(&builder->annotation_stream);
    vkd3d_spirv_stream_init(&builder->global_stream);
    vkd3d_spirv_stream_init(&builder->function_stream);

    builder->current_id = 1;

    rb_init(&builder->declarations, vkd3d_spirv_declaration_compare);

    void_id = vkd3d_spirv_get_op_type_void(builder);
    function_type_id = vkd3d_spirv_build_op_type_function(builder, void_id, NULL, 0);

    builder->main_function_id = vkd3d_spirv_build_op_function(builder, void_id,
            vkd3d_spirv_alloc_id(builder), SpvFunctionControlMaskNone, function_type_id);
    vkd3d_spirv_build_op_name(builder, builder->main_function_id, "main");
    vkd3d_spirv_build_op_label(builder, vkd3d_spirv_alloc_id(builder));
}

static void vkd3d_spirv_builder_free(struct vkd3d_spirv_builder *builder)
{
    vkd3d_spirv_stream_free(&builder->debug_stream);
    vkd3d_spirv_stream_free(&builder->annotation_stream);
    vkd3d_spirv_stream_free(&builder->global_stream);
    vkd3d_spirv_stream_free(&builder->function_stream);

    rb_destroy(&builder->declarations, vkd3d_spirv_declaration_free, NULL);

    vkd3d_free(builder->iface);
}

static void vkd3d_spirv_build_execution_mode_declarations(struct vkd3d_spirv_builder *builder,
        struct vkd3d_spirv_stream *stream)
{
    if (builder->execution_model == SpvExecutionModelGLCompute)
    {
        vkd3d_spirv_build_op_execution_mode(stream, builder->main_function_id, SpvExecutionModeLocalSize,
                builder->u.compute.local_size, ARRAY_SIZE(builder->u.compute.local_size));
    }
}

static bool vkd3d_spirv_compile_module(struct vkd3d_spirv_builder *builder,
        struct vkd3d_shader_code *spirv)
{
    uint64_t capability_mask = builder->capability_mask;
    struct vkd3d_spirv_stream stream;
    uint32_t *code;
    unsigned int i;
    size_t size;

    vkd3d_spirv_stream_init(&stream);

    vkd3d_spirv_build_word(&stream, SpvMagicNumber);
    vkd3d_spirv_build_word(&stream, SpvVersion);
    vkd3d_spirv_build_word(&stream, 0); /* generator */
    vkd3d_spirv_build_word(&stream, builder->current_id); /* bound */
    vkd3d_spirv_build_word(&stream, 0); /* schema, reserved */

    for (i = 0; capability_mask; ++i)
    {
        if (capability_mask & 1)
            vkd3d_spirv_build_op_capability(&stream, i);
        capability_mask >>= 1;
    }

    if (builder->ext_instr_set_glsl_450)
        vkd3d_spirv_build_op_ext_inst_import(&stream, builder->ext_instr_set_glsl_450, "GLSL.std.450");

    vkd3d_spirv_build_op_memory_model(&stream, SpvAddressingModelLogical, SpvMemoryModelGLSL450);
    vkd3d_spirv_build_op_entry_point(&stream, builder->execution_model, builder->main_function_id,
            "main", builder->iface, builder->iface_element_count);

    vkd3d_spirv_build_execution_mode_declarations(builder, &stream);

    vkd3d_spirv_stream_append(&stream, &builder->debug_stream);
    vkd3d_spirv_stream_append(&stream, &builder->annotation_stream);
    vkd3d_spirv_stream_append(&stream, &builder->global_stream);
    vkd3d_spirv_stream_append(&stream, &builder->function_stream);

    if (!(code = vkd3d_calloc(stream.word_count, sizeof(*code))))
    {
        vkd3d_spirv_stream_free(&stream);
        return false;
    }

    size = stream.word_count * sizeof(*code);
    memcpy(code, stream.words, size);
    vkd3d_spirv_stream_free(&stream);

    spirv->code = code;
    spirv->size = size;

    return true;
}

struct vkd3d_symbol_register
{
    enum vkd3d_shader_register_type type;
    unsigned int idx;
};

struct vkd3d_symbol_resource
{
    enum vkd3d_shader_register_type type;
    unsigned int idx;
};

struct vkd3d_symbol
{
    struct rb_entry entry;

    enum
    {
        VKD3D_SYMBOL_REGISTER,
        VKD3D_SYMBOL_RESOURCE,
    } type;

    union
    {
        struct vkd3d_symbol_register reg;
        struct vkd3d_symbol_resource resource;
    } key;

    uint32_t id;
    union
    {
        SpvStorageClass storage_class;
        struct
        {
            enum vkd3d_component_type sampled_type;
            uint32_t type_id;
            DWORD coordinate_mask;
        } resource;
    } info;
};

static int vkd3d_symbol_compare(const void *key, const struct rb_entry *entry)
{
    const struct vkd3d_symbol *a = key;
    const struct vkd3d_symbol *b = RB_ENTRY_VALUE(entry, const struct vkd3d_symbol, entry);

    if (a->type != b->type)
        return a->type - b->type;
    return memcmp(&a->key, &b->key, sizeof(a->key));
}

static void vkd3d_symbol_free(struct rb_entry *entry, void *context)
{
    struct vkd3d_symbol *s = RB_ENTRY_VALUE(entry, struct vkd3d_symbol, entry);

    vkd3d_free(s);
}

static void vkd3d_symbol_make_register(struct vkd3d_symbol *symbol,
        const struct vkd3d_shader_register *reg)
{
    symbol->type = VKD3D_SYMBOL_REGISTER;
    memset(&symbol->key, 0, sizeof(symbol->key));
    symbol->key.reg.type = reg->type;
    if (reg->type != VKD3DSPR_IMMCONSTBUFFER)
        symbol->key.reg.idx = reg->idx[0].offset;
}

static void vkd3d_symbol_make_resource(struct vkd3d_symbol *symbol,
        const struct vkd3d_shader_register *reg)
{
    symbol->type = VKD3D_SYMBOL_RESOURCE;
    memset(&symbol->key, 0, sizeof(symbol->key));
    symbol->key.resource.type = reg->type;
    symbol->key.resource.idx = reg->idx[0].offset;
}

static struct vkd3d_symbol *vkd3d_symbol_dup(const struct vkd3d_symbol *symbol)
{
    struct vkd3d_symbol *s;

    if (!(s = vkd3d_malloc(sizeof(*s))))
        return NULL;

    return memcpy(s, symbol, sizeof(*s));
}

struct vkd3d_if_cf_info
{
    uint32_t merge_block_id;
    uint32_t else_block_id;
};

struct vkd3d_loop_cf_info
{
    uint32_t header_block_id;
    uint32_t continue_block_id;
    uint32_t merge_block_id;
};

struct vkd3d_control_flow_info
{
    union
    {
        struct vkd3d_if_cf_info branch;
        struct vkd3d_loop_cf_info loop;
    } u;

    enum
    {
        VKD3D_BLOCK_MAIN,
        VKD3D_BLOCK_IF,
        VKD3D_BLOCK_ELSE,
        VKD3D_BLOCK_LOOP,
        VKD3D_BLOCK_NONE,
    } current_block;
};

struct vkd3d_push_constant_buffer
{
    struct vkd3d_shader_register reg;
    struct vkd3d_shader_push_constant pc;
};

struct vkd3d_dxbc_compiler
{
    struct vkd3d_spirv_builder spirv_builder;

    uint32_t options;

    struct rb_tree symbol_table;
    uint32_t temp_id;
    unsigned int temp_count;

    enum vkd3d_shader_type shader_type;

    unsigned int branch_id;
    unsigned int loop_id;
    unsigned int control_flow_depth;
    struct vkd3d_control_flow_info *control_flow_info;
    size_t control_flow_info_size;

    unsigned int binding_count;
    const struct vkd3d_shader_resource_binding *bindings;
    unsigned int push_constant_count;
    struct vkd3d_push_constant_buffer *push_constants;

    bool after_declarations_section;
    const struct vkd3d_shader_signature *input_signature;
    const struct vkd3d_shader_signature *output_signature;
    struct
    {
        uint32_t id;
        enum vkd3d_component_type component_type;
    } *output_info;
    uint32_t private_output_variable[MAX_REG_OUTPUT];
    uint32_t output_setup_function_id;
};

struct vkd3d_dxbc_compiler *vkd3d_dxbc_compiler_create(const struct vkd3d_shader_version *shader_version,
        const struct vkd3d_shader_desc *shader_desc, uint32_t compiler_options,
        const struct vkd3d_shader_resource_binding *bindings, unsigned int binding_count,
        const struct vkd3d_shader_push_constant *constants, unsigned int constant_count)
{
    const struct vkd3d_shader_signature *output_signature = &shader_desc->output_signature;
    struct vkd3d_dxbc_compiler *compiler;
    unsigned int i;

    if (!(compiler = vkd3d_malloc(sizeof(*compiler))))
        return NULL;

    memset(compiler, 0, sizeof(*compiler));

    if (!(compiler->output_info = vkd3d_calloc(output_signature->element_count, sizeof(*compiler->output_info))))
    {
        vkd3d_free(compiler);
        return NULL;
    }

    vkd3d_spirv_builder_init(&compiler->spirv_builder);
    compiler->options = compiler_options;

    rb_init(&compiler->symbol_table, vkd3d_symbol_compare);

    switch (shader_version->type)
    {
        case VKD3D_SHADER_TYPE_VERTEX:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelVertex);
            break;
        case VKD3D_SHADER_TYPE_HULL:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelTessellationControl);
            break;
        case VKD3D_SHADER_TYPE_DOMAIN:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelTessellationEvaluation);
            break;
        case VKD3D_SHADER_TYPE_GEOMETRY:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelGeometry);
            break;
        case VKD3D_SHADER_TYPE_PIXEL:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelFragment);
            break;
        case VKD3D_SHADER_TYPE_COMPUTE:
            vkd3d_spirv_set_execution_model(&compiler->spirv_builder, SpvExecutionModelGLCompute);
            break;
        default:
            ERR("Invalid shader type %#x.\n", shader_version->type);
    }

    compiler->shader_type = shader_version->type;

    compiler->input_signature = &shader_desc->input_signature;
    compiler->output_signature = &shader_desc->output_signature;

    if (binding_count)
    {
        compiler->binding_count = binding_count;
        compiler->bindings = bindings;
    }

    if (constant_count)
    {
        compiler->push_constant_count = constant_count;
        if (!(compiler->push_constants = vkd3d_calloc(constant_count, sizeof(*compiler->push_constants))))
        {
            vkd3d_dxbc_compiler_destroy(compiler);
            return NULL;
        }
        for (i = 0; i < compiler->push_constant_count; ++i)
            compiler->push_constants[i].pc = constants[i];
    }

    return compiler;
}

static struct vkd3d_push_constant_buffer *vkd3d_dxbc_compiler_find_push_constant(
        struct vkd3d_dxbc_compiler *compiler, const struct vkd3d_shader_register *reg)
{
    unsigned int reg_idx = reg->idx[0].offset;
    unsigned int i;

    for (i = 0; i < compiler->push_constant_count; ++i)
    {
        struct vkd3d_push_constant_buffer *current = &compiler->push_constants[i];

        if (current->pc.register_index == reg_idx)
            return current;
    }

    return NULL;
}

struct vkd3d_descriptor_binding
{
    uint32_t set;
    uint32_t binding;
};

static struct vkd3d_descriptor_binding vkd3d_dxbc_compiler_get_descriptor_binding(
        struct vkd3d_dxbc_compiler *compiler, const struct vkd3d_shader_register *reg)
{
    enum vkd3d_descriptor_type descriptor_type;
    struct vkd3d_descriptor_binding vk_binding;
    unsigned int reg_idx = reg->idx[0].offset;
    unsigned int i;

    descriptor_type = VKD3D_DESCRIPTOR_TYPE_UNKNOWN;
    if (reg->type == VKD3DSPR_CONSTBUFFER)
        descriptor_type = VKD3D_DESCRIPTOR_TYPE_CBV;
    else if (reg->type == VKD3DSPR_RESOURCE)
        descriptor_type = VKD3D_DESCRIPTOR_TYPE_SRV;
    else if (reg->type == VKD3DSPR_UAV)
        descriptor_type = VKD3D_DESCRIPTOR_TYPE_UAV;
    else if (reg->type == VKD3DSPR_SAMPLER)
        descriptor_type = VKD3D_DESCRIPTOR_TYPE_SAMPLER;
    else
        FIXME("Unhandled register type %#x.\n", reg->type);

    if (descriptor_type != VKD3D_DESCRIPTOR_TYPE_UNKNOWN)
    {
        for (i = 0; i < compiler->binding_count; ++i)
        {
            const struct vkd3d_shader_resource_binding *current = &compiler->bindings[i];

            if (current->type == descriptor_type && current->register_index == reg_idx)
            {
                vk_binding.set = current->descriptor_set;
                vk_binding.binding = current->binding;
                return vk_binding;
            }
        }
        if (compiler->binding_count)
            FIXME("Could not find descriptor binding for %#x, %u.\n", descriptor_type, reg_idx);
    }

    vk_binding.set = 0;
    vk_binding.binding = reg_idx;
    return vk_binding;
}

static void vkd3d_dxbc_compiler_emit_descriptor_binding(struct vkd3d_dxbc_compiler *compiler,
        uint32_t variable_id, const struct vkd3d_shader_register *reg)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    struct vkd3d_descriptor_binding vk_binding;

    vk_binding = vkd3d_dxbc_compiler_get_descriptor_binding(compiler, reg);
    vkd3d_spirv_build_op_decorate1(builder, variable_id, SpvDecorationDescriptorSet, vk_binding.set);
    vkd3d_spirv_build_op_decorate1(builder, variable_id, SpvDecorationBinding, vk_binding.binding);
}

static void vkd3d_dxbc_compiler_put_symbol(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_symbol *symbol)
{
    struct vkd3d_symbol *s;

    s = vkd3d_symbol_dup(symbol);
    if (rb_put(&compiler->symbol_table, s, &s->entry) == -1)
    {
        ERR("Failed to insert symbol entry.\n");
        vkd3d_free(s);
    }
}

static uint32_t vkd3d_dxbc_compiler_get_constant(struct vkd3d_dxbc_compiler *compiler,
        enum vkd3d_component_type component_type, unsigned int component_count, const uint32_t *values)
{
    uint32_t type_id, scalar_type_id, component_ids[VKD3D_VEC4_SIZE];
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    unsigned int i;

    assert(0 < component_count && component_count <= VKD3D_VEC4_SIZE);
    type_id = vkd3d_spirv_get_type_id(builder, component_type, component_count);

    switch (component_type)
    {
        case VKD3D_TYPE_UINT:
        case VKD3D_TYPE_INT:
        case VKD3D_TYPE_FLOAT:
            break;
        default:
            FIXME("Unhandled component_type %#x.\n", component_type);
            return vkd3d_spirv_build_op_undef(builder, &builder->global_stream, type_id);
    }

    if (component_count == 1)
    {
        return vkd3d_spirv_get_op_constant(builder, type_id, *values);
    }
    else
    {
        scalar_type_id = vkd3d_spirv_get_type_id(builder, component_type, 1);
        for (i = 0; i < component_count; ++i)
            component_ids[i] = vkd3d_spirv_get_op_constant(builder, scalar_type_id, values[i]);
        return vkd3d_spirv_get_op_constant_composite(builder, type_id, component_ids, component_count);
    }
}

static uint32_t vkd3d_dxbc_compiler_get_constant_uint(struct vkd3d_dxbc_compiler *compiler,
        uint32_t value)
{
    return vkd3d_dxbc_compiler_get_constant(compiler, VKD3D_TYPE_UINT, 1, &value);
}

static uint32_t vkd3d_dxbc_compiler_get_constant_float(struct vkd3d_dxbc_compiler *compiler,
        float value)
{
    return vkd3d_dxbc_compiler_get_constant(compiler, VKD3D_TYPE_FLOAT, 1, (uint32_t *)&value);
}

static bool vkd3d_dxbc_compiler_get_register_name(char *buffer, unsigned int buffer_size,
        const struct vkd3d_shader_register *reg)
{
    switch (reg->type)
    {
        case VKD3DSPR_RESOURCE:
            snprintf(buffer, buffer_size, "t%u", reg->idx[0].offset);
            break;
        case VKD3DSPR_UAV:
            snprintf(buffer, buffer_size, "u%u", reg->idx[0].offset);
            break;
        case VKD3DSPR_SAMPLER:
            snprintf(buffer, buffer_size, "s%u", reg->idx[0].offset);
            break;
        case VKD3DSPR_CONSTBUFFER:
            snprintf(buffer, buffer_size, "cb%u_%u", reg->idx[0].offset, reg->idx[1].offset);
            break;
        case VKD3DSPR_INPUT:
            snprintf(buffer, buffer_size, "v%u", reg->idx[0].offset);
            break;
        case VKD3DSPR_OUTPUT:
        case VKD3DSPR_COLOROUT:
            snprintf(buffer, buffer_size, "o%u", reg->idx[0].offset);
            break;
        case VKD3DSPR_THREADID:
            snprintf(buffer, buffer_size, "vThreadID");
            break;
        case VKD3DSPR_LOCALTHREADID:
            snprintf(buffer, buffer_size, "vThreadIDInGroup");
            break;
        case VKD3DSPR_LOCALTHREADINDEX:
            snprintf(buffer, buffer_size, "vThreadIDInGroupFlattened");
            break;
        case VKD3DSPR_THREADGROUPID:
            snprintf(buffer, buffer_size, "vThreadGroupID");
            break;
        default:
            FIXME("Unhandled register %#x.\n", reg->type);
            snprintf(buffer, buffer_size, "unrecognized_%#x", reg->type);
            return false;
    }

    return true;
}

static void vkd3d_dxbc_compiler_emit_register_debug_name(struct vkd3d_spirv_builder *builder,
        uint32_t id, const struct vkd3d_shader_register *reg)
{
    char debug_name[256];
    if (vkd3d_dxbc_compiler_get_register_name(debug_name, ARRAY_SIZE(debug_name), reg))
        vkd3d_spirv_build_op_name(builder, id, debug_name);
}

static uint32_t vkd3d_dxbc_compiler_emit_variable(struct vkd3d_dxbc_compiler *compiler,
        struct vkd3d_spirv_stream *stream, SpvStorageClass storage_class,
        enum vkd3d_component_type component_type, unsigned int component_count)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, ptr_type_id;

    type_id = vkd3d_spirv_get_type_id(builder, component_type, component_count);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, storage_class, type_id);
    return vkd3d_spirv_build_op_variable(builder, stream, ptr_type_id, storage_class, 0);
}

static uint32_t vkd3d_dxbc_compiler_emit_undef(struct vkd3d_dxbc_compiler *compiler,
        struct vkd3d_spirv_stream *stream, const struct vkd3d_shader_register *reg)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, ptr_type_id;

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, SpvStorageClassPrivate, type_id);
    return vkd3d_spirv_build_op_undef(builder, stream, ptr_type_id);
}

static uint32_t vkd3d_dxbc_compiler_emit_load_src(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_src_param *src, DWORD write_mask);

static uint32_t vkd3d_dxbc_compiler_emit_register_addressing(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register_index *reg_index)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, addr_id;

    if (!reg_index->rel_addr)
        return vkd3d_dxbc_compiler_get_constant_uint(compiler, reg_index->offset);

    addr_id = vkd3d_dxbc_compiler_emit_load_src(compiler, reg_index->rel_addr, VKD3DSP_WRITEMASK_0);
    if (reg_index->offset)
    {
        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_UINT, 1);
        addr_id = vkd3d_spirv_build_op_iadd(builder, type_id,
                addr_id, vkd3d_dxbc_compiler_get_constant_uint(compiler, reg_index->offset));
    }
    return addr_id;
}

struct vkd3d_dxbc_register_info
{
    uint32_t id;
    SpvStorageClass storage_class;
};

static void vkd3d_dxbc_compiler_get_register_info(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, struct vkd3d_dxbc_register_info *register_info)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    struct vkd3d_symbol reg_symbol, *symbol;
    struct rb_entry *entry;

    assert(reg->type != VKD3DSPR_IMMCONST);

    if (reg->idx[0].rel_addr || reg->idx[1].rel_addr)
        FIXME("Relative indexing not implemented.\n");

    if (reg->type == VKD3DSPR_TEMP)
    {
        assert(reg->idx[0].offset < compiler->temp_count);
        register_info->id = compiler->temp_id + reg->idx[0].offset;
        register_info->storage_class = SpvStorageClassFunction;
        return;
    }

    vkd3d_symbol_make_register(&reg_symbol, reg);
    entry = rb_get(&compiler->symbol_table, &reg_symbol);
    assert(entry);
    symbol = RB_ENTRY_VALUE(entry, struct vkd3d_symbol, entry);
    register_info->id = symbol->id;
    register_info->storage_class = symbol->info.storage_class;

    if (reg->type == VKD3DSPR_CONSTBUFFER)
    {
        uint32_t type_id, ptr_type_id;
        uint32_t indexes[] =
        {
            vkd3d_dxbc_compiler_get_constant_uint(compiler, 0),
            vkd3d_dxbc_compiler_get_constant_uint(compiler, reg->idx[1].offset),
        };

        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
        ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, register_info->storage_class, type_id);
        register_info->id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                register_info->id, indexes, ARRAY_SIZE(indexes));
    }
    else if (reg->type == VKD3DSPR_IMMCONSTBUFFER)
    {
        uint32_t indexes[] = {vkd3d_dxbc_compiler_emit_register_addressing(compiler, &reg->idx[0])};
        uint32_t type_id, ptr_type_id;

        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
        ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, register_info->storage_class, type_id);
        register_info->id = vkd3d_spirv_build_op_access_chain(builder, ptr_type_id,
                register_info->id, indexes, ARRAY_SIZE(indexes));
    }
}

static uint32_t vkd3d_dxbc_compiler_get_register_id(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    struct vkd3d_dxbc_register_info register_info;

    switch (reg->type)
    {
        case VKD3DSPR_TEMP:
        case VKD3DSPR_INPUT:
        case VKD3DSPR_OUTPUT:
        case VKD3DSPR_COLOROUT:
        case VKD3DSPR_CONSTBUFFER:
        case VKD3DSPR_IMMCONSTBUFFER:
        case VKD3DSPR_SAMPLER:
        case VKD3DSPR_THREADID:
        case VKD3DSPR_LOCALTHREADID:
        case VKD3DSPR_LOCALTHREADINDEX:
        case VKD3DSPR_THREADGROUPID:
            vkd3d_dxbc_compiler_get_register_info(compiler, reg, &register_info);
            return register_info.id;
        case VKD3DSPR_IMMCONST:
            ERR("Unexpected register type %#x.\n", reg->type);
            return vkd3d_dxbc_compiler_emit_undef(compiler, &builder->global_stream, reg);
        default:
            FIXME("Unhandled register type %#x.\n", reg->type);
            return vkd3d_dxbc_compiler_emit_undef(compiler, &builder->global_stream, reg);
    }
}

static uint32_t vkd3d_dxbc_compiler_emit_swizzle(struct vkd3d_dxbc_compiler *compiler,
        uint32_t val_id, DWORD swizzle, DWORD write_mask)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    unsigned int i, component_idx, component_count;
    uint32_t type_id, components[VKD3D_VEC4_SIZE];

    if (swizzle == VKD3DSP_NOSWIZZLE && write_mask == VKD3DSP_WRITEMASK_ALL)
        return val_id;

    component_count = vkd3d_write_mask_component_count(write_mask);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, component_count);

    if (component_count == 1)
    {
        component_idx = vkd3d_write_mask_get_component_idx(write_mask);
        component_idx = vkd3d_swizzle_get_component(swizzle, component_idx);
        return vkd3d_spirv_build_op_composite_extract(builder, type_id, val_id, &component_idx, 1);
    }

    for (i = 0, component_idx = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (write_mask & (VKD3DSP_WRITEMASK_0 << i))
            components[component_idx++] = vkd3d_swizzle_get_component(swizzle, i);
    }
    return vkd3d_spirv_build_op_vector_shuffle(builder,
            type_id, val_id, val_id, components, component_count);
}

static uint32_t vkd3d_dxbc_compiler_emit_load_constant(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD swizzle, DWORD write_mask)
{
    unsigned int component_count = vkd3d_write_mask_component_count(write_mask);
    uint32_t values[VKD3D_VEC4_SIZE];
    unsigned int i, j;

    assert(reg->type == VKD3DSPR_IMMCONST);

    if (reg->immconst_type == VKD3D_IMMCONST_SCALAR)
    {
        assert(component_count == 1);
        values[0] = *reg->u.immconst_data;
    }
    else
    {
        for (i = 0, j = 0; i < VKD3D_VEC4_SIZE; ++i)
        {
            if (write_mask & (VKD3DSP_WRITEMASK_0 << i))
                values[j++] = reg->u.immconst_data[vkd3d_swizzle_get_component(swizzle, i)];
        }
    }

    return vkd3d_dxbc_compiler_get_constant(compiler,
            vkd3d_component_type_from_data_type(reg->data_type), component_count, values);
}

static uint32_t vkd3d_dxbc_compiler_emit_load_scalar(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD swizzle, DWORD write_mask)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, ptr_type_id, indexes[1], chain_id, val_id;
    struct vkd3d_dxbc_register_info reg_info;
    unsigned int component_idx;

    assert(reg->type != VKD3DSPR_IMMCONST);
    assert(vkd3d_write_mask_component_count(write_mask) == 1);

    component_idx = vkd3d_write_mask_get_component_idx(write_mask);
    component_idx = vkd3d_swizzle_get_component(swizzle, component_idx);

    vkd3d_dxbc_compiler_get_register_info(compiler, reg, &reg_info);

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 1);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, reg_info.storage_class, type_id);
    indexes[0] = vkd3d_dxbc_compiler_get_constant_uint(compiler, component_idx);
    chain_id = vkd3d_spirv_build_op_in_bounds_access_chain(builder,
            ptr_type_id, reg_info.id, indexes, ARRAY_SIZE(indexes));

    val_id = vkd3d_spirv_build_op_load(builder, type_id, chain_id, SpvMemoryAccessMaskNone);

    if (reg->data_type != VKD3D_DATA_FLOAT)
    {
        type_id = vkd3d_spirv_get_type_id(builder,
                vkd3d_component_type_from_data_type(reg->data_type), 1);
        val_id = vkd3d_spirv_build_op_bitcast(builder, type_id, val_id);
    }

    return val_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_load_reg(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD swizzle, DWORD write_mask)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t src_id, type_id, val_id;
    unsigned int component_count;

    if (reg->type == VKD3DSPR_IMMCONST)
        return vkd3d_dxbc_compiler_emit_load_constant(compiler, reg, swizzle, write_mask);

    component_count = vkd3d_write_mask_component_count(write_mask);
    if (component_count == 1)
        return vkd3d_dxbc_compiler_emit_load_scalar(compiler, reg, swizzle, write_mask);

    src_id = vkd3d_dxbc_compiler_get_register_id(compiler, reg);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    val_id = vkd3d_spirv_build_op_load(builder, type_id, src_id, SpvMemoryAccessMaskNone);

    val_id = vkd3d_dxbc_compiler_emit_swizzle(compiler, val_id, swizzle, write_mask);

    if (reg->data_type != VKD3D_DATA_FLOAT)
    {
        type_id = vkd3d_spirv_get_type_id(builder,
                vkd3d_component_type_from_data_type(reg->data_type), component_count);
        val_id = vkd3d_spirv_build_op_bitcast(builder, type_id, val_id);
    }

    return val_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_abs(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask, uint32_t val_id)
{
    unsigned int component_count = vkd3d_write_mask_component_count(write_mask);
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id;

    if (reg->data_type == VKD3D_DATA_FLOAT)
    {
        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, component_count);
        return vkd3d_spirv_build_op_glsl_std450_fabs(builder, type_id, val_id);
    }

    FIXME("Unhandled data type %#x.\n", reg->data_type);
    return val_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_neg(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask, uint32_t val_id)
{
    unsigned int component_count = vkd3d_write_mask_component_count(write_mask);
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id;

    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(reg->data_type), component_count);
    if (reg->data_type == VKD3D_DATA_FLOAT)
        return vkd3d_spirv_build_op_fnegate(builder, type_id, val_id);
    else if (reg->data_type == VKD3D_DATA_INT)
        return vkd3d_spirv_build_op_snegate(builder, type_id, val_id);

    FIXME("Unhandled data type %#x.\n", reg->data_type);
    return val_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_src_modifier(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask,
        enum vkd3d_shader_src_modifier modifier, uint32_t val_id)
{
    switch (modifier)
    {
        case VKD3DSPSM_NONE:
            break;
        case VKD3DSPSM_NEG:
            return vkd3d_dxbc_compiler_emit_neg(compiler, reg, write_mask, val_id);
        case VKD3DSPSM_ABS:
            return vkd3d_dxbc_compiler_emit_abs(compiler, reg, write_mask, val_id);
        case VKD3DSPSM_ABSNEG:
            val_id = vkd3d_dxbc_compiler_emit_abs(compiler, reg, write_mask, val_id);
            return vkd3d_dxbc_compiler_emit_neg(compiler, reg, write_mask, val_id);
        default:
            FIXME("Unhandled src modifier %#x.\n", modifier);
            break;
    }

    return val_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_load_src(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_src_param *src, DWORD write_mask)
{
    uint32_t val_id;

    val_id = vkd3d_dxbc_compiler_emit_load_reg(compiler, &src->reg, src->swizzle, write_mask);
    return vkd3d_dxbc_compiler_emit_src_modifier(compiler, &src->reg, write_mask, src->modifiers, val_id);
}

static void vkd3d_dxbc_compiler_emit_store_scalar(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask, uint32_t val_id)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, ptr_type_id, chain_id, index[1];
    struct vkd3d_dxbc_register_info reg_info;
    unsigned int component_idx;

    assert(reg->type != VKD3DSPR_IMMCONST);

    vkd3d_dxbc_compiler_get_register_info(compiler, reg, &reg_info);

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 1);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, reg_info.storage_class, type_id);
    component_idx = vkd3d_write_mask_get_component_idx(write_mask);
    index[0] = vkd3d_dxbc_compiler_get_constant_uint(compiler, component_idx);
    chain_id = vkd3d_spirv_build_op_in_bounds_access_chain(builder,
            ptr_type_id, reg_info.id, index, ARRAY_SIZE(index));

    vkd3d_spirv_build_op_store(builder, chain_id, val_id, SpvMemoryAccessMaskNone);
}

static void vkd3d_dxbc_compiler_emit_store_reg(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask, uint32_t val_id)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    unsigned int i, component_idx, component_count;
    uint32_t components[VKD3D_VEC4_SIZE];
    uint32_t reg_id;

    assert(reg->type != VKD3DSPR_IMMCONST);
    assert(write_mask);

    component_count = vkd3d_write_mask_component_count(write_mask);

    if (reg->data_type != VKD3D_DATA_FLOAT)
    {
        uint32_t type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, component_count);
        val_id = vkd3d_spirv_build_op_bitcast(builder, type_id, val_id);
    }

    if (component_count == 1)
        return vkd3d_dxbc_compiler_emit_store_scalar(compiler, reg, write_mask, val_id);

    reg_id = vkd3d_dxbc_compiler_get_register_id(compiler, reg);

    if (component_count != VKD3D_VEC4_SIZE)
    {
        uint32_t type_id, reg_val_id;

        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
        reg_val_id = vkd3d_spirv_build_op_load(builder, type_id, reg_id, SpvMemoryAccessMaskNone);

        for (i = 0, component_idx = 0; i < ARRAY_SIZE(components); ++i)
        {
            if (write_mask & (VKD3DSP_WRITEMASK_0 << i))
                components[i] = VKD3D_VEC4_SIZE + component_idx++;
            else
                components[i] = i;
        }

        val_id = vkd3d_spirv_build_op_vector_shuffle(builder,
                type_id, reg_val_id, val_id, components, ARRAY_SIZE(components));
    }

    vkd3d_spirv_build_op_store(builder, reg_id, val_id, SpvMemoryAccessMaskNone);
}

static uint32_t vkd3d_dxbc_compiler_emit_sat(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *reg, DWORD write_mask, uint32_t val_id)
{
    static const float zero[] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const float one[] = {1.0f, 1.0f, 1.0f, 1.0f};

    unsigned int component_count = vkd3d_write_mask_component_count(write_mask);
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, zero_id, one_id;

    zero_id = vkd3d_dxbc_compiler_get_constant(compiler,
            VKD3D_TYPE_FLOAT, component_count, (const uint32_t *)zero);
    one_id = vkd3d_dxbc_compiler_get_constant(compiler,
            VKD3D_TYPE_FLOAT, component_count, (const uint32_t *)one);

    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(reg->data_type), component_count);
    if (reg->data_type == VKD3D_DATA_FLOAT)
        return vkd3d_spirv_build_op_glsl_std450_nclamp(builder, type_id, val_id, zero_id, one_id);

    FIXME("Unhandled data type %#x.\n", reg->data_type);
    return val_id;
}

static void vkd3d_dxbc_compiler_emit_store_dst(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_dst_param *dst, uint32_t val_id)
{
    assert(!(dst->modifiers & ~VKD3DSPDM_SATURATE));
    if (dst->modifiers & VKD3DSPDM_SATURATE)
        val_id = vkd3d_dxbc_compiler_emit_sat(compiler, &dst->reg, dst->write_mask, val_id);

    vkd3d_dxbc_compiler_emit_store_reg(compiler, &dst->reg, dst->write_mask, val_id);
}

/* The Vulkan spec says:
 *
 *   "The variable decorated with GlobalInvocationId must be declared as a
 *   three-component vector of 32-bit integers."
 *
 *   "The variable decorated with LocalInvocationId must be declared as a
 *   three-component vector of 32-bit integers."
 *
 *   "The variable decorated with WorkgroupId must be declared as a
 *   three-component vector of 32-bit integers."
 *
 *   "The variable decorated with FragCoord must be declared as a
 *   four-component vector of 32-bit floating-point values."
 *
 *   "Any variable decorated with Position must be declared as a four-component
 *   vector of 32-bit floating-point values."
 *
 *   "The variable decorated with VertexIndex must be declared as a scalar
 *   32-bit integer."
 */
static const struct vkd3d_spirv_builtin
{
    enum vkd3d_shader_input_sysval_semantic sysval;
    enum vkd3d_shader_register_type reg_type;

    enum vkd3d_component_type component_type;
    unsigned int component_count;
    SpvBuiltIn spirv_builtin;
}
vkd3d_spirv_builtin_table[] =
{
    {VKD3D_SIV_NONE, VKD3DSPR_THREADID,         VKD3D_TYPE_INT, 3, SpvBuiltInGlobalInvocationId},
    {VKD3D_SIV_NONE, VKD3DSPR_LOCALTHREADID,    VKD3D_TYPE_INT, 3, SpvBuiltInLocalInvocationId},
    {VKD3D_SIV_NONE, VKD3DSPR_LOCALTHREADINDEX, VKD3D_TYPE_INT, 1, SpvBuiltInLocalInvocationIndex},
    {VKD3D_SIV_NONE, VKD3DSPR_THREADGROUPID,    VKD3D_TYPE_INT, 3, SpvBuiltInWorkgroupId},

    {VKD3D_SIV_POSITION,  ~0u, VKD3D_TYPE_FLOAT, 4, SpvBuiltInPosition},
    {VKD3D_SIV_VERTEX_ID, ~0u, VKD3D_TYPE_INT,   1, SpvBuiltInVertexIndex},
};

static const struct vkd3d_spirv_builtin *vkd3d_get_spirv_builtin(enum vkd3d_shader_register_type reg_type,
        enum vkd3d_shader_input_sysval_semantic sysval)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vkd3d_spirv_builtin_table); ++i)
    {
        const struct vkd3d_spirv_builtin* current = &vkd3d_spirv_builtin_table[i];

        if (current->sysval == VKD3D_SIV_NONE && current->reg_type == reg_type)
            return current;

        if (current->reg_type == ~0u && current->sysval == sysval)
            return current;
    }

    if (sysval != VKD3D_SIV_NONE
            || (reg_type != VKD3DSPR_INPUT && reg_type != VKD3DSPR_OUTPUT && reg_type != VKD3DSPR_COLOROUT))
        FIXME("Unhandled builtin (register type %#x, semantic %#x).\n", reg_type, sysval);
    return NULL;
}

static void vkd3d_dxbc_compiler_decorate_builtin(struct vkd3d_dxbc_compiler *compiler,
        uint32_t target_id, SpvBuiltIn builtin)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;

    if (compiler->shader_type == VKD3D_SHADER_TYPE_PIXEL && builtin == SpvBuiltInPosition)
        builtin = SpvBuiltInFragCoord;

    vkd3d_spirv_build_op_decorate1(builder, target_id, SpvDecorationBuiltIn, builtin);
}

static const struct vkd3d_shader_signature_element *vkd3d_find_signature_element_for_reg(
        const struct vkd3d_shader_signature *signature, unsigned int *signature_element_index,
        const struct vkd3d_shader_register *reg, DWORD write_mask)
{
    unsigned int signature_idx;

    for (signature_idx = 0; signature_idx < signature->element_count; ++signature_idx)
    {
        if (signature->elements[signature_idx].register_idx == reg->idx[0].offset
                && (signature->elements[signature_idx].mask & 0xff) == write_mask)
        {
            if (signature_element_index)
                *signature_element_index = signature_idx;
            return &signature->elements[signature_idx];
        }
    }

    FIXME("Could not find shader signature element (register %u, write mask %#x).\n",
            reg->idx[0].offset, write_mask);
    if (signature_element_index)
        *signature_element_index = ~0u;
    return NULL;
}

static uint32_t vkd3d_dxbc_compiler_emit_input(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_dst_param *dst, enum vkd3d_shader_input_sysval_semantic sysval)
{
    unsigned int component_idx, component_count, input_component_count;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_signature_element *signature_element;
    const struct vkd3d_shader_register *reg = &dst->reg;
    const struct vkd3d_spirv_builtin *builtin;
    enum vkd3d_component_type component_type;
    uint32_t val_id = 0, input_id, var_id;
    uint32_t type_id, float_type_id;
    struct vkd3d_symbol reg_symbol;
    SpvStorageClass storage_class;
    struct rb_entry *entry = NULL;
    bool use_private_var = false;
    DWORD write_mask;

    /* vThreadIDInGroupFlattened is declared with no write mask in shader
     * bytecode generated by fxc. */
    write_mask = dst->write_mask;
    if (!write_mask && reg->type == VKD3DSPR_LOCALTHREADINDEX)
        write_mask = VKD3DSP_WRITEMASK_0;

    signature_element = vkd3d_find_signature_element_for_reg(compiler->input_signature,
            NULL, reg, write_mask);
    builtin = vkd3d_get_spirv_builtin(reg->type, sysval);

    component_idx = vkd3d_write_mask_get_component_idx(write_mask);
    component_count = vkd3d_write_mask_component_count(write_mask);
    if (builtin)
    {
        component_type = builtin->component_type;
        input_component_count = builtin->component_count;
    }
    else
    {
        component_type = signature_element ? signature_element->component_type : VKD3D_TYPE_FLOAT;
        input_component_count = component_count;
    }
    assert(component_count <= input_component_count);

    storage_class = SpvStorageClassInput;
    input_id = vkd3d_dxbc_compiler_emit_variable(compiler, &builder->global_stream,
            storage_class, component_type, input_component_count);
    vkd3d_spirv_add_iface_variable(builder, input_id);
    if (builtin)
    {
        vkd3d_dxbc_compiler_decorate_builtin(compiler, input_id, builtin->spirv_builtin);
        if (component_idx)
            FIXME("Unhandled component index %u.\n", component_idx);
    }
    else
    {
        vkd3d_spirv_build_op_decorate1(builder, input_id, SpvDecorationLocation, reg->idx[0].offset);
        if (component_idx)
            vkd3d_spirv_build_op_decorate1(builder, input_id, SpvDecorationComponent, component_idx);
    }

    if (component_type != VKD3D_TYPE_FLOAT || component_count != VKD3D_VEC4_SIZE)
    {
        type_id = vkd3d_spirv_get_type_id(builder, component_type, input_component_count);
        val_id = vkd3d_spirv_build_op_load(builder, type_id, input_id, SpvMemoryAccessMaskNone);

        if (component_type != VKD3D_TYPE_FLOAT)
        {
            float_type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, input_component_count);
            val_id = vkd3d_spirv_build_op_bitcast(builder, float_type_id, val_id);
        }

        use_private_var = true;
    }

    if (val_id && input_component_count != component_count)
        val_id = vkd3d_dxbc_compiler_emit_swizzle(compiler, val_id, VKD3DSP_NOSWIZZLE, write_mask);

    vkd3d_symbol_make_register(&reg_symbol, reg);

    if (!use_private_var)
    {
        var_id = input_id;
    }
    else if (!(entry = rb_get(&compiler->symbol_table, &reg_symbol)))
    {
        storage_class = SpvStorageClassPrivate;
        var_id = vkd3d_dxbc_compiler_emit_variable(compiler, &builder->global_stream,
                storage_class, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    }
    if (!entry)
    {
        reg_symbol.id = var_id;
        reg_symbol.info.storage_class = storage_class;
        vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);

        vkd3d_dxbc_compiler_emit_register_debug_name(builder, var_id, reg);
    }

    if (use_private_var)
    {
        assert(val_id);
        vkd3d_dxbc_compiler_emit_store_reg(compiler, reg, write_mask, val_id);
    }

    return input_id;
}

static uint32_t vkd3d_dxbc_compiler_emit_output(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_dst_param *dst, enum vkd3d_shader_input_sysval_semantic sysval)
{
    unsigned int component_idx, component_count, output_component_count;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_signature_element *signature_element;
    const struct vkd3d_shader_register *reg = &dst->reg;
    const struct vkd3d_spirv_builtin *builtin;
    enum vkd3d_component_type component_type;
    struct vkd3d_symbol reg_symbol;
    SpvStorageClass storage_class;
    struct rb_entry *entry = NULL;
    unsigned int signature_idx;
    bool use_private_variable;
    uint32_t id, var_id;

    signature_element = vkd3d_find_signature_element_for_reg(compiler->output_signature,
            &signature_idx, reg, dst->write_mask);
    builtin = vkd3d_get_spirv_builtin(dst->reg.type, sysval);

    component_idx = vkd3d_write_mask_get_component_idx(dst->write_mask);
    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    if (builtin)
    {
        component_type = builtin->component_type;
        output_component_count = builtin->component_count;
    }
    else
    {
        component_type = signature_element ? signature_element->component_type : VKD3D_TYPE_FLOAT;
        output_component_count = component_count;
    }
    assert(component_count <= output_component_count);

    storage_class = SpvStorageClassOutput;
    id = vkd3d_dxbc_compiler_emit_variable(compiler, &builder->global_stream,
            storage_class, component_type, component_count);
    vkd3d_spirv_add_iface_variable(builder, id);
    if (builtin)
    {
        vkd3d_dxbc_compiler_decorate_builtin(compiler, id, builtin->spirv_builtin);
        if (component_idx)
            FIXME("Unhandled component index %u.\n", component_idx);
    }
    else
    {
        vkd3d_spirv_build_op_decorate1(builder, id, SpvDecorationLocation, reg->idx[0].offset);
        if (component_idx)
            vkd3d_spirv_build_op_decorate1(builder, id, SpvDecorationComponent, component_idx);
    }
    if (signature_element)
    {
        compiler->output_info[signature_idx].id = id;
        compiler->output_info[signature_idx].component_type = component_type;
    }

    if ((use_private_variable = component_type != VKD3D_TYPE_FLOAT || component_count != VKD3D_VEC4_SIZE))
        storage_class = SpvStorageClassPrivate;

    vkd3d_symbol_make_register(&reg_symbol, reg);

    if (!use_private_variable)
        var_id = id;
    else if ((entry = rb_get(&compiler->symbol_table, &reg_symbol)))
        var_id = RB_ENTRY_VALUE(entry, const struct vkd3d_symbol, entry)->id;
    else
        var_id = vkd3d_dxbc_compiler_emit_variable(compiler, &builder->global_stream,
                storage_class, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    if (!entry)
    {
        reg_symbol.id = var_id;
        reg_symbol.info.storage_class = storage_class;
        vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);

        vkd3d_dxbc_compiler_emit_register_debug_name(builder, var_id, reg);
    }

    if (use_private_variable)
    {
        compiler->private_output_variable[reg->idx[0].offset] = var_id;
        if (!compiler->output_setup_function_id)
            compiler->output_setup_function_id = vkd3d_spirv_alloc_id(builder);
    }

    return id;
}

static void vkd3d_dxbc_compiler_emit_dcl_global_flags(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    if (instruction->flags & ~(VKD3DSGF_REFACTORING_ALLOWED | VKD3DSGF_ENABLE_RAW_AND_STRUCTURED_BUFFERS))
        FIXME("Unrecognized global flags %#x.\n", instruction->flags);
    else
        WARN("Unhandled global flags %#x.\n", instruction->flags);
}

static void vkd3d_dxbc_compiler_emit_dcl_temps(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    unsigned int i;
    uint32_t id;

    /* FIXME: Make sure that function variables are declared at the beginning
     * of the first block in the function. Otherwise, we'll produce invalid
     * SPIR-V code. */
    assert(!compiler->temp_count);
    compiler->temp_count = instruction->declaration.count;
    for (i = 0; i < compiler->temp_count; ++i)
    {
        id = vkd3d_dxbc_compiler_emit_variable(compiler, &builder->function_stream,
                SpvStorageClassFunction, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
        if (!i)
            compiler->temp_id = id;
        assert(id == compiler->temp_id + i);

        vkd3d_spirv_build_op_name(builder, id, "r%u", i);
    }
}

static void vkd3d_dxbc_compiler_emit_push_constants(struct vkd3d_dxbc_compiler *compiler)
{
    const SpvStorageClass storage_class = SpvStorageClassPushConstant;
    uint32_t vec4_id, length_id, struct_id, pointer_type_id, var_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    unsigned int i, j, count, reg_idx, cb_size;
    struct vkd3d_symbol reg_symbol;
    uint32_t *member_ids;

    count = 0;
    for (i = 0; i < compiler->push_constant_count; ++i)
    {
        const struct vkd3d_push_constant_buffer *cb = &compiler->push_constants[i];

        if (cb->reg.type)
            ++count;
    }
    if (!count)
        return;

    if (!(member_ids = vkd3d_calloc(count, sizeof(*member_ids))))
        return;

    vec4_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);

    for (i = 0, j = 0; i < compiler->push_constant_count; ++i)
    {
        const struct vkd3d_push_constant_buffer *cb = &compiler->push_constants[i];
        if (!cb->reg.type)
            continue;

        cb_size = cb->reg.idx[1].offset;
        length_id = vkd3d_dxbc_compiler_get_constant_uint(compiler, cb_size);
        member_ids[j]  = vkd3d_spirv_build_op_type_array(builder, vec4_id, length_id);
        vkd3d_spirv_build_op_decorate1(builder, member_ids[j], SpvDecorationArrayStride, 16);

        ++j;
    }

    struct_id = vkd3d_spirv_build_op_type_struct(builder, member_ids, count);
    vkd3d_spirv_build_op_decorate(builder, struct_id, SpvDecorationBlock, NULL, 0);
    vkd3d_spirv_build_op_name(builder, struct_id, "push_cb");
    vkd3d_free(member_ids);

    pointer_type_id = vkd3d_spirv_get_op_type_pointer(builder, storage_class, struct_id);
    var_id = vkd3d_spirv_build_op_variable(builder, &builder->global_stream,
            pointer_type_id, storage_class, 0);

    for (i = 0, j = 0; i < compiler->push_constant_count; ++i)
    {
        const struct vkd3d_push_constant_buffer *cb = &compiler->push_constants[i];
        if (!cb->reg.type)
            continue;

        reg_idx = cb->reg.idx[0].offset;
        vkd3d_spirv_build_op_member_decorate1(builder, struct_id, j,
                SpvDecorationOffset, cb->pc.offset * sizeof(uint32_t));
        vkd3d_spirv_build_op_member_name(builder, struct_id, j, "cb%u", reg_idx);

        vkd3d_symbol_make_register(&reg_symbol, &cb->reg);
        reg_symbol.id = var_id;
        reg_symbol.info.storage_class = storage_class;
        vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);
        if (j)
            FIXME("Multiple push constant buffers not supported yet.\n");

        ++j;
    }
}

static void vkd3d_dxbc_compiler_emit_dcl_constant_buffer(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t vec4_id, array_type_id, length_id, struct_id, pointer_type_id, var_id;
    const struct vkd3d_shader_register *reg = &instruction->declaration.src.reg;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const SpvStorageClass storage_class = SpvStorageClassUniform;
    struct vkd3d_push_constant_buffer *push_cb;
    struct vkd3d_symbol reg_symbol;
    unsigned int cb_size;

    assert(!(instruction->flags & ~VKD3DSI_INDEXED_DYNAMIC));

    if (instruction->flags & VKD3DSI_INDEXED_DYNAMIC)
        vkd3d_spirv_enable_capability(builder, SpvCapabilityUniformBufferArrayDynamicIndexing);

    cb_size = reg->idx[1].offset;

    if ((push_cb = vkd3d_dxbc_compiler_find_push_constant(compiler, reg)))
    {
        push_cb->reg = *reg;
        if (cb_size * VKD3D_VEC4_SIZE != push_cb->pc.count)
            FIXME("Push constant size do not match (cb size %u, constant count %u).\n",
                    cb_size, push_cb->pc.count);
        return;
    }

    vec4_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    length_id = vkd3d_dxbc_compiler_get_constant_uint(compiler, cb_size);
    array_type_id = vkd3d_spirv_build_op_type_array(builder, vec4_id, length_id);
    vkd3d_spirv_build_op_decorate1(builder, array_type_id, SpvDecorationArrayStride, 16);

    struct_id = vkd3d_spirv_build_op_type_struct(builder, &array_type_id, 1);
    vkd3d_spirv_build_op_decorate(builder, struct_id, SpvDecorationBlock, NULL, 0);
    vkd3d_spirv_build_op_member_decorate1(builder, struct_id, 0, SpvDecorationOffset, 0);
    vkd3d_spirv_build_op_name(builder, struct_id, "cb%u_struct", cb_size);

    pointer_type_id = vkd3d_spirv_get_op_type_pointer(builder, storage_class, struct_id);
    var_id = vkd3d_spirv_build_op_variable(builder, &builder->global_stream,
            pointer_type_id, storage_class, 0);

    vkd3d_dxbc_compiler_emit_descriptor_binding(compiler, var_id, reg);

    vkd3d_dxbc_compiler_emit_register_debug_name(builder, var_id, reg);

    vkd3d_symbol_make_register(&reg_symbol, reg);
    reg_symbol.id = var_id;
    reg_symbol.info.storage_class = storage_class;
    vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);
}

static void vkd3d_dxbc_compiler_emit_dcl_immediate_constant_buffer(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    const struct vkd3d_shader_immediate_constant_buffer *icb = instruction->declaration.icb;
    uint32_t *elements, length_id, type_id, const_id, ptr_type_id, icb_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    struct vkd3d_shader_register reg;
    struct vkd3d_symbol reg_symbol;
    unsigned int i;

    if (!(elements = vkd3d_calloc(icb->vec4_count, sizeof(*elements))))
        return;
    for (i = 0; i < icb->vec4_count; ++i)
        elements[i] = vkd3d_dxbc_compiler_get_constant(compiler,
                VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE, &icb->data[4 * i]);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
    length_id = vkd3d_dxbc_compiler_get_constant_uint(compiler, icb->vec4_count);
    type_id = vkd3d_spirv_build_op_type_array(builder, type_id, length_id);
    const_id = vkd3d_spirv_build_op_constant_composite(builder, type_id, elements, icb->vec4_count);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, SpvStorageClassPrivate, type_id);
    icb_id = vkd3d_spirv_build_op_variable(builder, &builder->global_stream,
            ptr_type_id, SpvStorageClassPrivate, const_id);
    vkd3d_spirv_build_op_name(builder, icb_id, "icb");
    vkd3d_free(elements);

    memset(&reg, 0, sizeof(reg));
    reg.type = VKD3DSPR_IMMCONSTBUFFER;
    vkd3d_symbol_make_register(&reg_symbol, &reg);
    reg_symbol.id = icb_id;
    reg_symbol.info.storage_class = SpvStorageClassPrivate;
    vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);
}

static void vkd3d_dxbc_compiler_emit_dcl_sampler(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    const struct vkd3d_shader_register *reg = &instruction->declaration.dst.reg;
    const SpvStorageClass storage_class = SpvStorageClassUniformConstant;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t type_id, ptr_type_id, var_id;
    struct vkd3d_symbol reg_symbol;

    type_id = vkd3d_spirv_get_op_type_sampler(builder);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, storage_class, type_id);
    var_id = vkd3d_spirv_build_op_variable(builder, &builder->global_stream,
            ptr_type_id, storage_class, 0);

    vkd3d_dxbc_compiler_emit_descriptor_binding(compiler, var_id, reg);

    vkd3d_dxbc_compiler_emit_register_debug_name(builder, var_id, reg);

    vkd3d_symbol_make_register(&reg_symbol, reg);
    reg_symbol.id = var_id;
    reg_symbol.info.storage_class = storage_class;
    vkd3d_dxbc_compiler_put_symbol(compiler, &reg_symbol);
}

static const struct vkd3d_spirv_resource_type
{
    enum vkd3d_shader_resource_type resource_type;

    SpvDim dim;
    uint32_t arrayed;
    uint32_t ms;

    unsigned int coordinate_component_count;

    SpvCapability capability;
    SpvCapability uav_capability;
}
vkd3d_spirv_resource_type_table[] =
{
    {VKD3D_SHADER_RESOURCE_BUFFER,            SpvDimBuffer, 0, 0, 1,
            SpvCapabilitySampledBuffer, SpvCapabilityImageBuffer},
    {VKD3D_SHADER_RESOURCE_TEXTURE_1D,        SpvDim1D,     0, 0, 1,
            SpvCapabilitySampled1D, SpvCapabilityImage1D},
    {VKD3D_SHADER_RESOURCE_TEXTURE_2DMS,      SpvDim2D,     0, 1, 2},
    {VKD3D_SHADER_RESOURCE_TEXTURE_2D,        SpvDim2D,     0, 0, 2},
    {VKD3D_SHADER_RESOURCE_TEXTURE_3D,        SpvDim3D,     0, 0, 3},
    {VKD3D_SHADER_RESOURCE_TEXTURE_CUBE,      SpvDimCube,   0, 0, 3},
    {VKD3D_SHADER_RESOURCE_TEXTURE_1DARRAY,   SpvDim1D,     1, 0, 2,
            SpvCapabilitySampled1D, SpvCapabilityImage1D},
    {VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY,   SpvDim2D,     1, 0, 3},
    {VKD3D_SHADER_RESOURCE_TEXTURE_CUBEARRAY, SpvDimCube,   1, 0, 3,
            SpvCapabilitySampledCubeArray, SpvCapabilityImageCubeArray},
};

static const struct vkd3d_spirv_resource_type *vkd3d_get_spirv_resource_type(
        enum vkd3d_shader_resource_type resource_type)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vkd3d_spirv_resource_type_table); ++i)
    {
        const struct vkd3d_spirv_resource_type* current = &vkd3d_spirv_resource_type_table[i];

        if (current->resource_type == resource_type)
            return current;
    }

    FIXME("Unhandled resource type %#x.\n", resource_type);
    return NULL;
}

static const struct vkd3d_spirv_resource_type *vkd3d_dxbc_compiler_enable_resource_type(
        struct vkd3d_dxbc_compiler *compiler, enum vkd3d_shader_resource_type resource_type, bool is_uav)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_spirv_resource_type *resource_type_info;

    if (!(resource_type_info = vkd3d_get_spirv_resource_type(resource_type)))
        return NULL;

    if (resource_type_info->capability)
        vkd3d_spirv_enable_capability(builder, resource_type_info->capability);
    if (is_uav && resource_type_info->uav_capability)
        vkd3d_spirv_enable_capability(builder, resource_type_info->uav_capability);

    return resource_type_info;
}

static void vkd3d_dxbc_compiler_emit_resource_declaration(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_semantic *semantic)
{
    const SpvStorageClass storage_class = SpvStorageClassUniformConstant;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_register *reg = &semantic->reg.reg;
    const struct vkd3d_spirv_resource_type *resource_type_info;
    uint32_t sampled_type_id, type_id, ptr_type_id, var_id;
    enum vkd3d_component_type sampled_type;
    struct vkd3d_symbol resource_symbol;
    bool is_uav;

    is_uav = reg->type == VKD3DSPR_UAV;
    if (!(resource_type_info = vkd3d_dxbc_compiler_enable_resource_type(compiler,
            semantic->resource_type, is_uav)))
    {
        FIXME("Failed to emit resource declaration.\n");
        return;
    }

    sampled_type = vkd3d_component_type_from_data_type(semantic->resource_data_type);
    sampled_type_id = vkd3d_spirv_get_type_id(builder, sampled_type, 1);

    type_id = vkd3d_spirv_get_op_type_image(builder, sampled_type_id, resource_type_info->dim,
            0, resource_type_info->arrayed, resource_type_info->ms, is_uav ? 2 : 1, SpvImageFormatUnknown);

    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, storage_class, type_id);
    var_id = vkd3d_spirv_build_op_variable(builder, &builder->global_stream,
            ptr_type_id, storage_class, 0);

    vkd3d_dxbc_compiler_emit_descriptor_binding(compiler, var_id, reg);

    vkd3d_dxbc_compiler_emit_register_debug_name(builder, var_id, reg);

    vkd3d_symbol_make_resource(&resource_symbol, reg);
    resource_symbol.id = var_id;
    resource_symbol.info.resource.sampled_type = sampled_type;
    resource_symbol.info.resource.type_id = type_id;
    resource_symbol.info.resource.coordinate_mask = (1u << resource_type_info->coordinate_component_count) - 1;
    vkd3d_dxbc_compiler_put_symbol(compiler, &resource_symbol);
}

static void vkd3d_dxbc_compiler_emit_dcl_resource(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    vkd3d_dxbc_compiler_emit_resource_declaration(compiler, &instruction->declaration.semantic);
}

static void vkd3d_dxbc_compiler_emit_dcl_uav_typed(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    if (instruction->flags)
        FIXME("Unhandled flags %#x.\n", instruction->flags);

    vkd3d_dxbc_compiler_emit_resource_declaration(compiler, &instruction->declaration.semantic);
}

static void vkd3d_dxbc_compiler_emit_dcl_input(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    vkd3d_dxbc_compiler_emit_input(compiler, &instruction->declaration.dst, VKD3D_SIV_NONE);
}

static void vkd3d_dxbc_compiler_emit_interpolation_decorations(struct vkd3d_dxbc_compiler *compiler,
        uint32_t id, enum vkd3d_shader_interpolation_mode mode)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;

    switch (mode)
    {
        case VKD3DSIM_CONSTANT:
            vkd3d_spirv_build_op_decorate(builder, id, SpvDecorationFlat, NULL, 0);
            break;
        case VKD3DSIM_LINEAR:
            break;
        default:
            FIXME("Unhandled interpolation mode %#x.\n", mode);
            break;
    }
}

static void vkd3d_dxbc_compiler_emit_dcl_input_ps(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t input_id;

    input_id = vkd3d_dxbc_compiler_emit_input(compiler, &instruction->declaration.dst, VKD3D_SIV_NONE);
    vkd3d_dxbc_compiler_emit_interpolation_decorations(compiler, input_id, instruction->flags);
}

static void vkd3d_dxbc_compiler_emit_dcl_input_ps_siv(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t input_id;

    input_id = vkd3d_dxbc_compiler_emit_input(compiler, &instruction->declaration.register_semantic.reg,
            instruction->declaration.register_semantic.sysval_semantic);
    if (!instruction->declaration.register_semantic.sysval_semantic)
        vkd3d_dxbc_compiler_emit_interpolation_decorations(compiler, input_id, instruction->flags);
}

static void vkd3d_dxbc_compiler_emit_dcl_input_sgv(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    vkd3d_dxbc_compiler_emit_input(compiler, &instruction->declaration.register_semantic.reg,
            instruction->declaration.register_semantic.sysval_semantic);
}

static void vkd3d_dxbc_compiler_emit_dcl_output(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    vkd3d_dxbc_compiler_emit_output(compiler, &instruction->declaration.dst, VKD3D_SIV_NONE);
}

static void vkd3d_dxbc_compiler_emit_dcl_output_siv(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    vkd3d_dxbc_compiler_emit_output(compiler, &instruction->declaration.register_semantic.reg,
            instruction->declaration.register_semantic.sysval_semantic);
}

static void vkd3d_dxbc_compiler_emit_dcl_thread_group(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    const struct vkd3d_shader_thread_group_size *group_size = &instruction->declaration.thread_group_size;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;

    vkd3d_spirv_set_local_size(builder, group_size->x, group_size->y, group_size->z);
}

static SpvOp vkd3d_dxbc_compiler_map_alu_instruction(const struct vkd3d_shader_instruction *instruction)
{
    static const struct
    {
        enum VKD3D_SHADER_INSTRUCTION_HANDLER handler_idx;
        SpvOp spirv_op;
    }
    alu_ops[] =
    {
        {VKD3DSIH_ADD,        SpvOpFAdd},
        {VKD3DSIH_AND,        SpvOpBitwiseAnd},
        {VKD3DSIH_BFREV,      SpvOpBitReverse},
        {VKD3DSIH_COUNTBITS,  SpvOpBitCount},
        {VKD3DSIH_DIV,        SpvOpFDiv},
        {VKD3DSIH_FTOI,       SpvOpConvertFToS},
        {VKD3DSIH_FTOU,       SpvOpConvertFToU},
        {VKD3DSIH_IADD,       SpvOpIAdd},
        {VKD3DSIH_ISHL,       SpvOpShiftLeftLogical},
        {VKD3DSIH_ISHR,       SpvOpShiftRightArithmetic},
        {VKD3DSIH_ITOF,       SpvOpConvertSToF},
        {VKD3DSIH_MUL,        SpvOpFMul},
        {VKD3DSIH_NOT,        SpvOpNot},
        {VKD3DSIH_OR,         SpvOpBitwiseOr},
        {VKD3DSIH_USHR,       SpvOpShiftRightLogical},
        {VKD3DSIH_UTOF,       SpvOpConvertUToF},
        {VKD3DSIH_XOR,        SpvOpBitwiseXor},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(alu_ops); ++i)
    {
        if (alu_ops[i].handler_idx == instruction->handler_idx)
            return alu_ops[i].spirv_op;
    }

    return SpvOpMax;
}

static void vkd3d_dxbc_compiler_emit_alu_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t src_ids[VKD3D_DXBC_MAX_SOURCE_COUNT];
    unsigned int component_count;
    uint32_t type_id, val_id;
    unsigned int i;
    SpvOp op;

    op = vkd3d_dxbc_compiler_map_alu_instruction(instruction);
    if (op == SpvOpMax)
    {
        ERR("Unexpected instruction %#x.\n", instruction->handler_idx);
        return;
    }

    assert(instruction->dst_count == 1);
    assert(instruction->src_count <= VKD3D_DXBC_MAX_SOURCE_COUNT);

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(dst->reg.data_type), component_count);

    for (i = 0; i < instruction->src_count; ++i)
        src_ids[i] = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[i], dst->write_mask);

    val_id = vkd3d_spirv_build_op_trv(builder, &builder->function_stream, op, type_id,
            src_ids, instruction->src_count);

    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static enum GLSLstd450 vkd3d_dxbc_compiler_map_ext_glsl_instruction(
        const struct vkd3d_shader_instruction *instruction)
{
    static const struct
    {
        enum VKD3D_SHADER_INSTRUCTION_HANDLER handler_idx;
        enum GLSLstd450 glsl_inst;
    }
    glsl_insts[] =
    {
        {VKD3DSIH_EXP,             GLSLstd450Exp2},
        {VKD3DSIH_FIRSTBIT_HI,     GLSLstd450FindUMsb},
        {VKD3DSIH_FIRSTBIT_LO,     GLSLstd450FindILsb},
        {VKD3DSIH_FIRSTBIT_SHI,    GLSLstd450FindSMsb},
        {VKD3DSIH_FRC,             GLSLstd450Fract},
        {VKD3DSIH_IMAX,            GLSLstd450SMax},
        {VKD3DSIH_IMIN,            GLSLstd450SMin},
        {VKD3DSIH_LOG,             GLSLstd450Log2},
        {VKD3DSIH_MAD,             GLSLstd450Fma},
        {VKD3DSIH_MAX,             GLSLstd450FMax},
        {VKD3DSIH_MIN,             GLSLstd450FMin},
        {VKD3DSIH_ROUND_NI,        GLSLstd450Floor},
        {VKD3DSIH_ROUND_PI,        GLSLstd450Ceil},
        {VKD3DSIH_RSQ,             GLSLstd450InverseSqrt},
        {VKD3DSIH_SQRT,            GLSLstd450Sqrt},
        {VKD3DSIH_UMAX,            GLSLstd450UMax},
        {VKD3DSIH_UMIN,            GLSLstd450UMin},
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(glsl_insts); ++i)
    {
        if (glsl_insts[i].handler_idx == instruction->handler_idx)
            return glsl_insts[i].glsl_inst;
    }

    return GLSLstd450Bad;
}

static void vkd3d_dxbc_compiler_emit_ext_glsl_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t src_id[VKD3D_DXBC_MAX_SOURCE_COUNT];
    uint32_t instr_set_id, type_id, val_id;
    unsigned int component_count;
    enum GLSLstd450 glsl_inst;
    unsigned int i;

    glsl_inst = vkd3d_dxbc_compiler_map_ext_glsl_instruction(instruction);
    if (glsl_inst == GLSLstd450Bad)
    {
        ERR("Unexpected instruction %#x.\n", instruction->handler_idx);
        return;
    }

    instr_set_id = vkd3d_spirv_get_glsl_std450_instr_set(builder);

    assert(instruction->dst_count == 1);
    assert(instruction->src_count <= VKD3D_DXBC_MAX_SOURCE_COUNT);

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(dst->reg.data_type), component_count);

    for (i = 0; i < instruction->src_count; ++i)
        src_id[i] = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[i], dst->write_mask);

    val_id = vkd3d_spirv_build_op_ext_inst(builder, type_id,
            instr_set_id, glsl_inst, src_id, instruction->src_count);

    if (instruction->handler_idx == VKD3DSIH_FIRSTBIT_HI
            || instruction->handler_idx == VKD3DSIH_FIRSTBIT_SHI)
    {
        /* In D3D bits are numbered from the most significant bit. */
        val_id = vkd3d_spirv_build_op_isub(builder, type_id,
                vkd3d_dxbc_compiler_get_constant_uint(compiler, 31), val_id);
    }

    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_mov(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t val_id, dst_val_id, type_id, dst_id, src_id;
    uint32_t components[VKD3D_VEC4_SIZE];
    unsigned int i, component_count;

    component_count = vkd3d_write_mask_component_count(dst->write_mask);

    if (component_count == 1 || component_count == VKD3D_VEC4_SIZE
            || dst->modifiers || src->modifiers || src->reg.type == VKD3DSPR_IMMCONST)
    {
        val_id = vkd3d_dxbc_compiler_emit_load_src(compiler, src, dst->write_mask);
        vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
    }
    else
    {
        struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;

        type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, VKD3D_VEC4_SIZE);
        dst_id = vkd3d_dxbc_compiler_get_register_id(compiler, &dst->reg);
        src_id = vkd3d_dxbc_compiler_get_register_id(compiler, &src->reg);

        val_id = vkd3d_spirv_build_op_load(builder, type_id, src_id, SpvMemoryAccessMaskNone);
        dst_val_id = vkd3d_spirv_build_op_load(builder, type_id, dst_id, SpvMemoryAccessMaskNone);

        for (i = 0; i < ARRAY_SIZE(components); ++i)
        {
            if (dst->write_mask & (VKD3DSP_WRITEMASK_0 << i))
                components[i] = VKD3D_VEC4_SIZE + vkd3d_swizzle_get_component(src->swizzle, i);
            else
                components[i] = i;
        }

        val_id = vkd3d_spirv_build_op_vector_shuffle(builder,
                type_id, dst_val_id, val_id, components, VKD3D_VEC4_SIZE);

        vkd3d_spirv_build_op_store(builder, dst_id, val_id, SpvMemoryAccessMaskNone);
    }
}

static uint32_t vkd3d_dxbc_compiler_emit_int_to_bool(struct vkd3d_dxbc_compiler *compiler,
        enum vkd3d_shader_conditional_op condition, unsigned int component_count, uint32_t val_id)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    static const uint32_t zero[VKD3D_VEC4_SIZE];
    uint32_t type_id;
    SpvOp op;

    assert(!(condition & ~(VKD3D_SHADER_CONDITIONAL_OP_NZ | VKD3D_SHADER_CONDITIONAL_OP_Z)));

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_BOOL, component_count);
    op = condition & VKD3D_SHADER_CONDITIONAL_OP_Z ? SpvOpIEqual : SpvOpINotEqual;
    return vkd3d_spirv_build_op_tr2(builder, &builder->function_stream, op, type_id, val_id,
            vkd3d_dxbc_compiler_get_constant(compiler, VKD3D_TYPE_UINT, component_count, zero));
}

static void vkd3d_dxbc_compiler_emit_movc(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t condition_id, src1_id, src2_id, type_id, val_id;
    unsigned int component_count;

    condition_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst->write_mask);
    src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst->write_mask);
    src2_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[2], dst->write_mask);

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, component_count);

    condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler,
            VKD3D_SHADER_CONDITIONAL_OP_NZ, component_count, condition_id);
    val_id = vkd3d_spirv_build_op_select(builder, type_id, condition_id, src1_id, src2_id);

    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_swapc(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t condition_id, src1_id, src2_id, type_id, val_id;
    unsigned int component_count;

    assert(dst[0].write_mask == dst[1].write_mask);

    condition_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst->write_mask);
    src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst->write_mask);
    src2_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[2], dst->write_mask);

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, component_count);

    condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler,
            VKD3D_SHADER_CONDITIONAL_OP_NZ, component_count, condition_id);

    val_id = vkd3d_spirv_build_op_select(builder, type_id, condition_id, src2_id, src1_id);
    vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst[0], val_id);
    val_id = vkd3d_spirv_build_op_select(builder, type_id, condition_id, src1_id, src2_id);
    vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst[1], val_id);
}

static void vkd3d_dxbc_compiler_emit_dot(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t type_id, val_id, src_ids[2];
    DWORD write_mask;
    unsigned int i;

    assert(vkd3d_write_mask_component_count(dst->write_mask) == 1);

    if (instruction->handler_idx == VKD3DSIH_DP4)
        write_mask = VKD3DSP_WRITEMASK_ALL;
    else if (instruction->handler_idx == VKD3DSIH_DP3)
        write_mask = VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1 | VKD3DSP_WRITEMASK_2;
    else
        write_mask = VKD3DSP_WRITEMASK_0 | VKD3DSP_WRITEMASK_1;

    assert(instruction->src_count == ARRAY_SIZE(src_ids));
    for (i = 0; i < instruction->src_count; ++i)
        src_ids[i] = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[i], write_mask);

    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(dst->reg.data_type), 1);

    val_id = vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            SpvOpDot, type_id, src_ids[0], src_ids[1]);

    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_rcp(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t type_id, src_id, val_id;
    unsigned int component_count;

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(dst->reg.data_type), component_count);

    src_id = vkd3d_dxbc_compiler_emit_load_src(compiler, src, dst->write_mask);
    val_id = vkd3d_spirv_build_op_fdiv(builder, type_id,
            vkd3d_dxbc_compiler_get_constant_float(compiler, 1.0f), src_id);
    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_imul(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t type_id, val_id, src0_id, src1_id;
    unsigned int component_count;

    if (dst[0].reg.type != VKD3DSPR_NULL)
        FIXME("Extended multiplies not implemented.\n"); /* SpvOpSMulExtended */

    if (dst[1].reg.type == VKD3DSPR_NULL)
        return;

    component_count = vkd3d_write_mask_component_count(dst[1].write_mask);
    type_id = vkd3d_spirv_get_type_id(builder,
            vkd3d_component_type_from_data_type(dst[1].reg.data_type), component_count);

    src0_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst[1].write_mask);
    src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst[1].write_mask);

    val_id = vkd3d_spirv_build_op_imul(builder, type_id, src0_id, src1_id);

    vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst[1], val_id);
}

static void vkd3d_dxbc_compiler_emit_imad(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t type_id, val_id, src_ids[3];
    unsigned int i, component_count;

    component_count = vkd3d_write_mask_component_count(dst->write_mask);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_INT, component_count);

    for (i = 0; i < ARRAY_SIZE(src_ids); ++i)
        src_ids[i] = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[i], dst->write_mask);

    val_id = vkd3d_spirv_build_op_imul(builder, type_id, src_ids[0], src_ids[1]);
    val_id = vkd3d_spirv_build_op_iadd(builder, type_id, val_id, src_ids[2]);

    vkd3d_dxbc_compiler_emit_store_dst(compiler, dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_udiv(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    static const uint32_t ffffffff[] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};

    uint32_t type_id, val_id, src0_id, src1_id, condition_id, ffffffff_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    unsigned int component_count = 0;

    if (dst[0].reg.type != VKD3DSPR_NULL)
    {
        component_count = vkd3d_write_mask_component_count(dst[0].write_mask);
        type_id = vkd3d_spirv_get_type_id(builder,
                vkd3d_component_type_from_data_type(dst[0].reg.data_type), component_count);

        src0_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst[0].write_mask);
        src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst[0].write_mask);

        condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler,
                VKD3D_SHADER_CONDITIONAL_OP_NZ, component_count, src1_id);
        ffffffff_id = vkd3d_dxbc_compiler_get_constant(compiler,
                VKD3D_TYPE_UINT, component_count, ffffffff);

        val_id = vkd3d_spirv_build_op_udiv(builder, type_id, src0_id, src1_id);
        /* The SPIR-V spec says: "The resulting value is undefined if Operand 2 is 0." */
        val_id = vkd3d_spirv_build_op_select(builder, type_id, condition_id, val_id, ffffffff_id);

        vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst[0], val_id);
    }

    if (dst[1].reg.type != VKD3DSPR_NULL)
    {
        if (!component_count || dst[0].write_mask != dst[1].write_mask)
        {
            component_count = vkd3d_write_mask_component_count(dst[1].write_mask);
            type_id = vkd3d_spirv_get_type_id(builder,
                    vkd3d_component_type_from_data_type(dst[1].reg.data_type), component_count);

            src0_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst[1].write_mask);
            src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst[1].write_mask);

            condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler,
                    VKD3D_SHADER_CONDITIONAL_OP_NZ, component_count, src1_id);
            ffffffff_id = vkd3d_dxbc_compiler_get_constant(compiler,
                    VKD3D_TYPE_UINT, component_count, ffffffff);
        }

        val_id = vkd3d_spirv_build_op_umod(builder, type_id, src0_id, src1_id);
        /* The SPIR-V spec says: "The resulting value is undefined if Operand 2 is 0." */
        val_id = vkd3d_spirv_build_op_select(builder, type_id, condition_id, val_id, ffffffff_id);

        vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst[1], val_id);
    }
}

static void vkd3d_dxbc_compiler_emit_bitfield_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t src_ids[4], result_id, type_id, mask_id;
    unsigned int i, j, src_count;
    DWORD write_mask;
    SpvOp op;

    src_count = instruction->src_count;
    assert(2 <= src_count && src_count <= ARRAY_SIZE(src_ids));

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_UINT, 1);
    mask_id = vkd3d_dxbc_compiler_get_constant_uint(compiler, 0x1f);

    switch (instruction->handler_idx)
    {
        case VKD3DSIH_BFI:  op = SpvOpBitFieldInsert; break;
        case VKD3DSIH_IBFE: op = SpvOpBitFieldSExtract; break;
        case VKD3DSIH_UBFE: op = SpvOpBitFieldUExtract; break;
        default:
            ERR("Unexpected instruction %#x.\n", instruction->handler_idx);
            return;
    }

    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (!(write_mask = dst->write_mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;

        for (j = 0; j < src_count; ++j)
            src_ids[src_count - j - 1] = vkd3d_dxbc_compiler_emit_load_reg(compiler,
                    &src[j].reg, src[j].swizzle, write_mask);

        for (j = src_count - 2; j < src_count; ++j)
        {
            uint32_t int_type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_INT, 1);
            src_ids[j] = vkd3d_spirv_build_op_and(builder, int_type_id, src_ids[j], mask_id);
        }

        result_id = vkd3d_spirv_build_op_trv(builder, &builder->function_stream,
                op, type_id, src_ids, src_count);

        vkd3d_dxbc_compiler_emit_store_reg(compiler, &dst->reg, write_mask, result_id);
    }
}

static void vkd3d_dxbc_compiler_emit_f16tof32(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    static const uint32_t indexes[] = {0};

    uint32_t instr_set_id, type_id, scalar_type_id, src_id, result_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    DWORD write_mask;
    unsigned int i;

    instr_set_id = vkd3d_spirv_get_glsl_std450_instr_set(builder);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 2);
    scalar_type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 1);

    /* FIXME: Consider a single UnpackHalf2x16 intruction per 2 components. */
    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (!(write_mask = dst->write_mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;

        src_id = vkd3d_dxbc_compiler_emit_load_src(compiler, src, write_mask);
        result_id = vkd3d_spirv_build_op_ext_inst(builder, type_id,
                instr_set_id, GLSLstd450UnpackHalf2x16, &src_id, 1);
        result_id = vkd3d_spirv_build_op_composite_extract(builder, scalar_type_id,
                result_id, indexes, ARRAY_SIZE(indexes));
        vkd3d_dxbc_compiler_emit_store_reg(compiler, &dst->reg, write_mask, result_id);
    }
}

static void vkd3d_dxbc_compiler_emit_f32tof16(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t instr_set_id, type_id, scalar_type_id, src_id, zero_id, constituents[2], result_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    DWORD write_mask;
    unsigned int i;

    instr_set_id = vkd3d_spirv_get_glsl_std450_instr_set(builder);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 2);
    scalar_type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_UINT, 1);
    zero_id = vkd3d_dxbc_compiler_get_constant_float(compiler, 0.0f);

    /* FIXME: Consider a single PackHalf2x16 intruction per 2 components. */
    for (i = 0; i < VKD3D_VEC4_SIZE; ++i)
    {
        if (!(write_mask = dst->write_mask & (VKD3DSP_WRITEMASK_0 << i)))
            continue;

        src_id = vkd3d_dxbc_compiler_emit_load_src(compiler, src, write_mask);
        constituents[0] = src_id;
        constituents[1] = zero_id;
        src_id = vkd3d_spirv_build_op_composite_construct(builder,
                type_id, constituents, ARRAY_SIZE(constituents));
        result_id = vkd3d_spirv_build_op_ext_inst(builder, scalar_type_id,
                instr_set_id, GLSLstd450PackHalf2x16, &src_id, 1);
        vkd3d_dxbc_compiler_emit_store_reg(compiler, &dst->reg, write_mask, result_id);
    }
}

static void vkd3d_dxbc_compiler_emit_comparison_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    static const uint32_t d3d_true[] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    static const uint32_t d3d_false[] = {0, 0, 0, 0};

    uint32_t src0_id, src1_id, type_id, result_id, true_id, false_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    unsigned int component_count;
    SpvOp op;

    switch (instruction->handler_idx)
    {
        case VKD3DSIH_EQ:  op = SpvOpFOrdEqual; break;
        case VKD3DSIH_GE:  op = SpvOpFOrdGreaterThanEqual; break;
        case VKD3DSIH_IEQ: op = SpvOpIEqual; break;
        case VKD3DSIH_IGE: op = SpvOpSGreaterThanEqual; break;
        case VKD3DSIH_ILT: op = SpvOpSLessThan; break;
        case VKD3DSIH_INE: op = SpvOpINotEqual; break;
        case VKD3DSIH_LT:  op = SpvOpFOrdLessThan; break;
        case VKD3DSIH_NE:  op = SpvOpFUnordNotEqual; break;
        case VKD3DSIH_UGE: op = SpvOpUGreaterThanEqual; break;
        case VKD3DSIH_ULT: op = SpvOpULessThan; break;
        default:
            ERR("Unexpected instruction %#x.\n", instruction->handler_idx);
            return;
    }

    component_count = vkd3d_write_mask_component_count(dst->write_mask);

    src0_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], dst->write_mask);
    src1_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[1], dst->write_mask);

    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_BOOL, component_count);
    result_id = vkd3d_spirv_build_op_tr2(builder, &builder->function_stream,
            op, type_id, src0_id, src1_id);

    true_id = vkd3d_dxbc_compiler_get_constant(compiler, VKD3D_TYPE_UINT, component_count, d3d_true);
    false_id = vkd3d_dxbc_compiler_get_constant(compiler, VKD3D_TYPE_UINT, component_count, d3d_false);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_UINT, component_count);
    result_id = vkd3d_spirv_build_op_select(builder, type_id, result_id, true_id, false_id);

    vkd3d_dxbc_compiler_emit_store_reg(compiler, &dst->reg, dst->write_mask, result_id);
}

static void vkd3d_dxbc_compiler_emit_breakc(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction, uint32_t target_block_id)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_src_param *src = instruction->src;
    uint32_t condition_id, merge_block_id;

    condition_id = vkd3d_dxbc_compiler_emit_load_src(compiler, src, VKD3DSP_WRITEMASK_0);
    condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler, instruction->flags, 1, condition_id);

    merge_block_id = vkd3d_spirv_alloc_id(builder);

    vkd3d_spirv_build_op_selection_merge(builder, merge_block_id, SpvSelectionControlMaskNone);
    vkd3d_spirv_build_op_branch_conditional(builder, condition_id, target_block_id, merge_block_id);
    vkd3d_spirv_build_op_label(builder, merge_block_id);
}

static void vkd3d_dxbc_compiler_emit_return(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    uint32_t void_id, function_id, arguments[MAX_REG_OUTPUT];
    unsigned int i, count;;

    if ((function_id = compiler->output_setup_function_id))
    {
        void_id = vkd3d_spirv_get_op_type_void(builder);
        for (i = 0, count = 0; i < ARRAY_SIZE(compiler->private_output_variable); ++i)
        {
            if (compiler->private_output_variable[i])
                arguments[count++] = compiler->private_output_variable[i];
        }

        vkd3d_spirv_build_op_function_call(builder, void_id, function_id, arguments, count);
    }

    vkd3d_spirv_build_op_return(builder);
}

static struct vkd3d_control_flow_info *vkd3d_dxbc_compiler_push_control_flow_level(
        struct vkd3d_dxbc_compiler *compiler)
{
    if (!vkd3d_array_reserve((void **)&compiler->control_flow_info, &compiler->control_flow_info_size,
            compiler->control_flow_depth + 1, sizeof(*compiler->control_flow_info)))
    {
        ERR("Failed to allocate control flow info structure.\n");
        return NULL;
    }

    return &compiler->control_flow_info[compiler->control_flow_depth++];
}

static void vkd3d_dxbc_compiler_pop_control_flow_level(struct vkd3d_dxbc_compiler *compiler)
{
    struct vkd3d_control_flow_info *cf_info;

    assert(compiler->control_flow_depth);

    cf_info = &compiler->control_flow_info[--compiler->control_flow_depth];
    memset(cf_info, 0, sizeof(*cf_info));
}

static struct vkd3d_control_flow_info *vkd3d_dxbc_compiler_find_innermost_loop(
        struct vkd3d_dxbc_compiler *compiler)
{
    int depth;

    for (depth = compiler->control_flow_depth - 1; depth >= 0; --depth)
    {
        if (compiler->control_flow_info[depth].current_block == VKD3D_BLOCK_LOOP)
            return &compiler->control_flow_info[depth];
    }

    return NULL;
}

static void vkd3d_dxbc_compiler_emit_control_flow_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t merge_block_id, val_id, condition_id, true_label, false_label;
    uint32_t loop_header_block_id, loop_body_block_id, continue_block_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_src_param *src = instruction->src;
    struct vkd3d_control_flow_info *cf_info;

    cf_info = compiler->control_flow_depth
            ? &compiler->control_flow_info[compiler->control_flow_depth - 1] : NULL;

    switch (instruction->handler_idx)
    {
        case VKD3DSIH_IF:
            if (!(cf_info = vkd3d_dxbc_compiler_push_control_flow_level(compiler)))
                return;

            val_id = vkd3d_dxbc_compiler_emit_load_reg(compiler,
                    &src->reg, src->swizzle, VKD3DSP_WRITEMASK_0);
            condition_id = vkd3d_dxbc_compiler_emit_int_to_bool(compiler, instruction->flags, 1, val_id);

            true_label = vkd3d_spirv_alloc_id(builder);
            false_label = vkd3d_spirv_alloc_id(builder);
            merge_block_id = vkd3d_spirv_alloc_id(builder);
            vkd3d_spirv_build_op_selection_merge(builder, merge_block_id, SpvSelectionControlMaskNone);
            vkd3d_spirv_build_op_branch_conditional(builder, condition_id, true_label, false_label);

            vkd3d_spirv_build_op_label(builder, true_label);

            cf_info->u.branch.merge_block_id = merge_block_id;
            cf_info->u.branch.else_block_id = false_label;
            cf_info->current_block = VKD3D_BLOCK_IF;

            vkd3d_spirv_build_op_name(builder, merge_block_id, "branch%u_merge", compiler->branch_id);
            vkd3d_spirv_build_op_name(builder, true_label, "branch%u_true", compiler->branch_id);
            vkd3d_spirv_build_op_name(builder, false_label, "branch%u_false", compiler->branch_id);
            ++compiler->branch_id;
            break;

        case VKD3DSIH_ELSE:
            assert(compiler->control_flow_depth);
            assert(cf_info->current_block != VKD3D_BLOCK_LOOP);

            if (cf_info->current_block == VKD3D_BLOCK_IF)
                vkd3d_spirv_build_op_branch(builder, cf_info->u.branch.merge_block_id);

            if (cf_info->current_block != VKD3D_BLOCK_ELSE)
                vkd3d_spirv_build_op_label(builder, cf_info->u.branch.else_block_id);
            cf_info->current_block = VKD3D_BLOCK_ELSE;
            break;

        case VKD3DSIH_ENDIF:
            assert(compiler->control_flow_depth);
            assert(cf_info->current_block != VKD3D_BLOCK_MAIN);
            assert(cf_info->current_block != VKD3D_BLOCK_LOOP);

            if (cf_info->current_block == VKD3D_BLOCK_IF)
            {
                vkd3d_spirv_build_op_branch(builder, cf_info->u.branch.merge_block_id);

                vkd3d_spirv_build_op_label(builder, cf_info->u.branch.else_block_id);
                vkd3d_spirv_build_op_branch(builder, cf_info->u.branch.merge_block_id);

            }
            else if (cf_info->current_block == VKD3D_BLOCK_ELSE)
            {
                vkd3d_spirv_build_op_branch(builder, cf_info->u.branch.merge_block_id);
            }

            vkd3d_spirv_build_op_label(builder, cf_info->u.branch.merge_block_id);

            vkd3d_dxbc_compiler_pop_control_flow_level(compiler);
            break;

        case VKD3DSIH_LOOP:
            if (!(cf_info = vkd3d_dxbc_compiler_push_control_flow_level(compiler)))
                return;

            loop_header_block_id = vkd3d_spirv_alloc_id(builder);
            loop_body_block_id = vkd3d_spirv_alloc_id(builder);
            continue_block_id = vkd3d_spirv_alloc_id(builder);
            merge_block_id = vkd3d_spirv_alloc_id(builder);

            vkd3d_spirv_build_op_branch(builder, loop_header_block_id);
            vkd3d_spirv_build_op_label(builder, loop_header_block_id);
            vkd3d_spirv_build_op_loop_merge(builder, merge_block_id, continue_block_id, SpvLoopControlMaskNone);
            vkd3d_spirv_build_op_branch(builder, loop_body_block_id);

            vkd3d_spirv_build_op_label(builder, loop_body_block_id);

            cf_info->u.loop.header_block_id = loop_header_block_id;
            cf_info->u.loop.continue_block_id = continue_block_id;
            cf_info->u.loop.merge_block_id = merge_block_id;
            cf_info->current_block = VKD3D_BLOCK_LOOP;

            vkd3d_spirv_build_op_name(builder, loop_header_block_id, "loop%u_header", compiler->loop_id);
            vkd3d_spirv_build_op_name(builder, loop_body_block_id, "loop%u_body", compiler->loop_id);
            vkd3d_spirv_build_op_name(builder, continue_block_id, "loop%u_continue", compiler->loop_id);
            vkd3d_spirv_build_op_name(builder, merge_block_id, "loop%u_merge", compiler->loop_id);
            ++compiler->loop_id;
            break;

        case VKD3DSIH_ENDLOOP:
            assert(compiler->control_flow_depth);
            assert(cf_info->current_block == VKD3D_BLOCK_LOOP);

            vkd3d_spirv_build_op_branch(builder, cf_info->u.loop.continue_block_id);

            vkd3d_spirv_build_op_label(builder, cf_info->u.loop.continue_block_id);
            vkd3d_spirv_build_op_branch(builder, cf_info->u.loop.header_block_id);
            vkd3d_spirv_build_op_label(builder, cf_info->u.loop.merge_block_id);

            vkd3d_dxbc_compiler_pop_control_flow_level(compiler);
            break;

        case VKD3DSIH_BREAK:
        {
            const struct vkd3d_control_flow_info *loop_cf_info;

            if (!(loop_cf_info = vkd3d_dxbc_compiler_find_innermost_loop(compiler)))
            {
                FIXME("Unhandled break instruction.\n");
                return;
            }

            assert(compiler->control_flow_depth);

            vkd3d_spirv_build_op_branch(builder, loop_cf_info->u.loop.merge_block_id);

            if (cf_info->current_block == VKD3D_BLOCK_IF)
            {
                vkd3d_spirv_build_op_label(builder, cf_info->u.branch.else_block_id);
                cf_info->current_block = VKD3D_BLOCK_ELSE;
            }
            else
            {
                cf_info->current_block = VKD3D_BLOCK_NONE;
            }
            break;
        }

        case VKD3DSIH_BREAKP:
            assert(compiler->control_flow_depth);
            assert(cf_info->current_block == VKD3D_BLOCK_LOOP);

            vkd3d_dxbc_compiler_emit_breakc(compiler, instruction, cf_info->u.loop.merge_block_id);
            break;

        case VKD3DSIH_RET:
            vkd3d_dxbc_compiler_emit_return(compiler, instruction);

            if (cf_info && cf_info->current_block == VKD3D_BLOCK_IF)
            {
                vkd3d_spirv_build_op_label(builder, cf_info->u.branch.else_block_id);
                cf_info->current_block = VKD3D_BLOCK_ELSE;
            }
            else if (cf_info)
            {
                cf_info->current_block = VKD3D_BLOCK_NONE;
            }
            break;

        default:
            ERR("Unexpected instruction %#x.\n", instruction->handler_idx);
            break;
    }
}

static uint32_t vkd3d_dxbc_compiler_prepare_image(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *resource_reg, enum vkd3d_component_type *sampled_type,
        DWORD *coordinate_mask)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_symbol *resource_symbol;
    struct vkd3d_symbol resource_key;
    struct rb_entry *entry;
    uint32_t image_id;

    vkd3d_symbol_make_resource(&resource_key, resource_reg);
    entry = rb_get(&compiler->symbol_table, &resource_key);
    assert(entry);
    resource_symbol = RB_ENTRY_VALUE(entry, struct vkd3d_symbol, entry);

    image_id = vkd3d_spirv_build_op_load(builder,
            resource_symbol->info.resource.type_id, resource_symbol->id, SpvMemoryAccessMaskNone);

    *sampled_type = resource_symbol->info.resource.sampled_type;
    *coordinate_mask = resource_symbol->info.resource.coordinate_mask;
    return image_id;
}

static uint32_t vkd3d_dxbc_compiler_prepare_sampled_image(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_register *resource_reg, const struct vkd3d_shader_register *sampler_reg,
        enum vkd3d_component_type *sampled_type)
{
    uint32_t image_id, sampler_var_id, sampler_id, sampled_image_type_id, sampled_image_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_symbol *resource_symbol;
    struct vkd3d_symbol resource_key;
    struct rb_entry *entry;

    vkd3d_symbol_make_resource(&resource_key, resource_reg);
    entry = rb_get(&compiler->symbol_table, &resource_key);
    assert(entry);
    resource_symbol = RB_ENTRY_VALUE(entry, struct vkd3d_symbol, entry);

    image_id = vkd3d_spirv_build_op_load(builder,
            resource_symbol->info.resource.type_id, resource_symbol->id, SpvMemoryAccessMaskNone);
    sampler_var_id = vkd3d_dxbc_compiler_get_register_id(compiler, sampler_reg);
    sampler_id = vkd3d_spirv_build_op_load(builder,
            vkd3d_spirv_get_op_type_sampler(builder), sampler_var_id, SpvMemoryAccessMaskNone);

    sampled_image_type_id = vkd3d_spirv_get_op_type_sampled_image(builder,
            resource_symbol->info.resource.type_id);
    sampled_image_id = vkd3d_spirv_build_op_sampled_image(builder,
            sampled_image_type_id, image_id, sampler_id);

    *sampled_type = resource_symbol->info.resource.sampled_type;
    return sampled_image_id;
}

static void vkd3d_dxbc_compiler_emit_sample(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    uint32_t sampled_image_id, sampled_type_id, coordinate_id, val_id;
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_src_param *src = instruction->src;
    struct vkd3d_shader_dst_param dst = *instruction->dst;
    enum vkd3d_component_type sampled_type;

    if (vkd3d_shader_instruction_has_texel_offset(instruction))
        FIXME("Texel offset not supported.\n");

    sampled_image_id = vkd3d_dxbc_compiler_prepare_sampled_image(compiler,
            &src[1].reg, &src[2].reg, &sampled_type);
    sampled_type_id = vkd3d_spirv_get_type_id(builder, sampled_type, VKD3D_VEC4_SIZE);
    coordinate_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], VKD3DSP_WRITEMASK_ALL);
    val_id = vkd3d_spirv_build_op_image_sample_implicit_lod(builder, sampled_type_id,
            sampled_image_id, coordinate_id, SpvImageOperandsMaskNone, NULL, 0);

    val_id = vkd3d_dxbc_compiler_emit_swizzle(compiler, val_id, src[1].swizzle, dst.write_mask);
    /* XXX: Fix the result data type. */
    dst.reg.data_type = vkd3d_data_type_from_component_type(sampled_type);
    vkd3d_dxbc_compiler_emit_store_dst(compiler, &dst, val_id);
}

static void vkd3d_dxbc_compiler_emit_store_uav_typed(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    const struct vkd3d_shader_dst_param *dst = instruction->dst;
    const struct vkd3d_shader_src_param *src = instruction->src;
    struct vkd3d_shader_src_param texel_param = src[1];
    uint32_t image_id, coordinate_id, texel_id;
    enum vkd3d_component_type sampled_type;
    DWORD coordinate_mask;

    vkd3d_spirv_enable_capability(builder, SpvCapabilityStorageImageWriteWithoutFormat);

    image_id = vkd3d_dxbc_compiler_prepare_image(compiler, &dst->reg, &sampled_type, &coordinate_mask);
    coordinate_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &src[0], coordinate_mask);
    /* XXX: Fix the data type. */
    texel_param.reg.data_type = vkd3d_data_type_from_component_type(sampled_type);
    texel_id = vkd3d_dxbc_compiler_emit_load_src(compiler, &texel_param, dst->write_mask);

    vkd3d_spirv_build_op_image_write(builder, image_id, coordinate_id, texel_id,
            SpvImageOperandsMaskNone, NULL, 0);
}

/* This function is called after declarations are processed. */
static void vkd3d_dxbc_compiler_emit_main_prolog(struct vkd3d_dxbc_compiler *compiler)
{
    vkd3d_dxbc_compiler_emit_push_constants(compiler);
}

static bool is_dcl_instruction(enum VKD3D_SHADER_INSTRUCTION_HANDLER handler_idx)
{
    return VKD3DSIH_DCL <= handler_idx && handler_idx <= VKD3DSIH_DCL_VERTICES_OUT;
}

void vkd3d_dxbc_compiler_handle_instruction(struct vkd3d_dxbc_compiler *compiler,
        const struct vkd3d_shader_instruction *instruction)
{
    if (!is_dcl_instruction(instruction->handler_idx) && !compiler->after_declarations_section)
    {
        compiler->after_declarations_section = true;
        vkd3d_dxbc_compiler_emit_main_prolog(compiler);
    }

    switch (instruction->handler_idx)
    {
        case VKD3DSIH_DCL_GLOBAL_FLAGS:
            vkd3d_dxbc_compiler_emit_dcl_global_flags(compiler, instruction);
            break;
        case VKD3DSIH_DCL_TEMPS:
            vkd3d_dxbc_compiler_emit_dcl_temps(compiler, instruction);
            break;
        case VKD3DSIH_DCL_CONSTANT_BUFFER:
            vkd3d_dxbc_compiler_emit_dcl_constant_buffer(compiler, instruction);
            break;
        case VKD3DSIH_DCL_IMMEDIATE_CONSTANT_BUFFER:
            vkd3d_dxbc_compiler_emit_dcl_immediate_constant_buffer(compiler, instruction);
            break;
        case VKD3DSIH_DCL_SAMPLER:
            vkd3d_dxbc_compiler_emit_dcl_sampler(compiler, instruction);
            break;
        case VKD3DSIH_DCL:
            vkd3d_dxbc_compiler_emit_dcl_resource(compiler, instruction);
            break;
        case VKD3DSIH_DCL_UAV_TYPED:
            vkd3d_dxbc_compiler_emit_dcl_uav_typed(compiler, instruction);
            break;
        case VKD3DSIH_DCL_INPUT:
            vkd3d_dxbc_compiler_emit_dcl_input(compiler, instruction);
            break;
        case VKD3DSIH_DCL_INPUT_PS:
            vkd3d_dxbc_compiler_emit_dcl_input_ps(compiler, instruction);
            break;
        case VKD3DSIH_DCL_INPUT_PS_SIV:
            vkd3d_dxbc_compiler_emit_dcl_input_ps_siv(compiler, instruction);
            break;
        case VKD3DSIH_DCL_INPUT_SGV:
            vkd3d_dxbc_compiler_emit_dcl_input_sgv(compiler, instruction);
            break;
        case VKD3DSIH_DCL_OUTPUT:
            vkd3d_dxbc_compiler_emit_dcl_output(compiler, instruction);
            break;
        case VKD3DSIH_DCL_OUTPUT_SIV:
            vkd3d_dxbc_compiler_emit_dcl_output_siv(compiler, instruction);
            break;
        case VKD3DSIH_DCL_THREAD_GROUP:
            vkd3d_dxbc_compiler_emit_dcl_thread_group(compiler, instruction);
            break;
        case VKD3DSIH_MOV:
            vkd3d_dxbc_compiler_emit_mov(compiler, instruction);
            break;
        case VKD3DSIH_MOVC:
            vkd3d_dxbc_compiler_emit_movc(compiler, instruction);
            break;
        case VKD3DSIH_SWAPC:
            vkd3d_dxbc_compiler_emit_swapc(compiler, instruction);
            break;
        case VKD3DSIH_ADD:
        case VKD3DSIH_AND:
        case VKD3DSIH_BFREV:
        case VKD3DSIH_COUNTBITS:
        case VKD3DSIH_DIV:
        case VKD3DSIH_FTOI:
        case VKD3DSIH_FTOU:
        case VKD3DSIH_IADD:
        case VKD3DSIH_ISHL:
        case VKD3DSIH_ISHR:
        case VKD3DSIH_ITOF:
        case VKD3DSIH_MUL:
        case VKD3DSIH_NOT:
        case VKD3DSIH_OR:
        case VKD3DSIH_USHR:
        case VKD3DSIH_UTOF:
        case VKD3DSIH_XOR:
            vkd3d_dxbc_compiler_emit_alu_instruction(compiler, instruction);
            break;
        case VKD3DSIH_EXP:
        case VKD3DSIH_FIRSTBIT_HI:
        case VKD3DSIH_FIRSTBIT_LO:
        case VKD3DSIH_FIRSTBIT_SHI:
        case VKD3DSIH_FRC:
        case VKD3DSIH_IMAX:
        case VKD3DSIH_IMIN:
        case VKD3DSIH_LOG:
        case VKD3DSIH_MAD:
        case VKD3DSIH_MAX:
        case VKD3DSIH_MIN:
        case VKD3DSIH_ROUND_NI:
        case VKD3DSIH_ROUND_PI:
        case VKD3DSIH_RSQ:
        case VKD3DSIH_SQRT:
        case VKD3DSIH_UMAX:
        case VKD3DSIH_UMIN:
            vkd3d_dxbc_compiler_emit_ext_glsl_instruction(compiler, instruction);
            break;
        case VKD3DSIH_DP4:
        case VKD3DSIH_DP3:
        case VKD3DSIH_DP2:
            vkd3d_dxbc_compiler_emit_dot(compiler, instruction);
            break;
        case VKD3DSIH_RCP:
            vkd3d_dxbc_compiler_emit_rcp(compiler, instruction);
            break;
        case VKD3DSIH_IMUL:
            vkd3d_dxbc_compiler_emit_imul(compiler, instruction);
            break;
        case VKD3DSIH_IMAD:
            vkd3d_dxbc_compiler_emit_imad(compiler, instruction);
            break;
        case VKD3DSIH_UDIV:
            vkd3d_dxbc_compiler_emit_udiv(compiler, instruction);
            break;
        case VKD3DSIH_EQ:
        case VKD3DSIH_GE:
        case VKD3DSIH_IEQ:
        case VKD3DSIH_IGE:
        case VKD3DSIH_ILT:
        case VKD3DSIH_INE:
        case VKD3DSIH_LT:
        case VKD3DSIH_NE:
        case VKD3DSIH_UGE:
        case VKD3DSIH_ULT:
            vkd3d_dxbc_compiler_emit_comparison_instruction(compiler, instruction);
            break;
        case VKD3DSIH_BFI:
        case VKD3DSIH_IBFE:
        case VKD3DSIH_UBFE:
            vkd3d_dxbc_compiler_emit_bitfield_instruction(compiler, instruction);
            break;
        case VKD3DSIH_F16TOF32:
            vkd3d_dxbc_compiler_emit_f16tof32(compiler, instruction);
            break;
        case VKD3DSIH_F32TOF16:
            vkd3d_dxbc_compiler_emit_f32tof16(compiler, instruction);
            break;
        case VKD3DSIH_BREAK:
        case VKD3DSIH_BREAKP:
        case VKD3DSIH_ELSE:
        case VKD3DSIH_ENDIF:
        case VKD3DSIH_ENDLOOP:
        case VKD3DSIH_IF:
        case VKD3DSIH_LOOP:
        case VKD3DSIH_RET:
            vkd3d_dxbc_compiler_emit_control_flow_instruction(compiler, instruction);
            break;
        case VKD3DSIH_SAMPLE:
            vkd3d_dxbc_compiler_emit_sample(compiler, instruction);
            break;
        case VKD3DSIH_STORE_UAV_TYPED:
            vkd3d_dxbc_compiler_emit_store_uav_typed(compiler, instruction);
            break;
        default:
            FIXME("Unhandled instruction %#x.\n", instruction->handler_idx);
    }
}

static void vkd3d_dxbc_compiler_emit_output_setup_function(struct vkd3d_dxbc_compiler *compiler)
{
    uint32_t void_id, type_id, ptr_type_id, function_type_id, function_id, val_id;
    const struct vkd3d_shader_signature *signature = compiler->output_signature;
    uint32_t param_type_id[MAX_REG_OUTPUT], param_id[MAX_REG_OUTPUT] = {};
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;
    DWORD write_mask, reg_idx;
    unsigned int i, count;

    function_id = compiler->output_setup_function_id;

    void_id = vkd3d_spirv_get_op_type_void(builder);
    type_id = vkd3d_spirv_get_type_id(builder, VKD3D_TYPE_FLOAT, 4);
    ptr_type_id = vkd3d_spirv_get_op_type_pointer(builder, SpvStorageClassPrivate, type_id);
    for (i = 0, count = 0; i < ARRAY_SIZE(compiler->private_output_variable); ++i)
    {
        if (compiler->private_output_variable[i])
            param_type_id[count++] = ptr_type_id;
    }
    function_type_id = vkd3d_spirv_build_op_type_function(builder, void_id, param_type_id, count);

    vkd3d_spirv_build_op_function(builder, void_id, function_id,
            SpvFunctionControlMaskNone, function_type_id);
    vkd3d_spirv_build_op_name(builder, function_id, "setup_output");

    for (i = 0; i < ARRAY_SIZE(compiler->private_output_variable); ++i)
    {
        if (compiler->private_output_variable[i])
            param_id[i] = vkd3d_spirv_build_op_function_parameter(builder, ptr_type_id);
    }

    vkd3d_spirv_build_op_label(builder, vkd3d_spirv_alloc_id(builder));

    for (i = 0; i < ARRAY_SIZE(compiler->private_output_variable); ++i)
    {
        if (compiler->private_output_variable[i])
            param_id[i] = vkd3d_spirv_build_op_load(builder, type_id, param_id[i], SpvMemoryAccessMaskNone);
    }

    for (i = 0; i < signature->element_count; ++i)
    {
        reg_idx = signature->elements[i].register_idx;
        write_mask = signature->elements[i].mask & 0xff;

        if (!param_id[reg_idx])
            continue;

        val_id = vkd3d_dxbc_compiler_emit_swizzle(compiler, param_id[reg_idx], VKD3DSP_NOSWIZZLE, write_mask);

        if (compiler->output_info[i].component_type != VKD3D_TYPE_FLOAT)
        {
            type_id = vkd3d_spirv_get_type_id(builder, compiler->output_info[i].component_type,
                    vkd3d_write_mask_component_count(write_mask));
            val_id = vkd3d_spirv_build_op_bitcast(builder, type_id, val_id);
        }

        vkd3d_spirv_build_op_store(builder, compiler->output_info[i].id, val_id, SpvMemoryAccessMaskNone);
    }

    vkd3d_spirv_build_op_return(&compiler->spirv_builder);
    vkd3d_spirv_build_op_function_end(builder);
}

bool vkd3d_dxbc_compiler_generate_spirv(struct vkd3d_dxbc_compiler *compiler,
        struct vkd3d_shader_code *spirv)
{
    struct vkd3d_spirv_builder *builder = &compiler->spirv_builder;

    if (compiler->options & VKD3D_SHADER_STRIP_DEBUG)
        vkd3d_spirv_stream_clear(&builder->debug_stream);

    vkd3d_spirv_build_op_function_end(builder);

    if (compiler->output_setup_function_id)
        vkd3d_dxbc_compiler_emit_output_setup_function(compiler);

    if (!vkd3d_spirv_compile_module(builder, spirv))
        return false;

    if (TRACE_ON())
    {
        vkd3d_spirv_dump(spirv);
        vkd3d_spirv_validate(spirv);
    }

    return true;
}

void vkd3d_dxbc_compiler_destroy(struct vkd3d_dxbc_compiler *compiler)
{
    vkd3d_free(compiler->control_flow_info);

    vkd3d_free(compiler->output_info);

    vkd3d_spirv_builder_free(&compiler->spirv_builder);

    rb_destroy(&compiler->symbol_table, vkd3d_symbol_free, NULL);

    vkd3d_free(compiler);
}
