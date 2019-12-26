#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "compiler.h"
#include "object.h"
#include "memory.h"

static au3VM firstVM;
static au3VM *g_pVM = NULL;

static void resetStack(au3VM *vm)
{
    vm->top = vm->stack;
    vm->frameCount = 0;
}

static void runtimeError(au3VM *vm, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm->frameCount - 1; i >= 0; i--) {
        au3CallFrame *frame = &vm->frames[i];
        au3Function *function = frame->function;
        // -1 because the IP is sitting on the next instruction to be
        // executed.                                                 
        size_t instruction = (size_t)frame->ip - (size_t)function->chunk.code - 1;
        int line = function->chunk.lines[instruction];
        int column = function->chunk.columns[instruction];

        fprintf(stderr, "[%d:%d] in ", line, column);

        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        }
        else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    resetStack(vm);
}

au3VM *au3_create()
{
    au3VM *vm;
    
    if (g_pVM == NULL) {
        vm = &firstVM;
        g_pVM = vm;
    }
    else {
        vm = calloc(sizeof(au3VM), 1);
        if (vm == NULL) return NULL;
    }
    
    vm->objects = NULL;
    au3_initTable(&vm->globals);
    au3_initTable(&vm->strings);

    resetStack(vm);
    return vm;
}

void au3_close(au3VM *vm)
{
    if (vm != NULL && g_pVM != NULL) {
        vm = g_pVM;
    }
    
    au3_freeTable(&vm->globals);
    au3_freeTable(&vm->strings);
    au3_freeObjects(vm);

    if (vm != g_pVM) free(vm);
    else g_pVM = NULL;
}

#define PUSH(vm, v)     *((vm)->top++) = (v)
#define POP(vm)         *(--(vm)->top)
#define PEEK(vm, i)     ((vm)->top[-1 - (i)])

static void concatenate(au3VM *vm)
{
    au3String *b = AU3_AS_STRING(POP(vm));
    au3String *a = AU3_AS_STRING(POP(vm));

    int length = a->length + b->length;
    char *chars = malloc(sizeof(char) * (length + 1));
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    au3String *result = au3_takeString(vm, chars, length);
    PUSH(vm, AU3_OBJECT(result));
}

static bool call(au3VM *vm, au3Function *function, int argCount)
{
    if (argCount != function->arity) {
        runtimeError(vm, "Expected %d arguments but got %d.",
            function->arity, argCount);
        return false;
    }

    if (vm->frameCount == AU3_MAX_FRAMES) {
        runtimeError(vm, "Stack overflow.");
        return false;
    }

    au3CallFrame *frame = &vm->frames[vm->frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;

    frame->slots = vm->top - argCount - 1;
    return true;
}

static bool callValue(au3VM *vm, au3Value callee, int argCount)
{
    if (AU3_IS_OBJECT(callee)) {
        switch (AU3_OBJECT_TYPE(callee)) {
            case AU3_TFUNCTION:
                return call(vm, AU3_AS_FUNCTION(callee), argCount);

            case AU3_TNATIVE: {
                au3NativeFn native = AU3_AS_NATIVE(callee);
                au3Value result = native(vm, argCount, vm->top - argCount);
                vm->top -= argCount + 1;
                PUSH(vm, result);
                return true;
            }

            default:
                // Non-callable object type.                   
                break;
            }
    }

    runtimeError(vm, "Can only call functions and classes.");
    return false;
}

static au3Status execute(au3VM *vm)
{
    register uint8_t *ip;
    register au3CallFrame *frame;

#define STORE_FRAME() \
	frame->ip = ip

#define LOAD_FRAME() \
	frame = &vm->frames[vm->frameCount - 1]; \
	ip = frame->ip

#define ERROR(fmt, ...) \
    do { \
        STORE_FRAME(); \
        runtimeError(vm, fmt, ##__VA_ARGS__); \
        return AU3_RUNTIME_ERROR; \
    } while (false)

#define READ_BYTE()     (*ip++)
#define READ_SHORT()    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_LAST()     (ip[-1])

#define READ_CONST()    (frame->function->chunk.constants.values[READ_BYTE()])
#define READ_STRING()   AU3_AS_STRING(READ_CONST())

#define BINARY_OP(valueType, op) \
    do { \
        if (!AU3_IS_INTEGER(PEEK(vm, 0)) || !AU3_IS_INTEGER(PEEK(vm, 1))) { \
            ERROR("Operands must be numbers."); \
        } \
        int64_t b = AU3_AS_INTEGER(POP(vm)); \
        int64_t a = AU3_AS_INTEGER(POP(vm)); \
        PUSH(vm, valueType(a op b)); \
    } while (false)

#define DISPATCH()      for (;;) switch (READ_BYTE())
#define CASE_CODE(x)    case OP_##x:
#define CASE_ERROR()    default:
#define NEXT            continue

    LOAD_FRAME();

    DISPATCH() {

        CASE_CODE(NOP) NEXT;

        CASE_CODE(PUTS) {
            int count = READ_BYTE();
            for (int i = count - 1; i >= 0; i--) {
                au3_printValue(PEEK(vm, i));
                if (i != 0) printf("\t");
            } 
            printf("\n");
            NEXT;
        }
        CASE_CODE(POP) {
            POP(vm);
            NEXT;
        }

        CASE_CODE(RET) {
            au3Value result = POP(vm);

            if (--vm->frameCount == 0) {
                POP(vm);
                return AU3_OK;
            }

            vm->top = frame->slots;
            PUSH(vm, result);

            LOAD_FRAME();
            NEXT;
        }
        CASE_CODE(CALL) {
            int argCount = READ_BYTE();

            STORE_FRAME();
            if (!callValue(vm, PEEK(vm, argCount), argCount)) {
                return AU3_RUNTIME_ERROR;
            }

            LOAD_FRAME();
            NEXT;
        }

        CASE_CODE(NEG) {
            if (!AU3_IS_INTEGER(PEEK(vm, 0))) {
                ERROR("Operand must be a number.");
            }

            PUSH(vm, AU3_INTEGER(-AU3_AS_INTEGER(POP(vm))));
            NEXT;
        }
        CASE_CODE(ADD) {
            if (AU3_IS_STRING(PEEK(vm, 0)) && AU3_IS_STRING(PEEK(vm, 1))) {
                concatenate(vm);
            }
            else if (AU3_IS_INTEGER(PEEK(vm, 0)) && AU3_IS_INTEGER(PEEK(vm, 1))) {
                int64_t b = AU3_AS_INTEGER(POP(vm));
                int64_t a = AU3_AS_INTEGER(POP(vm));
                PUSH(vm, AU3_INTEGER(a + b));
            }
            else {
                ERROR("Operands must be two numbers or two strings.");
            }
            NEXT;
        }
        CASE_CODE(SUB) {
            BINARY_OP(AU3_INTEGER, - );
            NEXT;
        }
        CASE_CODE(MUL) {
            BINARY_OP(AU3_INTEGER, * );
            NEXT;
        }
        CASE_CODE(DIV) {
            BINARY_OP(AU3_INTEGER, / );
            NEXT;
        }

        CASE_CODE(NOT) {
            PUSH(vm, AU3_BOOL(AU3_IS_FALSEY(POP(vm))));
            NEXT;
        }
        CASE_CODE(EQ) {
            au3Value b = POP(vm);
            au3Value a = POP(vm);
            PUSH(vm, AU3_BOOL(au3_valuesEqual(a, b)));
            NEXT;
        }
        CASE_CODE(LT) {
            BINARY_OP(AU3_BOOL, < );
            NEXT;
        }
        CASE_CODE(LE) {
            BINARY_OP(AU3_BOOL, <= );
            NEXT;
        }

        CASE_CODE(NULL) {
            PUSH(vm, AU3_NULL);
            NEXT;
        }
        CASE_CODE(TRUE) {
            PUSH(vm, AU3_TRUE);
            NEXT;
        }
        CASE_CODE(FALSE) {
            PUSH(vm, AU3_FALSE);
            NEXT;
        }
        CASE_CODE(CONST) {
            au3Value value = READ_CONST();
            PUSH(vm, value);
            NEXT;
        }

        CASE_CODE(DEF) {
            au3String *name = READ_STRING();
            au3_tableSet(&vm->globals, name, PEEK(vm, 0));
            POP(vm);
            NEXT;
        }
        CASE_CODE(GLD) {
            au3String *name = READ_STRING();
            au3Value value = AU3_NULL;
            au3_tableGet(&vm->globals, name, &value);
            PUSH(vm, value);
            NEXT;
        }
        CASE_CODE(GST) {
            au3String *name = READ_STRING();
            au3_tableSet(&vm->globals, name, PEEK(vm, 0));
            NEXT;
        }

        CASE_CODE(SELF) {
            PUSH(vm, frame->slots[0]);
            NEXT;
        }
        CASE_CODE(LD) {
            uint8_t slot = READ_BYTE();
            PUSH(vm, frame->slots[slot]);
            NEXT;
        }
        CASE_CODE(ST) {
            uint8_t slot = READ_BYTE();
            frame->slots[slot] = PEEK(vm, 0);
            NEXT;
        }

        CASE_CODE(JMP) {
            uint16_t offset = READ_SHORT();
            ip += offset;
            NEXT;
        }
        CASE_CODE(JMPF) {
            uint16_t offset = READ_SHORT();
            if (AU3_IS_FALSEY(PEEK(vm, 0))) ip += offset;
            NEXT;
        }
        CASE_CODE(LOOP) {
            uint16_t offset = READ_SHORT();
            ip -= offset;
            NEXT;
        }

        CASE_ERROR() {
            ERROR("Bad opcode, got %d!", READ_LAST());
        }
    }

    return AU3_OK;
}

au3Status au3_interpret(au3VM *vm, const char *source)
{
    au3Function *function = au3_compile(vm, source);
    if (function == NULL) return AU3_COMPILE_ERROR;

    PUSH(vm, AU3_OBJECT(function));
    callValue(vm, AU3_OBJECT(function), 0);

   return execute(vm);
}

void au3_defineNative(au3VM *vm, const char *name, au3NativeFn function, const char *tips)
{
    au3Value g_name = AU3_OBJECT(au3_copyString(vm, name, (int)strlen(name)));
    au3Value native = AU3_OBJECT(au3_newNative(vm, function, tips));
    PUSH(vm, g_name); PUSH(vm, native);

    au3_tableSet(&vm->globals, AU3_AS_STRING(g_name), native);
    POP(vm); POP(vm);
}

void au3_setGlobal(au3VM *vm, const char *name, au3Value value)
{
    au3Value g_name = AU3_OBJECT(au3_copyString(vm, name, (int)strlen(name)));
    PUSH(vm, g_name); PUSH(vm, value);

    au3_tableSet(&vm->globals, AU3_AS_STRING(g_name), value);
    POP(vm); POP(vm);
}

au3Value au3_getGlobal(au3VM *vm, const char *name)
{
    au3Value g_name = AU3_OBJECT(au3_copyString(vm, name, (int)strlen(name)));
    au3Value value;

    au3_tableGet(&vm->globals, AU3_AS_STRING(g_name), &value);
    POP(vm);

    return value;
}
